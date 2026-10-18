"""ADCS stream framing, silent behavior, real SCI3 and dashboard capture."""
import importlib.util
import json
from pathlib import Path
import re
import socket
import tempfile
import time
import unittest
from urllib.request import Request, urlopen

import obcfi
from _obcfi_adcs import Adcs

ROOT = Path(__file__).resolve().parents[4]


class AdcsStreamTest(unittest.TestCase):
    def setUp(self):
        directory=tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        self.addCleanup(listener.close)
        path=Path(directory.name)/'sci3.sock'
        listener.bind(str(path));listener.listen(1)
        self.adcs=Adcs(path)
        self.addCleanup(self.adcs.close)
        self.peer,_=listener.accept()
        self.addCleanup(self.peer.close)
        self.peer.settimeout(0.15)

    def test_fragmented_get_and_coalesced_set_never_reply(self):
        self.peer.sendall(bytes.fromhex('C9 05'))
        with self.assertRaises(TimeoutError):
            self.adcs.wait_for_request(timeout=0.05)
        self.peer.sendall(bytes.fromhex('01 00 31'))
        first=self.adcs.wait_for_request()
        self.assertEqual(first['request'],bytes.fromhex('C9 05 01 00 31'))
        self.assertTrue(first['checksum_valid'])
        # Payload contains header-like values; these must remain in the SET.
        body=bytes.fromhex('C8 05 01 00 C8 C9 00 FF')
        setting=body+bytes([(-sum(body)) & 255])
        get=bytes.fromhex('C9 84 04 00 AF')
        self.peer.sendall(setting+get)
        second=self.adcs.wait_for_request(after=first['id'])
        third=self.adcs.wait_for_request(after=second['id'])
        self.assertEqual(second['request'],setting)
        self.assertEqual(second['kind'],'SET')
        self.assertEqual(third['request'],get)
        self.assertIsNone(third['response'])
        with self.assertRaises(socket.timeout):
            self.peer.recv(4096) # No ACK, greeting or response of any kind.

    def test_noise_bad_checksum_and_partial_disconnect_are_preserved(self):
        self.peer.sendall(bytes.fromhex('00 FF C9 05 01 00 30'))
        first=self.adcs.wait_for_request()
        second=self.adcs.wait_for_request(after=first['id'])
        self.assertEqual(first['request'],b'\x00\xff')
        self.assertEqual(first['status'],'unrecognized')
        self.assertFalse(second['checksum_valid'])
        self.peer.sendall(bytes.fromhex('C8 05 01 00 AA'))
        deadline=time.monotonic()+2
        while self.adcs.snapshot()['pending_hex']!='C8 05 01 00 AA':
            self.assertLess(time.monotonic(),deadline)
            time.sleep(0.01)
        self.peer.shutdown(socket.SHUT_RDWR)
        partial=self.adcs.wait_for_request(after=second['id'])
        self.assertEqual(partial['status'],'incomplete')
        self.assertEqual(partial['request'],bytes.fromhex('C8 05 01 00 AA'))
        self.adcs.reader.join(timeout=2)
        self.assertFalse(self.adcs.connected)
        with self.assertRaises(ConnectionError):
            self.adcs.wait_for_request(after=partial['id'])


class AdcsFirmwareTest(unittest.TestCase):
    def launch(self, **options):
        o=obcfi.Obc(ROOT/'obc/build/TMS570LC43_OBC_PLUS_20240102.out',
                    ROOT/'qemu/build/qemu-system-arm',**options).start()
        self.addCleanup(o.stop)
        o.wait_for_boot()
        return o

    def test_real_commands_are_captured_without_response(self):
        o=self.launch()
        cases=[('ADCS_GET_TIME','C9 05 01 00 31','GET'),
               ('ADCS_GET_Q','C9 84 04 00 AF','GET'),
               ('ADCS_GET_RATE','C9 88 03 00 AC','GET'),
               ('ADCS_SET_TIME 00563412','C8 05 01 00 00 56 34 12 96','SET')]
        after=0
        for command,expected,kind in cases:
            output=o.command(command)
            record=o.adcs.wait_for_request(after=after)
            self.assertEqual(record['request'],bytes.fromhex(expected))
            self.assertEqual(record['kind'],kind)
            self.assertIsNone(record['response'])
            self.assertEqual(record['status'],'not_replied')
            self.assertTrue(record['checksum_valid'])
            self.assertIn('failed' if kind=='GET' else 'success',output)
            after=record['id']
        with self.assertRaises(TimeoutError):
            o.adcs.wait_for_request(after=after,timeout=0.1)
        old_reader=o.adcs.reader
        old_path=Path(o.adcs.socket_path)
        o.stop()
        self.assertFalse(old_reader.is_alive())
        self.assertFalse(old_path.exists())
        o.start();o.wait_for_boot()
        o.command('ADCS_GET_TIME')
        self.assertEqual(o.adcs.wait_for_request()['request'],bytes.fromhex(cases[0][1]))

    @unittest.skipUnless(importlib.util.find_spec('fastapi') and importlib.util.find_spec('uvicorn'),
                         'Optional web dependencies not installed')
    def test_http_console_exposes_hex_and_independent_cursor(self):
        o=self.launch(web=True)
        with urlopen(o.web_url) as response:
            token=re.search(r'name="obc-token" content="([^"]+)"',response.read().decode())[1]
        def call(path,data=None):
            request=Request(o.web_url+path,data=json.dumps(data).encode() if data else None,
                            headers={'X-OBC-Token':token,'Content-Type':'application/json'})
            with urlopen(request,timeout=15) as response:return json.load(response)
        reply=call('/api/control',{'action':'command','line':'ADCS_GET_TIME'})
        self.assertIn('failed',reply['output'])
        o.adcs.wait_for_request()
        state=call('/api/state?adcs_cursor=0')
        record=state['adcs']['requests'][0]
        self.assertEqual(record['request'],'C9 05 01 00 31')
        self.assertIsNone(record['response'])
        self.assertEqual(state['adcs']['reply_mode'],'silent')
        cursor=state['adcs']['cursor']
        self.assertEqual(call(f'/api/state?adcs_cursor={cursor}')['adcs']['requests'],[])
        # Reading HTTP records must not consume Python's records.
        self.assertEqual(o.adcs.requests()[0]['request'],bytes.fromhex('C9 05 01 00 31'))
        self.assertIn('RTC Time:',o.command('RTC_DATE'))
