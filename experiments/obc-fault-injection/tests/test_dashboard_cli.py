"""The dashboard must load the explicitly supplied ELF, including relative paths."""
import importlib.util
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.request import Request, urlopen

EXPERIMENT = Path(__file__).resolve().parents[1]
ROOT = EXPERIMENT.parents[2]
SCRIPT = EXPERIMENT / 'examples/dashboard.py'


class DashboardCliTest(unittest.TestCase):
    def env(self):
        return {**os.environ, 'PYTHONPATH': str(EXPERIMENT)}

    def test_firmware_is_required(self):
        result = subprocess.run([sys.executable, '-S', str(SCRIPT)],
                                env=self.env(), capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('--firmware', result.stderr)

    def test_missing_firmware_is_rejected_before_start(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, '-S', str(SCRIPT),
                                     '--firmware','missing.out'], cwd=directory,
                                    env=self.env(), capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn(f'firmware file not found: {directory}/missing.out', result.stderr)

    @unittest.skipUnless(importlib.util.find_spec('fastapi') and importlib.util.find_spec('uvicorn'),
                         'Optional web dependencies not installed')
    def test_custom_relative_firmware_is_loaded_by_qemu_and_served(self):
        with tempfile.TemporaryDirectory() as directory:
            firmware = Path(directory) / 'custom firmware.out'
            shutil.copyfile(ROOT/'obc/build/TMS570LC43_OBC_PLUS_20240102.out',firmware)
            process = subprocess.Popen([sys.executable,'-u',str(SCRIPT),'--web',
                                        '--firmware',firmware.name], cwd=directory,
                                       env=self.env(), stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            lines = queue.Queue()
            reader = threading.Thread(target=lambda: [lines.put(s) for s in process.stdout],daemon=True)
            reader.start()
            try:
                self.assertEqual(lines.get(timeout=15).strip(), f'Firmware: {firmware}')
                url = lines.get(timeout=15).strip().removeprefix('Dashboard: ')
                self.assertTrue(url.startswith('http://127.0.0.1:'),url)
                # Verify the actual spawned QEMU -kernel, not only printed text.
                children = Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()
                kernels=[]
                for child in children:
                    args=Path(f'/proc/{child}/cmdline').read_bytes().split(b'\0')
                    if b'-kernel' in args:
                        kernels.append(args[args.index(b'-kernel')+1].decode())
                self.assertEqual(kernels,[str(firmware)])
                with urlopen(url,timeout=5) as r:
                    token=re.search(r'name="obc-token" content="([^"]+)"',r.read().decode())[1]
                request=Request(url+'/api/control',
                                data=json.dumps({'action':'command','line':'RTC_DATE'}).encode(),
                                headers={'X-OBC-Token':token,'Content-Type':'application/json'})
                with urlopen(request,timeout=15) as r:
                    self.assertIn('RTC Time:',json.load(r)['output'])
            finally:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=20)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                    reader.join(timeout=2)
                    process.stdout.close()
            self.assertEqual(process.returncode,0)
