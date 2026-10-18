"""Passive ADCS UART peer: capture binary requests and deliberately never reply.

The socket carries actual SCI3 bytes, not console text or QMP JSON. GET frames
are five bytes; SET frames have a four-byte header, count*4 payload, checksum.
"""
from collections import deque
import socket
import threading
import time


class Adcs:
    """Receive OBC requests on a Unix socket without sending any response.

    socket_path: QEMU SCI3 chardev's Unix socket.
    Requests retain immutable ``bytes``; use ``record['request'].hex(' ')`` for
    display. At most 1000 completed/raw records are retained. IDs increase even
    when old records are evicted. A partial frame remains visible in pending.
    """
    def __init__(self, socket_path):
        self.socket_path = str(socket_path)
        self._condition = threading.Condition()
        self._records = deque(maxlen=1000)
        self._pending = bytearray()
        self._sequence = 0
        self._started = time.monotonic()
        self._closed = threading.Event()
        self.connected = False
        self.error = None
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self._socket.connect(self.socket_path)
            self._socket.settimeout(0.2)
        except BaseException:
            self._socket.close()
            raise
        self.connected = True
        self.reader = threading.Thread(target=self._read, name='obc-adcs', daemon=True)
        self.reader.start()

    def _record(self, data, kind, status):
        # Called with _condition held; no byte is ever sent back to QEMU.
        self._sequence += 1
        self._records.append(dict(id=self._sequence, elapsed_ms=round(
            (time.monotonic()-self._started)*1000, 3), kind=kind,
            request=bytes(data), response=None, status=status,
            checksum_valid=(sum(data) & 0xff) == 0 if kind in ('GET','SET') else None))
        self._condition.notify_all()

    def _parse(self):
        while self._pending:
            head = self._pending[0]
            if head not in (0xc8, 0xc9):
                end = next((i for i,b in enumerate(self._pending)
                            if b in (0xc8, 0xc9)), len(self._pending))
                self._record(self._pending[:end], 'RAW', 'unrecognized')
                del self._pending[:end]
                continue
            if len(self._pending) < 4:
                return
            length = 5 if head == 0xc9 else 5 + self._pending[2]*4
            if len(self._pending) < length:
                return
            frame = bytes(self._pending[:length])
            del self._pending[:length]
            self._record(frame, 'GET' if head == 0xc9 else 'SET', 'not_replied')

    def _read(self):
        try:
            while not self._closed.is_set():
                try:
                    data = self._socket.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    break
                with self._condition:
                    self._pending.extend(data)
                    self._parse()
        except OSError as exc:
            if not self._closed.is_set():
                self.error = str(exc)
        finally:
            with self._condition:
                if self._pending:
                    self._record(self._pending, 'RAW', 'incomplete')
                    self._pending.clear()
                self.connected = False
                self._condition.notify_all()

    def requests(self, after=0):
        """Return retained request records whose ID is greater than ``after``."""
        with self._condition:
            return [dict(record) for record in self._records if record['id'] > after]

    def wait_for_request(self, after=0, timeout=5):
        """Wait for one captured frame after an ID; does not consume the record."""
        deadline = time.monotonic()+timeout
        with self._condition:
            while True:
                for record in self._records:
                    if record['id'] > after:
                        return dict(record)
                if not self.connected:
                    raise ConnectionError(self.error or 'ADCS socket closed')
                remaining = deadline-time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('No ADCS request received')
                self._condition.wait(remaining)

    def snapshot(self, after=0):
        """JSON-safe view for the web dashboard, with pending bytes and cursor."""
        with self._condition:
            return dict(connected=self.connected, error=self.error, reply_mode='silent',
                        cursor=self._sequence, oldest_id=self._records[0]['id'] if self._records else 0,
                        pending_hex=self._pending.hex(' ').upper(), requests=[
                            {**r, 'request': r['request'].hex(' ').upper()}
                            for r in self._records if r['id'] > after])

    def close(self):
        """Release the peer and join its reader; safe to call more than once."""
        self._closed.set()
        try:
            self._socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.reader.join(timeout=2)
        self._socket.close()
