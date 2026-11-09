#!/usr/bin/env python3
"""Shared process harness for the MiniVectorDB protocol/SQL edge tests.

This module only starts the *existing* binary, feeds it bytes on stdin and
collects stdout/stderr.  It never rebuilds or edits the database.

Both test drivers (tools/protocol_matrix.py and tests/run_sql_edges.py) use
this because the interesting protocol questions are about timing: when the
process flushes a result, whether it needs a newline, and what it does when
stdin stays open.
"""

from __future__ import annotations

import os
import selectors
import shutil
import signal
import subprocess
import tempfile
import time


def new_data_dir(prefix: str = "mvdb-test-") -> str:
    return tempfile.mkdtemp(prefix=prefix)


def remove_data_dir(path: str) -> None:
    shutil.rmtree(path, ignore_errors=True)


def run_batch(bin_path: str, data_dir: str, script, args=(), env=None,
              timeout: float = 20.0):
    """Run one script with stdin closed afterwards (the batch-driver shape).

    Returns (returncode, stdout_bytes, stderr_bytes).
    """
    e = dict(os.environ)
    e.update(env or {})
    cmd = [bin_path, "--data-dir", data_dir] + list(args)
    if isinstance(script, str):
        script = script.encode()
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, env=e)
    out, err = p.communicate(script, timeout=timeout)
    return p.returncode, out, err


class Session:
    """An interactive stdin/stdout session.

    ``pump`` reads both pipes without blocking forever; ``wait_lines`` waits
    until stdout has at least N lines (or a timeout elapses).  This is what
    lets the tests distinguish "flushed immediately" from "buffered until the
    next newline / EOF".
    """

    def __init__(self, bin_path: str, data_dir: str = None, args=(), env=None):
        self.bin_path = bin_path
        self.data_dir = data_dir or new_data_dir()
        self._owns_dir = data_dir is None
        e = dict(os.environ)
        e.update(env or {})
        self.proc = subprocess.Popen(
            [bin_path, "--data-dir", self.data_dir] + list(args),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=e)
        self._out = b""
        self._err = b""
        self._eof_out = False
        self._eof_err = False
        self._sel = selectors.DefaultSelector()
        self._sel.register(self.proc.stdout, selectors.EVENT_READ)
        self._sel.register(self.proc.stderr, selectors.EVENT_READ)

    # ------------------------------------------------------------------ io
    def pump(self, timeout: float = 0.2) -> bytes:
        """Read available output, waiting up to ``timeout`` for the first byte.

        Once data arrives only a short grace slice is added, so this returns
        quickly for flushed results but still gives a slow process time.
        """
        got = b""
        start = time.monotonic()
        deadline = start + timeout
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            try:
                events = self._sel.select(deadline - now)
            except (OSError, ValueError):
                break
            if not events:
                break
            read_any = False
            for key, _ in events:
                stream = key.fileobj
                try:
                    data = os.read(stream.fileno(), 65536)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    data = b""
                if not data:
                    try:
                        self._sel.unregister(stream)
                    except (KeyError, ValueError):
                        pass
                    if stream is self.proc.stdout:
                        self._eof_out = True
                    else:
                        self._eof_err = True
                    continue
                read_any = True
                if stream is self.proc.stdout:
                    self._out += data
                    got += data
                else:
                    self._err += data
            if read_any:
                # Burst grace: keep reading only while bytes keep arriving.
                deadline = min(start + timeout, time.monotonic() + 0.01)
            else:
                break
        return got

    def wait_lines(self, n: int, timeout: float = 1.5) -> bool:
        """Pump until stdout holds at least n newline-terminated lines."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._out.count(b"\n") >= n:
                self.pump(0.05)
                return True
            self.pump(min(0.05, max(0.0, deadline - time.monotonic())))
        return self._out.count(b"\n") >= n

    def wait_contains(self, needle: bytes, timeout: float = 1.5) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in self._out:
                return True
            self.pump(min(0.05, max(0.0, deadline - time.monotonic())))
        return needle in self._out

    def send(self, text) -> None:
        if isinstance(text, str):
            text = text.encode()
        try:
            self.proc.stdin.write(text)
            self.proc.stdin.flush()
        except (BrokenPipeError, ValueError, OSError):
            pass

    def close_stdin(self) -> None:
        try:
            self.proc.stdin.close()
        except (BrokenPipeError, OSError, ValueError):
            pass

    def wait(self, timeout: float = 3.0):
        """Wait for exit; return the code (or None when it had to be killed)."""
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def alive(self) -> bool:
        return self.proc.poll() is None

    def terminate(self, sig=signal.SIGTERM, timeout: float = 3.0):
        if self.proc.poll() is None:
            try:
                self.proc.send_signal(sig)
            except (ProcessLookupError, OSError):
                pass
        return self.wait(timeout)

    def kill9(self, timeout: float = 3.0):
        if self.proc.poll() is None:
            try:
                self.proc.kill()
            except (ProcessLookupError, OSError):
                pass
        return self.wait(timeout)

    # ------------------------------------------------------------- accessors
    def out_bytes(self) -> bytes:
        return self._out

    def out_text(self) -> str:
        return self._out.decode("utf-8", "backslashreplace")

    def err_text(self) -> str:
        return self._err.decode("utf-8", "backslashreplace")

    def out_lines(self):
        return self.out_text().splitlines()

    def take_out(self) -> str:
        text = self.out_text()
        self._out = b""
        return text

    def drain(self, timeout: float = 0.3) -> str:
        self.pump(timeout)
        return self.take_out()

    # --------------------------------------------------------------- cleanup
    def close(self, timeout: float = 3.0) -> int:
        self.close_stdin()
        code = self.wait(timeout)
        if code is None:
            self.kill9()
            code = self.proc.returncode
        self.pump(0.05)
        try:
            self._sel.close()
        except Exception:
            pass
        if self._owns_dir:
            remove_data_dir(self.data_dir)
        return code

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False


def strip_ready(text: str, marker: str = "ready"):
    """Drop the ready-marker line from a stdout transcript."""
    lines = text.splitlines()
    if marker and lines and lines[0] == marker:
        lines = lines[1:]
    return lines


def error_codes(lines):
    """Return [(line_index, code)] for ERROR lines."""
    out = []
    prefix = "ERROR "
    for i, line in enumerate(lines):
        if line.startswith(prefix):
            head = line[len(prefix):].split(":", 1)[0]
            if head.isdigit():
                out.append((i, int(head)))
    return out


def fmt_status(ok: bool) -> str:
    return "PASS" if ok else "FAIL"
