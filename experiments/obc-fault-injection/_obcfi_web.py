"""Optional FastAPI dashboard; imported only when Obc(web=True) starts."""
import secrets
import socket
import threading
import time
from dataclasses import asdict
from pathlib import Path
from typing import Literal, Union, Annotated

from fastapi import FastAPI, Request, Body
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ConfigDict
import uvicorn
from _obcfi_system import _INPUTS


class Input(BaseModel):
    model_config = ConfigDict(extra='forbid')
    action: Literal['input']
    name: str
    value: float

class Value(BaseModel):
    model_config = ConfigDict(extra='forbid')
    action: Literal['battery', 'shift']
    value: float

class Fault(BaseModel):
    model_config = ConfigDict(extra='forbid')
    action: Literal['fault']
    name: Literal['eps_disconnect', 'eps_checksum', 'rtc_freeze']
    enabled: bool

class Command(BaseModel):
    model_config = ConfigDict(extra='forbid')
    action: Literal['command']
    line: str


class Dashboard:
    """One loopback-only FastAPI service; UART polling never consumes expect()."""
    def __init__(self, obc, port=0):
        self.obc = obc
        self.token = secrets.token_urlsafe(24)
        self.lock = threading.RLock()
        self.faults = {}
        assets = Path(__file__).with_name('web') / 'dist'
        html = (assets / 'index.html').read_text()
        app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)

        @app.middleware('http')
        async def local_session(request: Request, call_next):
            if request.headers.get('host') != self.address or request.headers.get('origin') not in (None, self.url):
                return JSONResponse({'error':'Local origin required'}, status_code=403)
            if request.url.path != '/' and not request.url.path.startswith('/assets/') and request.headers.get('x-obc-token') != self.token:
                return JSONResponse({'error':'Invalid session token'}, status_code=403)
            if request.method == 'POST':
                # Limit actual streamed content, including chunked requests.
                body = bytearray()
                async for chunk in request.stream():
                    body.extend(chunk)
                    if len(body) > 4096:
                        return JSONResponse({'error':'Request exceeds 4096 bytes'}, status_code=413)
                request._body = bytes(body)
            response = await call_next(request)
            response.headers['Cache-Control'] = 'no-store'
            response.headers['X-Content-Type-Options'] = 'nosniff'
            response.headers['Content-Security-Policy'] = "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'"
            return response

        @app.get('/', response_class=HTMLResponse)
        def index():
            return html.replace('__TOKEN__', self.token)

        app.mount('/assets', StaticFiles(directory=assets / 'assets'), name='assets')

        @app.get('/api/state')
        def state(cursor: int = 0, adcs_cursor: int = 0):
            try:
                return self.state(cursor, adcs_cursor)
            except (ValueError, RuntimeError, OSError) as exc:
                return JSONResponse({'error':str(exc)}, status_code=400)

        @app.post('/api/control')
        def control(data: Annotated[Union[Input, Value, Fault, Command], Body(discriminator='action')]):
            try:
                return self.control(data.model_dump())
            except (ValueError, KeyError, TypeError, RuntimeError, OSError) as exc:
                return JSONResponse({'error':str(exc)}, status_code=400)

        self.socket = socket.socket()
        try:
            self.socket.bind(('127.0.0.1', port))
            self.socket.listen(128)
            self.address = f'127.0.0.1:{self.socket.getsockname()[1]}'
            self.url = 'http://' + self.address
            config = uvicorn.Config(app, log_level='warning', access_log=False,
                                    timeout_graceful_shutdown=12)
            self.server = uvicorn.Server(config)
            self.thread = threading.Thread(target=self.server.run,
                                           kwargs={'sockets':[self.socket]}, daemon=True)
            self.thread.start()
            deadline = time.monotonic() + 5
            while not self.server.started:
                if not self.thread.is_alive() or time.monotonic() > deadline:
                    self.server.should_exit = True
                    self.thread.join(timeout=2)
                    raise OSError('Dashboard server failed to start')
                time.sleep(0.01)
        except BaseException:
            self.socket.close()
            raise

    def state(self, cursor, adcs_cursor=0):
        """Read settings and incremental UART without interfering with Python waits."""
        with self.lock:
            with self.obc.console._condition:
                output = self.obc.output
                if not 0 <= cursor <= len(output):
                    cursor = 0
                end = min(len(output), cursor + 32768)
                chunk = output[cursor:end]
            inputs = [dict(name=name, kind=spec[0], unit=spec[1],
                           value=self.obc.system.value(name), firmware_index=spec[3]) for name, spec in _INPUTS.items()]
            return dict(running=self.obc.running, inputs=inputs,
                        battery=self.obc.eps.battery_voltage,
                        shift=self.obc.rtc.shift_seconds,
                        faults=list(self.faults), output=chunk, cursor=end,
                        adcs=self.obc.adcs.snapshot(adcs_cursor))

    def control(self, data):
        """Validate a narrow action list; never execute host commands."""
        action = data.get('action')
        if action == 'command':
            line = data.get('line')
            if not isinstance(line, str) or not 1 <= len(line) <= 128 or any(ord(c) < 32 or ord(c) > 126 for c in line):
                raise ValueError('Command must be 1..128 printable ASCII characters')
            if self.obc.console._interactive:
                raise RuntimeError('Exit terminal interactive mode before sending web commands')
            # Do not hold the parameter lock while waiting for UART: polling stays live.
            output = self.obc.command(line)
            return {'ok': True, 'output': output}
        with self.lock:
            if action == 'input':
                name = data['name']
                if name not in _INPUTS:
                    raise ValueError('Unknown system input')
                method = getattr(self.obc.system, 'set_' + _INPUTS[name][0])
                return asdict(method(name, float(data['value'])))
            if action == 'battery':
                self.obc.eps.battery_voltage = data['value']
            elif action == 'shift':
                value = data['value']
                if not float(value).is_integer():
                    raise ValueError('RTC shift must be integer seconds')
                self.obc.rtc.shift_seconds = int(value)
            elif action == 'fault':
                factories = {'eps_disconnect': self.obc.eps.disconnected,
                             'eps_checksum': self.obc.eps.bad_checksum,
                             'rtc_freeze': self.obc.rtc.frozen}
                name, enabled = data['name'], data['enabled']
                if name not in factories or type(enabled) is not bool:
                    raise ValueError('Invalid fault or enabled flag')
                if enabled and name not in self.faults:
                    context = factories[name]()
                    context.__enter__()
                    self.faults[name] = context
                elif not enabled and name in self.faults:
                    self.faults.pop(name).__exit__(None, None, None)
            else:
                raise ValueError('Unknown control action')
            return {'ok': True}

    def close(self):
        """Stop accepting HTTP requests and undo faults owned by the dashboard."""
        self.server.should_exit = True
        self.thread.join(timeout=15)
        self.socket.close()
        with self.lock:
            for context in reversed(list(self.faults.values())):
                try:
                    context.__exit__(None, None, None)
                except (RuntimeError, OSError):
                    pass  # QEMU may already have exited.
            self.faults.clear()
