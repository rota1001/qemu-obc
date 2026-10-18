"""Public entry point: QEMU lifecycle and access to OBC peripherals.

Use ``import obcfi`` with PYTHONPATH pointing to this directory; no installation.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import time
import warnings

from _obcfi_console import Console
from _obcfi_qmp import Qmp
from _obcfi_system import System, Result
from _obcfi_eps import Eps
from _obcfi_rtc import Rtc
from _obcfi_adcs import Adcs

__all__ = ['Obc', 'System', 'Result']


class Obc:
    """One real firmware running in QEMU.

    firmware: BE-32 ELF path. qemu: OBC100-enabled executable path.
    sd_image: optional raw SD image (guest writes modify it).
    rtc_base: initial RTC calendar time, with registers interpreted by firmware
        as local time; use a fixed value for reproducible tests.
    """
    def __init__(self, firmware, qemu, *, sd_image=None,
                 rtc_base='2026-01-01T00:00:00', web=False, web_port=0):
        self.firmware = Path(firmware).resolve()
        self.qemu = Path(qemu).resolve()
        self.sd_image = Path(sd_image).resolve() if sd_image else None
        self.rtc_base = rtc_base
        self.web_enabled = web
        self.web_port = web_port
        self.web_url = None
        self.web_error = None
        self._dashboard = None
        self.system = System(self._set_adc)
        self.eps = self.rtc = None  # Connected by start().
        self.adcs = None
        self.console = None
        self._qmp = self._process = self._tmp = None

    @property
    def running(self):
        """Whether the QEMU process is alive (CPU may still be paused)."""
        return self._process is not None and self._process.poll() is None

    @property
    def returncode(self):
        return self._process.poll() if self._process else None

    @property
    def output(self):
        """UART transcript, continually updated by the background reader."""
        return self.console.output if self.console else ''

    def status(self):
        """QEMU run state, e.g. running or paused, independently of UART."""
        return self._qmp.call('query-status')

    def start(self):
        """Connect control channels and initialize inputs before executing firmware."""
        if self.running:
            return self
        self.stop()
        if not self.firmware.is_file() or not self.qemu.is_file():
            raise FileNotFoundError('firmware and qemu paths must exist')
        self._tmp = tempfile.TemporaryDirectory(prefix='obcfi-')
        path = Path(self._tmp.name) / 'qmp.sock'
        adcs_path = Path(self._tmp.name) / 'adcs.sock'
        args = [str(self.qemu), '-M', 'obc100', '-kernel', str(self.firmware),
                '-S', '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
                '-rtc', f'base={self.rtc_base},clock=vm',
                '-qmp', f'unix:{path},server=on,wait=off',
                '-chardev', f'socket,id=obc-adcs,path={adcs_path},server=on,wait=off',
                '-serial', 'chardev:obc-adcs']
        if self.sd_image:
            args += ['-drive', f'if=sd,format=raw,file={self.sd_image}']
        try:
            self._process = subprocess.Popen(args, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT)
            self.console = Console(self._process)
            deadline = time.monotonic() + 10
            while not path.exists():
                if not self.running or time.monotonic() >= deadline:
                    raise RuntimeError(f'QMP startup failed:\n{self.output}')
                time.sleep(0.01)
            self._qmp = Qmp(path)
            self.adcs = Adcs(adcs_path)
            self.eps, self.rtc = Eps(self._qmp), Rtc(self._qmp)
            self.system.initialize()
            self._qmp.call('cont')
            if self.web_enabled:
                try:
                    from _obcfi_web import Dashboard
                    self._dashboard = Dashboard(self, self.web_port)
                    self.web_url = self._dashboard.url
                    self.web_error = None
                except (OSError, ImportError) as exc:
                    # A sandbox without listening sockets can still run tests.
                    self.web_error = str(exc)
                    warnings.warn(f'Dashboard unavailable; QEMU continues: {exc}')
            return self
        except BaseException:
            self.stop()
            raise

    def stop(self):
        """Reap QEMU and its reader, then release control sockets; safe to repeat."""
        if self._dashboard:
            self._dashboard.close()
            self._dashboard = None
        self.web_url = None
        if self.adcs:
            self.adcs.close()
        if self._process:
            if self.running:
                self._process.kill()
            self._process.wait()
            if self.console:
                self.console.reader.join(timeout=2)
            self._process.stdin.close()
            self._process.stdout.close()
        if self._qmp:
            self._qmp.close()
            self._qmp = None
        if self._tmp:
            self._tmp.cleanup()
            self._tmp = None

    def __enter__(self):
        return self.start()

    def __exit__(self, *_):
        self.stop()

    def wait_for_boot(self, timeout=30):
        """Wait for firmware boot messages and console readiness."""
        return self.console.wait_for_boot(timeout)

    def command(self, line, timeout=10):
        """Execute a UART command; return its output through the next prompt."""
        return self.console.command(line, timeout)

    def expect(self, pattern, timeout=10):
        """Wait for matching UART output from the current console cursor."""
        return self.console.expect(pattern, timeout)

    wait_for_output = expect

    def interactive(self):
        """Attach terminal to UART; Ctrl-] returns without stopping QEMU."""
        return self.console.interactive()

    def wait_for_adc_value(self, channel, expected, *, tolerance=0.02, timeout=25):
        """Poll firmware ADC statistics (channel is its table index, not pin ID)."""
        deadline = time.monotonic() + timeout
        row = re.compile(rf'^\s*{channel}\s+([-+0-9.]+)', re.MULTILINE)
        while time.monotonic() < deadline:
            match = row.search(self.command('adc print'))
            if match and abs(float(match[1]) - expected) <= tolerance:
                return float(match[1])
            time.sleep(0.25)
        raise TimeoutError(f'Firmware ADC {channel} did not reach {expected}')

    def _set_adc(self, unit, channel, raw):
        if not 1 <= unit <= 2 or not 0 <= channel <= 31 or not 0 <= raw <= 4095:
            raise ValueError('Invalid ADC input')
        if self._qmp is None:
            raise RuntimeError('Start QEMU before setting inputs')
        self._qmp.set(f'/machine/soc/adc{unit}', f'sample[{channel}]', raw)
