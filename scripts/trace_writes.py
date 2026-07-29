#!/usr/bin/env python3
"""Optional syscall diagnostic. Never mix strace timing with benchmark performance trials."""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_experiments import options

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,default=Path('build'))
    ap.add_argument('--output',type=Path,default=Path('results/diagnostics'))
    args=ap.parse_args(); args.output.mkdir(parents=True,exist_ok=False)
    for write in ('split','coalesced'):
        server_cmd=[str(args.build/'netbench_server'),'--port','0','--duration','5']
        server=subprocess.Popen(server_cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            ready=json.loads(server.stdout.readline())
            cmd=['strace','-f','-tt','-T','-e','trace=sendto,recvfrom,poll', '-o',str(args.output/f'{write}.strace'),
                 str(args.build/'netbench_client'),*options(dict(port=ready['port'],write=write,payload=64,messages=8,duration=1,rate=10000,nodelay=0,buffering='allocate'))]
            p=subprocess.run(cmd,capture_output=True,text=True,timeout=5)
            (args.output/f'{write}.json').write_text(json.dumps(dict(command=cmd,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr),indent=2)+'\n')
        finally:
            server.send_signal(signal.SIGTERM)
            out,err=server.communicate(timeout=3)
            (args.output/f'{write}.server.json').write_text(json.dumps(dict(command=server_cmd,stdout=out,stderr=err,returncode=server.returncode),indent=2)+'\n')
if __name__=='__main__':
    main()
