"""UART transcript, scripted console commands, and interactive terminal."""
import os
import re
import select
import sys
import termios
import threading
import time
import tty


class Console:
    """Own the single UART reader; keep CRLF for the terminal and LF for regexes."""
    PROMPT = re.compile(r"(/[^\s>]*)> ")

    def __init__(self, process):
        self._process = process
        self.output = ""
        self._cursor = 0
        self._condition = threading.Condition()
        self._command_lock = threading.Lock()
        self._interactive = False
        self.reader = threading.Thread(target=self._read_uart, daemon=True)
        self.reader.start()

    @property
    def running(self):
        return self._process.poll() is None

    def _read_uart(self) -> None:
        while self._process and self._process.stdout:
            chunk = self._process.stdout.read1(4096)
            if not chunk:
                break
            raw_text = chunk.decode("latin1")
            # Keep a normalized transcript for regex matching, but preserve
            # CRLF on the live terminal.  A terminal needs CR to return to
            # column zero; printing the normalized LF-only text causes each
            # new line to begin at the previous line's cursor position.
            text = raw_text.replace("\r", "")
            with self._condition:
                self.output += text
                self._condition.notify_all()
            if self._interactive:
                # latin1 preserves every UART byte one-to-one.  Write and
                # flush here so output remains live even without a newline.
                sys.stdout.write(raw_text)
                sys.stdout.flush()

    def expect(self, pattern: str | re.Pattern, timeout: float = 10):
        """Wait for future UART output matching a regex."""
        regex = re.compile(pattern) if isinstance(pattern, str) else pattern
        deadline = time.monotonic() + timeout
        with self._condition:
            while True:
                match = regex.search(self.output, self._cursor)
                if match:
                    self._cursor = match.end()
                    return match
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not self.running:
                    raise TimeoutError(f"did not see {regex.pattern!r}\n{self.output[-1500:]}")
                self._condition.wait(min(remaining, 0.25))

    def wait_for_boot(self, timeout: float = 30) -> None:
        """Wait until the real firmware has started and accepts commands."""
        self.expect(r"\[BOOT\] reset_cause=", timeout)
        self.expect(r"\[BOOT\] console ready; background tasks released")
        self.expect(self.PROMPT)

    def command(self, line: str, timeout: float = 10) -> str:
        """Run one UART console command and return text before the next prompt."""
        if not self.running:
            raise RuntimeError("QEMU is not running")
        with self._command_lock:
            self._cursor = len(self.output)
            self._process.stdin.write((line + "\r").encode("latin1"))
            self._process.stdin.flush()
            self.expect(re.escape(line) + r"\n", timeout)
            start = self._cursor
            prompt = self.expect(self.PROMPT, timeout)
            return self.output[start:prompt.start()]

    def interactive(self) -> None:
        """Connect this terminal directly to the firmware UART.

        Keyboard bytes are forwarded unchanged and UART output is mirrored to
        stdout while still being retained in :attr:`output`. Press ``Ctrl-]``
        to return to Python without stopping QEMU. This method requires a real
        terminal because canonical input and local echo must be disabled.
        """
        if not self.running:
            raise RuntimeError("QEMU is not running")
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            raise RuntimeError("interactive() requires terminal stdin/stdout")

        stdin_fd = sys.stdin.fileno()
        previous_terminal = termios.tcgetattr(stdin_fd)
        print("--- OBC UART interactive; press Ctrl-] to exit ---", flush=True)
        self._interactive = True
        try:
            tty.setraw(stdin_fd)
            while self.running:
                readable, _, _ = select.select([stdin_fd], [], [], 0.25)
                if not readable:
                    continue
                data = os.read(stdin_fd, 1024)
                if not data or b"\x1d" in data:  # Ctrl-]
                    before_escape = data.split(b"\x1d", 1)[0]
                    if before_escape:
                        self._process.stdin.write(before_escape)
                        self._process.stdin.flush()
                    break
                self._process.stdin.write(data)
                self._process.stdin.flush()
        finally:
            self._interactive = False
            self._cursor = len(self.output)
            termios.tcsetattr(stdin_fd, termios.TCSADRAIN, previous_terminal)
            print("\n--- left OBC UART interactive mode ---", flush=True)

