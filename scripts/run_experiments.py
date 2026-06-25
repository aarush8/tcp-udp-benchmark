#!/usr/bin/env python3
"""Run bounded, randomized localhost trials. Raw failures are preserved, never filtered away."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import selectors
import signal
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]

def capture(args):
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=5)
        return {'command': args, 'returncode': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr}
    except (OSError, subprocess.TimeoutExpired) as e:
        return {'command': args, 'error': str(e)}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def environment(build):
    files = ['/proc/cpuinfo', '/proc/meminfo', '/proc/loadavg',
             '/proc/sys/net/core/rmem_max', '/proc/sys/net/core/wmem_max',
             '/proc/sys/net/ipv4/tcp_rmem', '/proc/sys/net/ipv4/tcp_wmem',
             '/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor',
             '/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq',
             '/proc/sys/kernel/perf_event_paranoid']
    return {'timestamp_utc': datetime.now(timezone.utc).isoformat(), 'platform': platform.uname()._asdict(),
            'python': sys.version, 'cpu_affinity': sorted(os.sched_getaffinity(0)),
            'files': {p: Path(p).read_text() if Path(p).exists() else None for p in files},
            'compiler': capture(['c++', '--version']), 'cmake': capture(['cmake', '--version']),
            'git_head': capture(['git', '-C', str(ROOT), 'rev-parse', 'HEAD']),
            'git_status': capture(['git', '-C', str(ROOT), 'status', '--porcelain=v1']),
            'binaries': {p.name: sha(p) for p in (build/'netbench_client', build/'netbench_server')},
            'cmake_cache': (build/'CMakeCache.txt').read_text()}

def matrix(profile):
    base = dict(protocol='tcp', payload=1024, clients=1, io='blocking', nodelay=0, sndbuf=0, rcvbuf=0)
    cases = []
    def add(group, **changes):
        cases.append((group, base | changes))
    if profile == 'baseline':
        for proto, size in itertools.product(('tcp', 'udp'), (64, 1024, 16384, 65536, 1048576)):
            add('payload', protocol=proto, payload=size)
        for mode, clients in itertools.product(('blocking', 'nonblocking'), (1, 4, 8)):
            add('concurrency_io', io=mode, clients=clients, nodelay=1)
        for buf in (4096, 262144):
            add('buffers', sndbuf=buf, rcvbuf=buf, nodelay=1, payload=16384)
        add('nodelay', nodelay=1)
    elif profile == 'comparison':
        for size, nodelay, write, buffering in itertools.product((64, 65536, 1048576), (0, 1), ('split', 'coalesced'), ('allocate', 'reuse')):
            add('optimization', payload=size, nodelay=nodelay, write=write, buffering=buffering)
    elif profile == 'survey':
        base |= dict(write='coalesced', buffering='reuse')
        for proto, size in itertools.product(('tcp', 'udp'), (64, 1024, 16384, 65536, 1048576)):
            add('payload', protocol=proto, payload=size)
        for proto, mode, clients in itertools.product(('tcp', 'udp'), ('blocking', 'nonblocking'), (1, 4, 8)):
            add('concurrency_io', protocol=proto, io=mode, clients=clients)
        for proto, buf in itertools.product(('tcp', 'udp'), (4096, 65536, 262144)):
            add('buffers', protocol=proto, sndbuf=buf, rcvbuf=buf, payload=16384)
    return cases

def options(config):
    return list(itertools.chain.from_iterable((f'--{k.replace("_", "-")}', str(v)) for k, v in config.items()))

def trial(build, config, duration, rate, mbps):
    server_keys = ('protocol', 'clients', 'io', 'nodelay', 'sndbuf', 'rcvbuf', 'buffering')
    sc = {k: config[k] for k in server_keys if k in config}
    server_cmd = [str(build/'netbench_server'), '--host', '127.0.0.1', '--port', '0',
                  '--duration', str(min(60, duration+8)), *options(sc)]
    proc = subprocess.Popen(server_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    record = {'timestamp_utc': datetime.now(timezone.utc).isoformat(), 'server_command': server_cmd,
              'loadavg_before': os.getloadavg(), 'measurement': None}
    server_prefix = ''
    try:
        with selectors.DefaultSelector() as sel:
            sel.register(proc.stdout, selectors.EVENT_READ)
            if not sel.select(5):
                raise RuntimeError('server readiness timeout')
        server_prefix = proc.stdout.readline()
        ready = json.loads(server_prefix)
        cmd = [str(build/'netbench_client'), '--host', '127.0.0.1', '--port', str(ready['port']),
               '--duration', str(duration), '--messages', '100000', '--rate', str(rate),
               '--mbps', str(mbps), *options(config)]
        record['client_command'] = cmd
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=duration+8)
        record |= dict(client_returncode=p.returncode, client_stdout=p.stdout, client_stderr=p.stderr)
        try:
            record['measurement'] = json.loads(p.stdout)
        except json.JSONDecodeError:
            record['parse_error'] = 'client did not emit one valid JSON result'
        record['server_ready'] = ready
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as e:
        record['runner_error'] = str(e)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            out, err = proc.communicate(timeout=7)
        except subprocess.TimeoutExpired:
            proc.kill(); out, err = proc.communicate(); record['server_killed'] = True
        record |= dict(server_returncode=proc.returncode, server_stdout=server_prefix+out, server_stderr=err)
        lines = out.splitlines()
        if lines:
            try:
                record['server_summary'] = json.loads(lines[-1])
            except json.JSONDecodeError:
                pass
    return record

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT/'build')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--profile', choices=['baseline'], default='baseline')
    ap.add_argument('--repeats', type=int, default=5)
    ap.add_argument('--duration', type=float, default=.5)
    ap.add_argument('--rate', type=int, default=10000)
    ap.add_argument('--mbps', type=float, default=100)
    ap.add_argument('--seed', type=int, default=20261115)
    ap.add_argument('--warmups', type=int, default=1)
    args = ap.parse_args()
    if not (1 <= args.repeats <= 100 and .05 <= args.duration <= 30 and 0 <= args.warmups <= 3
            and 1 <= args.rate <= 100000 and .01 <= args.mbps <= 1000):
        ap.error('limits: repeats 1..100; duration .05..30; warmups 0..3; rate 1..100000; Mbps .01..1000')
    args.build = args.build.resolve()
    args.output.mkdir(parents=True, exist_ok=False)  # Never overwrite an earlier experiment.
    env = environment(args.build)
    (args.output/'environment.json').write_text(json.dumps(env, indent=2)+'\n')
    (args.output/'plan.json').write_text(json.dumps(vars(args), default=str, indent=2)+'\n')
    with tarfile.open(args.output/'source.tar.gz', 'w:gz') as archive:
        for dirname in ('src', 'include', 'scripts', 'tests'):
            archive.add(ROOT/dirname, arcname=dirname, filter=lambda info: None if '__pycache__' in info.name else info)
        archive.add(ROOT/'CMakeLists.txt', arcname='CMakeLists.txt')
    cases = matrix(args.profile)
    rng = random.Random(args.seed)
    jobs = [(group, conf, rep) for group, conf in cases for rep in range(args.repeats)]
    rng.shuffle(jobs)
    with (args.output/'warmups.jsonl').open('w') as f:
        for rep in range(args.warmups):
            conf = dict(protocol='tcp', payload=1024, clients=1, io='blocking', nodelay=1)
            r = trial(args.build, conf, .1, args.rate, args.mbps)
            f.write(json.dumps(r)+'\n'); f.flush()
    failures = 0
    with (args.output/'trials.jsonl').open('w') as f:
        for index, (group, conf, rep) in enumerate(jobs):
            r = trial(args.build, conf, args.duration, args.rate, args.mbps)
            r |= dict(index=index, group=group, repeat=rep, requested_config=conf,
                      environment_sha256=sha(args.output/'environment.json'))
            f.write(json.dumps(r)+'\n'); f.flush()
            bad = r.get('client_returncode') != 0 or r.get('server_returncode') != 0 or r.get('server_summary', {}).get('errors', 0) != 0
            failures += int(bad)
            print(f'{index+1}/{len(jobs)} {group} {conf} status={r.get("measurement", {}).get("status") if r.get("measurement") else "runner_error"}', flush=True)
    rows = []
    for line in (args.output/'trials.jsonl').read_text().splitlines():
        r = json.loads(line); m = r['measurement']
        if m:
            rows.append({'index': r['index'], 'group': r['group'], 'repeat': r['repeat'], **m['config'],
                         **{k: v for k, v in m.items() if not isinstance(v, (dict, list))},
                         'server_cpu_s': r.get('server_summary', {}).get('cpu_s')})
    if rows:
        with (args.output/'trials.csv').open('w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    print(f'Preserved {len(jobs)} measured trials, {failures} failed/incomplete trials in {args.output}')
    return 2 if failures else 0

if __name__ == '__main__':
    sys.exit(main())
