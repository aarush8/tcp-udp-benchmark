#!/usr/bin/env python3
"""Black-box checks against real sockets; no external Python dependencies."""

import contextlib
import csv
import io
import json
import selectors
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

BUILD = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path("build").resolve()


@contextlib.contextmanager
def server(protocol="tcp", extra=()):
    proc = subprocess.Popen(
        [
            str(BUILD / "netbench_server"),
            "--protocol",
            protocol,
            "--port",
            "0",
            "--clients",
            "8",
            "--duration",
            "10",
            *extra,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        with selectors.DefaultSelector() as sel:
            sel.register(proc.stdout, selectors.EVENT_READ)
            if not sel.select(5):
                raise RuntimeError("server did not become ready")
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError(proc.stderr.read())
        yield proc, json.loads(line)["port"]
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        out, err = proc.communicate(timeout=7)
        if proc.returncode != 0:
            raise RuntimeError(f"server exit {proc.returncode}: {err} {out}")


def client(port, *extra, protocol="tcp"):
    return subprocess.run(
        [
            str(BUILD / "netbench_client"),
            "--port",
            str(port),
            "--protocol",
            protocol,
            "--duration",
            ".2",
            "--messages",
            "8",
            "--nodelay",
            "1",
            *extra,
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )


class Integration(unittest.TestCase):
    def test_protocol_sizes_modes_concurrency(self):
        for protocol in ("tcp", "udp"):
            for mode in ("blocking", "nonblocking"):
                for size in (64, 4096, 1048576):
                    with self.subTest(protocol=protocol, mode=mode, payload=size):
                        with server(protocol, ("--io", mode)) as (_, port):
                            p = client(
                                port,
                                "--payload",
                                str(size),
                                "--clients",
                                "4",
                                "--io",
                                mode,
                                "--messages",
                                "2",
                                "--duration",
                                "1",
                                "--mbps",
                                "200",
                                protocol=protocol,
                            )
                            self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
                            r = json.loads(p.stdout)
                            self.assertEqual(r["completed_messages"], 8)
                            self.assertEqual(r["rtt_samples"], 8)
                            self.assertEqual(r["errors"], 0)
                            self.assertEqual(
                                r["udp_echo_loss_percent"], 0 if protocol == "udp" else None
                            )

    def test_optimization_variants(self):
        for buffering in ("allocate", "reuse"):
            for write in ("split", "coalesced"):
                for mode in ("blocking", "nonblocking"):
                    for protocol in ("tcp", "udp"):
                        with self.subTest(
                            buffering=buffering, write=write, mode=mode, protocol=protocol
                        ):
                            with server(protocol, ("--buffering", buffering, "--io", mode)) as (
                                _,
                                port,
                            ):
                                p = client(
                                    port,
                                    "--buffering",
                                    buffering,
                                    "--write",
                                    write,
                                    "--io",
                                    mode,
                                    "--clients",
                                    "4",
                                    "--payload",
                                    "16384",
                                    protocol=protocol,
                                )
                                self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
                                self.assertEqual(json.loads(p.stdout)["completed_messages"], 32)

    def test_udp_duplicate_and_stale_echoes(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(("127.0.0.1", 0))
            peer.settimeout(2)

            def respond():
                for _ in range(4):
                    data, addr = peer.recvfrom(2048)
                    peer.sendto(data, addr)
                    peer.sendto(data, addr)

            t = threading.Thread(target=respond)
            t.start()
            p = client(
                peer.getsockname()[1], "--messages", "2", "--payload", "2400", protocol="udp"
            )
            t.join()
        self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
        r = json.loads(p.stdout)
        self.assertEqual(r["completed_messages"], 2)
        self.assertGreaterEqual(r["duplicates"], 2)
        self.assertGreaterEqual(r["late_or_stale"], 1)
        self.assertEqual(r["udp_echo_loss_percent"], 0)

    def test_low_rate_tcp_idle_connection(self):
        with server() as (_, port):
            p = client(port, "--duration", "1.5", "--rate", "1", "--messages", "2")
        self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
        self.assertEqual(json.loads(p.stdout)["completed_messages"], 2)

    def test_udp_pacing_respects_io_deadline(self):
        with server("udp") as (_, port):
            began = time.monotonic()
            p = client(
                port,
                "--payload",
                "1048576",
                "--mbps",
                ".01",
                "--messages",
                "1",
                "--timeout-ms",
                "30",
                protocol="udp",
            )
            elapsed = time.monotonic() - began
        self.assertEqual(p.returncode, 2)
        self.assertLess(elapsed, 0.8)
        self.assertEqual(json.loads(p.stdout)["timeouts"], 1)

    def test_server_releases_completed_connections(self):
        with server() as (proc, port):
            baseline = len(list(Path(f"/proc/{proc.pid}/fd").iterdir()))
            p = client(port, "--iterations", "10", "--messages", "2")
            self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
            time.sleep(0.15)
            self.assertLessEqual(len(list(Path(f"/proc/{proc.pid}/fd").iterdir())), baseline)

    def test_connection_limit(self):
        with server(extra=("--clients", "1")) as (_, port):
            with socket.create_connection(("127.0.0.1", port)) as first:
                # Ensure the first connection is accepted and held in a partial-header read.
                first.sendall(b"N")
                time.sleep(0.03)
                p = client(port, "--timeout-ms", "50")
                self.assertEqual(p.returncode, 2)
                self.assertEqual(json.loads(p.stdout)["completed_messages"], 0)

    def test_sequential_reconnects_at_connection_limit(self):
        with server(extra=("--clients", "1")) as (_, port):
            p = client(port, "--iterations", "30", "--messages", "1")
        self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
        rows = [json.loads(line) for line in p.stdout.splitlines()]
        self.assertEqual(len(rows), 30)
        self.assertTrue(all(r["completed_messages"] == 1 for r in rows))

    def test_incomplete_frame_is_server_error(self):
        with server() as (proc, port):
            for partial in (b"N", struct.pack("!IIQII", 0x4E423031, 64, 0, 0, 1)):
                with socket.create_connection(("127.0.0.1", port)) as s:
                    s.sendall(partial)
                    s.shutdown(socket.SHUT_WR)
                    self.assertEqual(s.recv(1), b"")
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=2)
            summary = json.loads(proc.stdout.readline())
            self.assertEqual(summary["errors"], 2)
            self.assertEqual(summary["frames_echoed"], 0)

    def test_invalid_config(self):
        for args in (
            ("--payload", "1"),
            ("--clients", "33"),
            ("--duration", "nan"),
            ("--mbps", "-1"),
            ("--port", "65536"),
            ("--io", "epoll"),
            ("--messages", "1x"),
            ("--buffering", "unknown"),
            ("--write", "unknown"),
            ("--payload",),
            ("--nodelay", "2"),
        ):
            p = subprocess.run(
                [str(BUILD / "netbench_client"), *args], capture_output=True, timeout=2
            )
            self.assertEqual(p.returncode, 1)

    def test_connection_failure(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            p = client(s.getsockname()[1])
        self.assertEqual(p.returncode, 2)
        self.assertEqual(json.loads(p.stdout)["status"], "error")
        self.assertIsNone(json.loads(p.stdout)["rtt_mean_us"])

    def test_tcp_stall_deadline(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()

            def stall():
                conn, _ = listener.accept()
                with conn:
                    time.sleep(0.3)

            t = threading.Thread(target=stall)
            t.start()
            p = client(listener.getsockname()[1], "--timeout-ms", "50")
            t.join()
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertEqual(json.loads(p.stdout)["timeouts"], 1)

    def test_csv_and_iterations(self):
        with server() as (_, port):
            p = client(port, "--format", "csv", "--iterations", "2")
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = list(csv.DictReader(io.StringIO(p.stdout)))
        self.assertEqual(len(rows), 2)
        self.assertEqual(int(rows[0]["completed_messages"]), 8)
        self.assertEqual(json.loads(rows[0]["config_json"])["messages"], 8)
        self.assertEqual(json.loads(rows[0]["record_json"])["status"], "ok")

    def test_tcp_wire_framing_and_malformed(self):
        with server() as (_, port):
            with socket.create_connection(("127.0.0.1", port)) as s:
                frame = struct.pack("!IIQII", 0x4E423031, 64, 22, 0, 1) + bytes(range(64))
                for byte in frame[:24]:
                    s.sendall(bytes([byte]))
                s.sendall(frame[24:] + frame)
                out = bytearray()
                while len(out) < len(frame) * 2:
                    out += s.recv(len(frame) * 2 - len(out))
                self.assertEqual(out, frame * 2)
            with socket.create_connection(("127.0.0.1", port)) as s:
                s.sendall(struct.pack("!IIQII", 0x4E423031, 0xFFFFFFFF, 0, 0, 1))
                self.assertEqual(s.recv(1), b"")

    def test_udp_drop_is_visible(self):
        # A bound endpoint deliberately drops all requests; this is a correctness fixture.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
            sink.bind(("127.0.0.1", 0))
            p = client(
                sink.getsockname()[1], "--timeout-ms", "30", "--messages", "2", protocol="udp"
            )
        r = json.loads(p.stdout)
        self.assertEqual(p.returncode, 2)
        self.assertEqual(r["udp_echo_loss_percent"], 100)
        self.assertEqual(r["completed_messages"], 0)
        self.assertIsNone(r["rtt_mean_us"])

    def test_shutdown_active_connection(self):
        with server() as (proc, port):
            with socket.create_connection(("127.0.0.1", port)):
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=2)
                self.assertEqual(proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
