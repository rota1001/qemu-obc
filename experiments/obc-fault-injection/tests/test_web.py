"""Real QEMU tests for the optional, FastAPI dashboard."""
import importlib.util
import json
import re
import socket
import subprocess
import sys
import unittest
import warnings
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError
import obcfi

ROOT = Path(__file__).resolve().parents[4]


@unittest.skipUnless(importlib.util.find_spec('fastapi') and importlib.util.find_spec('uvicorn'), 'Optional web dependencies not installed')
class WebTest(unittest.TestCase):
    def launch(self, **options):
        obc = obcfi.Obc(ROOT/'obc/build/TMS570LC43_OBC_PLUS_20240102.out',
                        ROOT/'qemu/build/qemu-system-arm', **options).start()
        self.addCleanup(obc.stop)
        obc.wait_for_boot()
        return obc

    def request(self, obc, path, data=None, token=None):
        headers = {'X-OBC-Token': token or self.token}
        if data is not None:
            headers['Content-Type'] = 'application/json'
        with urlopen(Request(obc.web_url+path,
                             data=json.dumps(data).encode() if data is not None else None,
                             headers=headers), timeout=15) as response:
            return json.load(response)

    def test_live_controls_console_validation_and_cleanup(self):
        obc = self.launch(web=True)
        with urlopen(obc.web_url) as response:
            html = response.read().decode()
        self.token = re.search(r'name="obc-token" content="([^"]+)"', html)[1]
        self.assertIn('/assets/', html)
        state = self.request(obc, '/api/state')
        self.assertIn('[BOOT]', state['output'])
        self.request(obc, '/api/control', {'action':'battery', 'value':12})
        self.assertIn('00002ee0', obc.command('EPS_ANALOG_HK_DATA'))
        self.request(obc, '/api/control', {'action':'fault','name':'eps_checksum','enabled':True})
        self.assertIn('checksum mismatch', obc.command('EPS_CONFIG'))
        self.request(obc, '/api/control', {'action':'fault','name':'eps_checksum','enabled':False})
        self.request(obc, '/api/control', {'action':'shift', 'value':86400})
        self.request(obc, '/api/control', {'action':'command', 'line':'RTC_DATE'})
        self.assertIn('2026-01-02', self.request(obc, '/api/state')['output'])
        result = self.request(obc, '/api/control', {'action':'input','name':'obc_1v2','value':1.0})
        self.assertEqual(result['raw'], 1190)
        self.assertAlmostEqual(obc.wait_for_adc_value(1, 1.0), 1, delta=0.02)
        for data in [{'action':'input','name':'bad','value':1},
                     {'action':'battery','value':-1},
                     {'action':'command','line':'date\rreset'}]:
            with self.assertRaises(HTTPError) as err:
                self.request(obc, '/api/control', data)
            self.assertEqual(err.exception.code,400)
        with self.assertRaises(HTTPError) as err:
            self.request(obc, '/api/control', {'action':'shift','value':0}, token='wrong')
        self.assertEqual(err.exception.code,403)
        self.assertIn('RTC Time:', obc.command('RTC_DATE'))
        url = obc.web_url
        obc.stop()
        with self.assertRaises(OSError):
            urlopen(url, timeout=1)

    def test_headless_import_and_unavailable_port(self):
        subprocess.run([sys.executable, '-S', '-c',
                        "import obcfi,sys; assert '_obcfi_web' not in sys.modules"], check=True)
        with socket.socket() as occupied:
            occupied.bind(('127.0.0.1',0))
            occupied.listen()
            with warnings.catch_warnings(record=True) as caught:
                obc = self.launch(web=True, web_port=occupied.getsockname()[1])
            self.assertTrue(caught)
            self.assertIsNone(obc.web_url)
            self.assertTrue(obc.web_error)
            self.assertIn('RTC Time:', obc.command('RTC_DATE'))
