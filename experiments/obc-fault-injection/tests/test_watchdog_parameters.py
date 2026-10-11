"""Compare real sensor/transport faults with an actual missed watchdog feed.

Uses the BOARD_BRINGUP firmware and the same public controls as the dashboard.
Sensor faults are NOT assumed to imply heartbeat faults.
"""
from pathlib import Path
import re
import time
import unittest
import obcfi

ROOT = Path(__file__).resolve().parents[4]
ARTIFACTS = Path(__file__).parent / 'artifacts'


class WatchdogParametersTest(unittest.TestCase):
    def setUp(self):
        self.obc = obcfi.Obc(ROOT/'obc/build/TMS570LC43_OBC_PLUS_20240102.out',
                           ROOT/'qemu/build/qemu-system-arm').start()
        self.addCleanup(self.obc.stop)
        self.obc.wait_for_boot()
        self.assertIn('profile=BOARD_BRINGUP', self.obc.output)
        ARTIFACTS.mkdir(exist_ok=True)

    def status(self):
        output = self.obc.command('watchdog status')
        self.assertIn('hw=ON', output)
        feed = re.search(r'feed=(\w+)', output)[1]
        feeds = int(re.search(r'feeds=(\d+)',output)[1])
        denied = int(re.search(r'denied=(\d+)',output)[1])
        return output, feed, feeds, denied

    def test_extreme_parameters_and_bus_faults_keep_feeding(self):
        o = self.obc
        # Physical faults, rather than suppressing task beats inside firmware.
        o.system.set_temperature('obc_board', 100)
        o.system.set_voltage('obc_1v2', 0)
        o.system.set_voltage('obc_5v_in', 0)
        o.system.set_current('obc_5v', 1.0)
        o.wait_for_adc_value(0, 100, tolerance=0.15, timeout=35)
        for row, value in [(1,0),(7,1.0),(9,0)]:
            o.wait_for_adc_value(row,value,tolerance=0.004)
        # Observe the actual healthStatus.flag via the native read-only md
        # command. The map supplies the current symbol address; global.h's
        # packed structure has four uint8 fields before this uint32 union.
        symbol_map = (ROOT/'obc/build/TMS570LC43_OBC_PLUS_20240102.map').read_text()
        address = int(re.search(r'^([0-9a-f]+)\s+houseKeepingData\s*$',
                               symbol_map, re.MULTILINE)[1],16) + 4
        flag_dump = o.command(f'md {address:x} 4 l')
        flag = int(re.findall(r'\b[0-9a-f]{8}\b',flag_dump)[-1],16)
        self.assertEqual(flag & 0x283, 0x283, flag_dump) # temp, 1.2 V, current, 5 V
        with o.eps.bad_checksum():
            self.assertIn('checksum mismatch',o.command('EPS_CONFIG'))
        before = self.status()
        # Keep the external faults active for longer than the ADC heartbeat
        # interval (20 s) and hardware timeout (12 s). Console stays responsive.
        with o.eps.disconnected(), o.rtc.frozen():
            self.assertIn('i2c=-3',o.command('EPS_ANALOG_HK_DATA'))
            frozen = o.command('RTC_DATE')
            deadline = time.monotonic()+25
            while time.monotonic()<deadline:
                text,feed,feeds,denied = self.status()
                self.assertEqual(feed,'ALLOWED',text)
                self.assertEqual(denied,0,text)
                self.assertNotIn('watchdog feed blocked',o.output)
                self.assertEqual(o.output.count('reset_cause='),1,o.output[-1500:])
                time.sleep(1)
            self.assertEqual(o.command('RTC_DATE'), frozen)
            after = self.status()
            self.assertGreater(after[2],before[2])
        (ARTIFACTS/'watchdog_parameters.log').write_text(o.output)

    def test_explicit_stop_feed_causes_watchdog_reset(self):
        o = self.obc
        o.command('watchdog test-stop CONFIRM')
        o.expect(r'\[BOOT\] reset_cause=WATCHDOG_RESET \(', timeout=18)
        o.expect(r'\[BOOT\] console ready; background tasks released', timeout=10)
        o.expect(o.console.PROMPT)
        text,feed,feeds,denied=self.status()
        self.assertEqual(feed,'ALLOWED',text)
        self.assertEqual(denied,0,text)
        self.assertIn('watchdog feed blocked',o.output)
        (ARTIFACTS/'watchdog_reset.log').write_text(o.output)
