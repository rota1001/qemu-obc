"""Every dashboard setting must cross HTTP -> QEMU -> native firmware UART.

Run with the optional web venv. Each ADC test uses two distinct values and
waits for actual periodic task samples; it never changes the guest clock or
writes firmware RAM. Evidence is saved in tests/artifacts/parameter_results.json.
"""
import importlib.util
import json
import math
import re
import time
import unittest
from datetime import datetime
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError
import obcfi

ROOT = Path(__file__).resolve().parents[4]
ARTIFACTS = Path(__file__).parent / 'artifacts'
# Independent contract: input name, housekeeping row, unit, A, B, tolerance.
ADC_CASES = [
    ('obc_board', 0, 'degC', 45.0, 10.0, 0.15),
    ('obc_1v2', 1, 'V', 1.0, 1.2, 0.003),
    ('obc_3v3_sys', 2, 'V', 3.0, 3.2, 0.003),
    ('obc_3v3_adc', 3, 'V', 3.1, 3.3, 0.003),
    ('obc_ext_3v3_in', 4, 'V', 2.9, 3.1, 0.003),
    ('obc_ext_3v3_out', 5, 'V', 2.8, 0.0, 0.003),
    ('obc_5v_current', 7, 'A', 0.2, 0.1, 0.004),
    ('obc_ext_5v_current', 8, 'A', 0.3, 0.05, 0.004),
    ('obc_5v_in', 9, 'V', 4.8, 5.0, 0.004),
    ('obc_5v_out', 10, 'V', 4.6, 4.9, 0.004),
]


@unittest.skipUnless(importlib.util.find_spec('fastapi') and importlib.util.find_spec('uvicorn'),
                     'Install requirements-web.txt to exercise dashboard controls')
class ParameterApplyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.obc = obcfi.Obc(ROOT/'obc/build/TMS570LC43_OBC_PLUS_20240102.out',
                           ROOT/'qemu/build/qemu-system-arm', web=True).start()
        try:
            cls.obc.wait_for_boot()
            with urlopen(cls.obc.web_url) as r:
                cls.token = re.search(r'name="obc-token" content="([^"]+)"', r.read().decode())[1]
            cls.evidence = []
        except BaseException:
            cls.obc.stop()
            raise

    @classmethod
    def tearDownClass(cls):
        ARTIFACTS.mkdir(exist_ok=True)
        (ARTIFACTS/'parameter_results.json').write_text(json.dumps(cls.evidence, indent=2))
        (ARTIFACTS/'parameter_uart.log').write_text(cls.obc.output)
        cls.obc.stop()

    def request(self, data=None):
        path = '/api/control' if data is not None else '/api/state'
        req = Request(self.obc.web_url+path,
                      data=json.dumps(data).encode() if data is not None else None,
                      headers={'X-OBC-Token':self.token, 'Content-Type':'application/json'})
        with urlopen(req, timeout=20) as r:
            return json.load(r)

    def command(self, line):
        """Go through exactly the same web command endpoint as the React console."""
        output = self.request({'action':'command','line':line})['output']
        self.assertNotIn('Unknown command', output)
        return output

    def record(self, parameter, requested, observed):
        type(self).evidence.append(dict(parameter=parameter, requested=requested,
                                       observed=observed, passed=True))

    def rows(self):
        output = self.command('adc print')
        rows = {int(m[1]): tuple(map(float,m.groups()[1:])) for m in
                re.finditer(r'^\s*(\d+)\s+([-+\d.]+)\s+([-+\d.]+)\s+([-+\d.]+)\s*$', output,re.M)}
        self.assertEqual(len(rows), 26, output)
        return rows

    def wait_sample(self, index, target, tolerance):
        deadline = time.monotonic()+35
        while True:
            rows = self.rows()
            if all(math.isfinite(v) and abs(v-target)<=tolerance for v in rows[index]):
                return rows
            if time.monotonic()>deadline:
                self.fail(f'OS ADC row {index}: expected {target}, got {rows[index]}')
            time.sleep(0.5)

    def eps_packet(self):
        output = self.command('EPS_ANALOG_HK_DATA')
        words = re.findall(r'address\[[0-9a-f]+\] : \[([0-9a-f]{8})\]', output)
        self.assertEqual(len(words),39,output)
        return bytes.fromhex(''.join(words))

    def rtc(self):
        output = self.command('RTC_DATE')
        match = re.search(r'RTC Time: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)',output)
        self.assertIsNotNone(match,output)
        return datetime.fromisoformat(match[1])

    def test_eps_battery_voltage(self):
        for volts in (12.0,16.123):
            self.request({'action':'battery','value':volts})
            p=self.eps_packet()
            packs=[int.from_bytes(p[i:i+2],'big') for i in (90,110,130)]
            cells=[int.from_bytes(p[i:i+2],'big') for i in (92,94,96,98)]
            self.assertEqual(packs,[round(volts*1000)]*3)
            self.assertEqual(cells,[round(volts*1000)//4]*4)
            self.record('battery',volts,dict(pack_mv=packs,cell_mv=cells))

    def test_eps_checksum(self):
        self.request({'action':'fault','name':'eps_checksum','enabled':True})
        try:
            output=self.command('EPS_CONFIG')
            self.assertIn('checksum mismatch',output)
            self.record('eps_checksum',True,output)
        finally:
            self.request({'action':'fault','name':'eps_checksum','enabled':False})
        output=self.command('EPS_CONFIG')
        self.assertNotIn('checksum mismatch',output)
        self.assertIn('address[',output)
        self.record('eps_checksum',False,output)

    def test_eps_disconnect(self):
        self.request({'action':'fault','name':'eps_disconnect','enabled':True})
        try:
            output=self.command('EPS_ANALOG_HK_DATA')
            self.assertIn('i2c=-3',output)
            self.assertIn('RTC Time:',self.command('RTC_DATE')) # other slave still works
            self.record('eps_disconnect',True,output)
        finally:
            self.request({'action':'fault','name':'eps_disconnect','enabled':False})
        self.eps_packet()
        self.record('eps_disconnect',False,'Valid 157-byte analog packet returned')

    def test_rtc_shift(self):
        self.request({'action':'shift','value':0})
        base=self.rtc()
        for shift in (86400,-3600,0):
            self.request({'action':'shift','value':shift})
            read=self.rtc()
            self.assertAlmostEqual((read-base).total_seconds(),shift,delta=2)
            self.record('shift',shift,read.isoformat())

    def test_rtc_freeze_and_shift_while_frozen(self):
        self.request({'action':'shift','value':0})
        self.request({'action':'fault','name':'rtc_freeze','enabled':True})
        try:
            base=self.rtc()
            time.sleep(1.2)
            self.assertEqual(self.rtc(),base)
            self.record('rtc_freeze',True,base.isoformat())
            self.request({'action':'shift','value':86400})
            self.assertEqual((self.rtc()-base).total_seconds(),86400)
            self.record('shift_while_frozen',86400,self.rtc().isoformat())
            self.request({'action':'shift','value':0})
            self.assertEqual(self.rtc(),base)
        finally:
            self.request({'action':'fault','name':'rtc_freeze','enabled':False})
        deadline=time.monotonic()+4
        while self.rtc()<=base and time.monotonic()<deadline:
            time.sleep(0.2)
        self.assertGreater(self.rtc(),base)
        self.record('rtc_freeze',False,self.rtc().isoformat())

    def test_invalid_input_leaves_previous_value(self):
        before=self.request()
        for data in ({'action':'input','name':'obc_1v2','value':-1},
                     {'action':'input','name':'obc_5v_current','value':-1},
                     {'action':'input','name':'obc_board','value':-274},
                     {'action':'battery','value':-1}, {'action':'shift','value':0.5}):
            with self.assertRaises(HTTPError):self.request(data)
        after=self.request()
        self.assertEqual(before['inputs'],after['inputs'])
        self.assertEqual(before['battery'],after['battery'])
        self.assertEqual(before['shift'],after['shift'])


def adc_test(case):
    name,index,unit,a,b,tolerance=case
    def test(self):
        for value in (a,b):
            before = self.rows()
            result=self.request({'action':'input','name':name,'value':value})
            self.assertFalse(result['clipped'])
            observed=self.wait_sample(index,value,tolerance)
            # Distinct values on adjacent channels must not get changed by this input.
            for other in (1,2,3,4,5,7,8,9,10):
                if other != index and before[other][0] != 0:
                    self.assertAlmostEqual(observed[other][0],before[other][0],delta=0.004)
            self.record(name,value,dict(unit=unit,row=index,avg=observed[index][0],
                                      maximum=observed[index][1],minimum=observed[index][2]))
    return test

for case in ADC_CASES:
    setattr(ParameterApplyTest,'test_adc_'+case[0],adc_test(case))
