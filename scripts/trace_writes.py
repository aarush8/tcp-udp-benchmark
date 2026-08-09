#!/usr/bin/env python3
"""Inspect syscall counts separately from timed benchmark trials. Requires strace."""

import argparse
import json
import shutil
import subprocess
from pathlib import Path

from experiment_utils import read_ready_line, stop_server
from run_experiments import options


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    parser.add_argument("--output", type=Path, default=Path("results/diagnostics"))
    args = parser.parse_args()
    if shutil.which("strace") is None:
        parser.error("strace is required for syscall diagnostics")
    args.output.mkdir(parents=True, exist_ok=False)
    failed = False
    for write in ("split", "coalesced"):
        server_cmd = [str(args.build / "netbench_server"), "--port", "0", "--duration", "5"]
        server = subprocess.Popen(
            server_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        prefix = ""
        try:
            prefix = read_ready_line(server)
            ready = json.loads(prefix)
            config = dict(
                port=ready["port"],
                write=write,
                payload=64,
                messages=8,
                duration=1,
                rate=10000,
                nodelay=0,
                buffering="allocate",
            )
            command = [
                "strace",
                "-f",
                "-tt",
                "-T",
                "-e",
                "trace=sendto,recvfrom,poll",
                "-o",
                str(args.output / f"{write}.strace"),
                str(args.build / "netbench_client"),
                *options(config),
            ]
            result = subprocess.run(command, capture_output=True, text=True, timeout=5)
            failed |= result.returncode != 0
            record = dict(
                command=command,
                returncode=result.returncode,
                stdout=result.stdout,
                stderr=result.stderr,
            )
            (args.output / f"{write}.json").write_text(json.dumps(record, indent=2) + "\n")
        finally:
            summary = stop_server(server)
            failed |= summary["server_returncode"] != 0
            summary["server_stdout"] = prefix + summary["server_stdout"]
            summary["command"] = server_cmd
            (args.output / f"{write}.server.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 2 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
