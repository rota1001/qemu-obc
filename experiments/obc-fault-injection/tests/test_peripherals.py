"""Actual firmware reads, rejects and recovers from injected peripheral faults."""
from datetime import datetime
from pathlib import Path
import re
import time
import unittest
import obcfi

ROOT = Path(__file__).resolve().parents[4]


class PeripheralsTest(unittest.TestCase):
    def setUp(self):
        self.obc = obcfi.Obc(
            ROOT / 'obc/build/TMS570LC43_OBC_PLUS_20240102.out',
            ROOT / 'qemu/build/qemu-system-arm').start()
        self.addCleanup(self.obc.stop)
        self.obc.wait_for_boot()

    def test_eps_voltage_disconnect_checksum_and_recovery(self):
        eps = self.obc.eps
        eps.battery_voltage = 12.0
        output = self.obc.command('EPS_ANALOG_HK_DATA')
        words = re.findall(r'address\[[0-9a-f]+\] : \[([0-9a-f]{8})\]', output)
        packet = bytes.fromhex(''.join(words))
        self.assertEqual(int.from_bytes(packet[90:92], 'big'), 12000)
        self.assertEqual(eps.battery_voltage, 12.0)
        with eps.disconnected():
            self.assertIn('i2c=-3', self.obc.command('EPS_ANALOG_HK_DATA'))
        self.assertIn('9be0', self.obc.command('EPS_ANALOG_HK_DATA'))
        # E2 uses XOR validation; flight E0/E1 deliberately do not.
        with eps.bad_checksum():
            self.assertIn('checksum mismatch', self.obc.command('EPS_CONFIG'))
        self.assertNotIn('Fail', self.obc.command('EPS_CONFIG'))
        self.assertTrue(self.obc.status()['running'])

    def rtc_time(self):
        text = self.obc.command('RTC_DATE')
        value = re.search(r'RTC Time: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)', text)[1]
        return datetime.fromisoformat(value)

    def test_rtc_shift_freeze_resume(self):
        before = self.rtc_time()
        self.obc.rtc.shift_seconds = 86400
        after = self.rtc_time()
        self.assertAlmostEqual((after-before).total_seconds(), 86400, delta=1)
        with self.obc.rtc.frozen():
            frozen = self.rtc_time()
            time.sleep(1.2)
            self.assertEqual(self.rtc_time(), frozen)
        time.sleep(1.2)
        self.assertGreater(self.rtc_time(), frozen)
        self.obc.rtc.shift_seconds = 0
        self.assertEqual(self.rtc_time().date(), before.date())

    def test_restart_and_exception_restoration(self):
        with self.assertRaisesRegex(RuntimeError, 'test fault'):
            with self.obc.eps.disconnected():
                raise RuntimeError('test fault')
        self.assertIn('9be0', self.obc.command('EPS_ANALOG_HK_DATA'))
        self.obc.stop()
        self.obc.start()
        self.obc.wait_for_boot()
        self.assertIn('RTC Time:', self.obc.command('RTC_DATE'))
