#!/usr/bin/env python3
"""Produce repeat-aware plots and auditable aggregate tables from preserved trial JSONL."""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import statistics as st

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

METRICS = ['goodput_mbps', 'rtt_mean_us', 'rtt_median_us', 'rtt_p95_us', 'rtt_p99_us',
           'client_cpu_percent', 'completed_messages', 'rtt_samples', 'udp_echo_loss_percent']

def load(directories):
    rows = []
    for directory in directories:
        plan_path=directory/'plan.json'
        profile=json.loads(plan_path.read_text()).get('profile', directory.name) if plan_path.exists() else directory.name
        for line in (directory/'trials.jsonl').read_text().splitlines():
            r = json.loads(line)
            m = r.get('measurement')
            if m:
                c = dict(m['config'])
                c.setdefault('write', 'split'); c.setdefault('buffering', 'allocate')
                rows.append(dict(dataset=directory.name, profile=profile, group=r['group'], repeat=r['repeat'], config=c,
                                 valid=(r.get('client_returncode') == 0 and r.get('server_returncode') == 0
                                        and m['status'] == 'ok' and r.get('server_summary', {}).get('errors', 0) == 0),
                                 **{k: v for k, v in m.items() if k != 'config'}))
            else:
                rows.append(dict(dataset=directory.name, profile=profile, group=r['group'], valid=False, config=r['requested_config']))
    return rows

def aggregate(rows):
    groups = defaultdict(list)
    for r in rows:
        c = {k: v for k, v in r['config'].items() if k not in ('port', 'host', 'iterations')}
        groups[(r['dataset'], r['group'], json.dumps(c, sort_keys=True))].append(r)
    out = []
    for (dataset, group, c), values in sorted(groups.items()):
        good = [r for r in values if r['valid']]
        item = dict(dataset=dataset, group=group, config=json.loads(c), trials=len(values),
                    valid_trials=len(good), failed_trials=len(values)-len(good), metrics={})
        for metric in METRICS:
            nums = [r[metric] for r in good if r.get(metric) is not None]
            if nums:
                item['metrics'][metric] = dict(n=len(nums), mean=st.mean(nums), median=st.median(nums),
                                              min=min(nums), max=max(nums), stdev=st.stdev(nums) if len(nums)>1 else 0)
        out.append(item)
    return out

def series(ax, rows, x, y, label, **kwargs):
    groups = defaultdict(list)
    for r in rows:
        value = r.get(y)
        if value is not None:
            groups[r['config'][x]].append(value)
    if not groups:
        return
    xx = sorted(groups); yy = [st.mean(groups[n]) for n in xx]
    err = [[mean-min(groups[n]) for n, mean in zip(xx, yy)], [max(groups[n])-mean for n, mean in zip(xx, yy)]]
    ax.errorbar(xx, yy, yerr=err, label=label, marker='o', capsize=3, **kwargs)

def save(fig, output, name, caption):
    fig.text(.5, .015, caption, ha='center', fontsize=8)
    fig.tight_layout(rect=(0, .055, 1, 1))
    fig.savefig(output/f'{name}.png', dpi=160)
    fig.savefig(output/f'{name}.svg')
    plt.close(fig)

def plots(rows, output):
    valid = [r for r in rows if r['valid']]
    survey = [r for r in valid if r['profile']=='survey']
    comparison = [r for r in valid if r['profile']=='comparison']
    for metric, name, ylabel in [('rtt_mean_us', 'latency_payload', 'Mean logical RTT (µs)'),
                                  ('goodput_mbps', 'throughput_payload', 'Application goodput (Mbps)')]:
        fig, ax = plt.subplots(figsize=(8, 4.8))
        for proto in ('tcp', 'udp'):
            data = [r for r in survey if r['group']=='payload' and r['config']['protocol']==proto]
            series(ax, data, 'payload', metric, f'{proto.upper()} coalesced/reuse')
        baseline = [r for r in comparison if r['config']['nodelay']==0 and r['config']['write']=='split'
                    and r['config']['buffering']=='allocate']
        series(ax, baseline, 'payload', metric, 'TCP split/allocate (controlled baseline)', linestyle='--')
        ax.set_xscale('log', base=2)
        if metric=='rtt_mean_us':
            ax.set_yscale('log')
        ax.set(xlabel='Logical payload (bytes)', ylabel=ylabel, title='Payload sweep: one client, aggregate request cap 100 Mbps')
        ax.grid(True, alpha=.25); ax.legend(fontsize=8)
        save(fig, output, name, 'Bars: min–max trial means. UDP >1,200 B uses paced fragment echoes; its logical RTT includes pacing.')

    fig, ax = plt.subplots(figsize=(8, 4.8))
    for proto in ('tcp', 'udp'):
        for mode in ('blocking', 'nonblocking'):
            data = [r for r in survey if r['group']=='concurrency_io' and r['config']['protocol']==proto and r['config']['io']==mode]
            series(ax, data, 'clients', 'goodput_mbps', f'{proto.upper()} {mode}')
    ax.set(xlabel='Concurrent clients', ylabel='Application goodput (Mbps)', xticks=[1,4,8],
           title='Concurrency: 1 KiB payload, same aggregate rate budget')
    ax.grid(True, alpha=.25); ax.legend(fontsize=8)
    save(fig, output, 'throughput_clients', 'Mean and min–max across trials. Nonblocking mode retains one worker per TCP connection.')

    fig, ax = plt.subplots(figsize=(8, 4.8))
    for proto in ('tcp', 'udp'):
        data = [r for r in survey if r['config']['protocol']==proto]
        ax.scatter([r['goodput_mbps'] for r in data], [r['client_cpu_percent'] for r in data], alpha=.55, s=24, label=proto.upper())
    ax.set(xlabel='Application goodput (Mbps)', ylabel='Client CPU (% of one core)',
           title='CPU vs. goodput across measured survey workloads')
    ax.grid(True, alpha=.25); ax.legend()
    save(fig, output, 'cpu_throughput', 'Each point is a trial. Payload/concurrency/buffers vary; this scatter does not isolate a causal factor.')

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.8))
    variants = [('split', 'allocate'), ('coalesced', 'allocate'), ('coalesced', 'reuse')]
    labels = ['split\nallocate', 'coalesced\nallocate', 'coalesced\nreuse']
    for ax, size in zip(axes, (64, 1048576)):
        data = [r for r in comparison if r['config']['payload']==size and r['config']['nodelay']==0]
        means, low, high = [], [], []
        for write, buffering in variants:
            nums = [r['rtt_mean_us'] for r in data if r['config']['write']==write and r['config']['buffering']==buffering]
            if not nums:
                continue
            mean=st.mean(nums); means.append(mean); low.append(mean-min(nums)); high.append(max(nums)-mean)
        if len(means)==3:
            ax.bar(labels, means, yerr=[low, high], capsize=3, color=['#637a96', '#2b9c9c', '#bc8034'])
        ax.set_yscale('log'); ax.set(ylabel='Mean RTT (µs, log scale)', title=f'TCP payload {size:,} B; NODELAY off')
        ax.grid(True, axis='y', alpha=.2)
    save(fig, output, 'optimizations', 'Identical code/harness and workload except write/buffer options; bars show mean and min–max of trial means.')

    fig, ax = plt.subplots(figsize=(8, 4.8))
    for buffering in ('allocate', 'reuse'):
        data = [dict(r, client_cpu_us_per_message=r['client_cpu_s']*1e6/r['completed_messages']) for r in comparison
                if r['config']['buffering']==buffering and r['config']['write']=='coalesced'
                and r['config']['nodelay']==1 and r['completed_messages']]
        series(ax, data, 'payload', 'client_cpu_us_per_message', buffering)
    ax.set_xscale('log', base=2); ax.set_yscale('log')
    ax.set(xlabel='Payload (bytes)', ylabel='Client CPU time / completed message (µs)', title='Reusable buffers: CPU trade-off under the same paced workload')
    ax.grid(True, alpha=.25); ax.legend()
    save(fig, output, 'buffer_cpu', 'CPU includes process worker work and pacing syscalls; mean and min–max across trials, not profiler allocation costs.')

    fig, axes = plt.subplots(1,2,figsize=(10,4.8))
    for proto, ax in zip(('tcp','udp'), axes):
        data=[r for r in survey if r['group']=='buffers' and r['config']['protocol']==proto]
        series(ax,data,'sndbuf','rtt_mean_us',proto.upper())
        ax.set_xscale('log',base=2); ax.set(xlabel='Requested send AND receive buffer (bytes)',ylabel='Mean RTT (µs)',title=proto.upper())
        ax.grid(True,alpha=.25)
    save(fig, output, 'socket_buffers', '16 KiB payload, one client. Effective Linux buffers are recorded separately in raw JSON.')

    fig, ax = plt.subplots(figsize=(8, 4.8))
    for offset, (write, buffering) in enumerate(variants):
        data = [r for r in comparison if r['config']['payload']==64 and r['config']['nodelay']==0
                and r['config']['write']==write and r['config']['buffering']==buffering]
        ax.scatter([r['repeat']+offset*.04 for r in data], [r['rtt_mean_us'] for r in data], label=f'{write}/{buffering}')
    ax.set_yscale('log'); ax.set(xlabel='Repetition index (randomized execution order)',ylabel='Per-trial mean RTT (µs)',title='Repeated trials: 64-byte TCP, NODELAY off')
    ax.grid(True, alpha=.25); ax.legend()
    save(fig, output, 'trial_variation', 'Short trials are exploratory. Tail quantiles from the slow split-write case have very few samples.')

    confirm=[r for r in valid if r['profile']=='confirmation']
    if confirm:
        fig,axes=plt.subplots(1,2,figsize=(11,4.8))
        cases=[(64,[(0,'split','allocate'),(0,'coalesced','allocate'),(1,'split','allocate'),(1,'coalesced','allocate')],
                   ['split\nND off','coalesced\nND off','split\nND on','coalesced\nND on']),
               (1048576,[(1,'coalesced','allocate'),(1,'coalesced','reuse')],['allocate','reuse'])]
        for ax,(size,variants,labels) in zip(axes,cases):
            means,low,high=[],[],[]
            for nd,write,buffering in variants:
                nums=[r['rtt_mean_us'] for r in confirm if r['config']['protocol']=='tcp' and r['config']['payload']==size
                      and r['config']['nodelay']==nd and r['config']['write']==write and r['config']['buffering']==buffering]
                mean=st.mean(nums); means.append(mean); low.append(mean-min(nums)); high.append(max(nums)-mean)
            ax.bar(labels,means,yerr=[low,high],capsize=3,color='#2b9c9c')
            ax.set_yscale('log'); ax.set(ylabel='Mean RTT (µs, log scale)',title=f'Confirmation: TCP {size:,} B')
            ax.grid(True,axis='y',alpha=.2)
        save(fig,output,'confirmation','Five 3 s trials per configuration; bars are mean and min–max trial means. ND = TCP_NODELAY.')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directories', type=Path, nargs='+')
    ap.add_argument('--output', type=Path, default=Path('results/plots'))
    args=ap.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    rows=load(args.directories); summary=aggregate(rows)
    (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    with (args.output/'summary.csv').open('w', newline='') as f:
        writer=csv.writer(f); writer.writerow(['dataset','group','config','trials','valid_trials','failed_trials','metric','mean','min','max','stdev'])
        for item in summary:
            for metric,m in item['metrics'].items():
                writer.writerow([item['dataset'],item['group'],json.dumps(item['config'],sort_keys=True),item['trials'],item['valid_trials'],item['failed_trials'],metric,m['mean'],m['min'],m['max'],m['stdev']])
    counts={name:dict(total=sum(r['dataset']==name for r in rows),failed=sum(r['dataset']==name and not r['valid'] for r in rows)) for name in sorted({r['dataset'] for r in rows})}
    (args.output/'counts.json').write_text(json.dumps(counts,indent=2)+'\n')
    plots(rows,args.output)
    print(json.dumps(counts,indent=2)); print(f'Plots and summary tables: {args.output}')

if __name__=='__main__':
    main()
