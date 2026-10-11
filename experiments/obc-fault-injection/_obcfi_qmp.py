"""Small, serialized QMP connection with bounded waits and device discovery."""
import json
import socket
import threading
from contextlib import contextmanager


class Qmp:
    def __init__(self, path, timeout=5):
        """Connect a Unix socket; timeout bounds each QMP response in seconds."""
        self.lock = threading.RLock()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.stream = None
        self.sequence = 0
        self.events = []
        self.paths = {}
        try:
            self.sock.connect(str(path))
            self.stream = self.sock.makefile('rwb')
            if 'QMP' not in self._read():
                raise RuntimeError('Invalid QMP greeting')
            self.call('qmp_capabilities')
        except BaseException:
            self.close()
            raise

    def _read(self):
        line = self.stream.readline()
        if not line:
            raise ConnectionError('QEMU closed QMP')
        return json.loads(line)

    def call(self, command, arguments=None):
        """Issue a request; collect interleaved events without confusing replies."""
        with self.lock:
            self.sequence += 1
            request = dict(execute=command, id=self.sequence)
            if arguments is not None:
                request['arguments'] = arguments
            self.stream.write(json.dumps(request).encode() + b'\n')
            self.stream.flush()
            while True:
                reply = self._read()
                if 'event' in reply:
                    self.events.append(reply)
                    continue
                if reply.get('id') != self.sequence:
                    raise RuntimeError('Unexpected QMP response ID')
                if 'error' in reply:
                    raise RuntimeError(f"{command}: {reply['error']}")
                return reply['return']

    def device(self, typename):
        """Find one realized device by QOM type, without guessing anonymous IDs."""
        if typename in self.paths:
            return self.paths[typename]
        pending = ['/machine']
        matches = []
        while pending:
            path = pending.pop()
            for prop in self.call('qom-list', {'path': path}):
                kind = prop['type']
                if kind.startswith('child<'):
                    child = path + '/' + prop['name']
                    if kind == f'child<{typename}>':
                        matches.append(child)
                    pending.append(child)
        if len(matches) != 1:
            raise RuntimeError(f'Expected one {typename}, found {matches}')
        self.paths[typename] = matches[0]
        return matches[0]

    def get(self, path, name):
        return self.call('qom-get', {'path': path, 'property': name})

    def set(self, path, name, value):
        self.call('qom-set', {'path': path, 'property': name, 'value': value})

    @contextmanager
    def override(self, path, name, value):
        """Restore the previous device value on normal or exceptional exit."""
        old = self.get(path, name)
        self.set(path, name, value)
        try:
            yield
        finally:
            self.set(path, name, old)

    def close(self):
        if self.stream:
            self.stream.close()
        self.sock.close()
