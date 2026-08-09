"""Shared process handling for local experiments and syscall diagnostics."""

import os
import selectors
import signal
import subprocess
import time


def read_ready_line(process, timeout=5):
    """Read one bounded readiness line, including when a child writes it in pieces."""
    deadline = time.monotonic() + timeout
    line = bytearray()
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while len(line) < 65536:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not selector.select(remaining):
                raise RuntimeError("server readiness timeout")
            byte = os.read(process.stdout.fileno(), 1)
            if not byte:
                raise RuntimeError("server exited before reporting readiness")
            line.extend(byte)
            if byte == b"\n":
                return line.decode("utf-8")
    raise RuntimeError("server readiness line exceeds 64 KiB")


def stop_server(process, timeout=7):
    """Reap the child on every path, escalating if graceful shutdown fails."""
    if process.poll() is None:
        try:
            process.send_signal(signal.SIGTERM)
        except ProcessLookupError:
            pass
    killed = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        killed = True
    return {
        "server_returncode": process.returncode,
        "server_stdout": stdout,
        "server_stderr": stderr,
        "server_killed": killed,
    }


def trial_succeeded(record):
    """A zero exit alone is insufficient evidence of a usable measurement."""
    measurement = record.get("measurement") or {}
    summary = record.get("server_summary") or {}
    completed = measurement.get("completed_messages", 0)
    return (
        record.get("client_returncode") == 0
        and record.get("server_returncode") == 0
        and not record.get("runner_error")
        and measurement.get("status") == "ok"
        and completed > 0
        and measurement.get("rtt_samples") == completed
        and measurement.get("errors") == 0
        and measurement.get("timeouts") == 0
        and summary.get("event") == "summary"
        and summary.get("errors") == 0
    )
