#!/usr/bin/env python3
"""Summarize recorded trials and plot comparisons with matching workload controls."""

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from experiment_utils import trial_succeeded

METRICS = (
    "goodput_mbps",
    "rtt_mean_us",
    "rtt_median_us",
    "rtt_p95_us",
    "rtt_p99_us",
    "client_cpu_percent",
    "completed_messages",
    "rtt_samples",
    "udp_echo_loss_percent",
)


def load(directories):
    rows = []
    for directory in directories:
        plan_path = directory / "plan.json"
        profile = (
            json.loads(plan_path.read_text()).get("profile", directory.name)
            if plan_path.exists()
            else directory.name
        )
        for line in (directory / "trials.jsonl").read_text().splitlines():
            record = json.loads(line)
            measurement = record.get("measurement") or {}
            config = dict(measurement.get("config", record["requested_config"]))
            # The original baseline predates these explicit configuration fields.
            config.setdefault("write", "split")
            config.setdefault("buffering", "allocate")
            rows.append(
                {
                    "dataset": directory.name,
                    "profile": profile,
                    "group": record["group"],
                    "repeat": record["repeat"],
                    "config": config,
                    "valid": trial_succeeded(record),
                    **{key: value for key, value in measurement.items() if key != "config"},
                }
            )
    return rows


def aggregate(rows):
    groups = defaultdict(list)
    for row in rows:
        config = {
            key: value
            for key, value in row["config"].items()
            if key not in ("port", "host", "iterations", "format")
        }
        groups[(row["dataset"], row["group"], json.dumps(config, sort_keys=True))].append(row)
    summaries = []
    for (dataset, group, config), trials in sorted(groups.items()):
        valid = [row for row in trials if row["valid"]]
        item = {
            "dataset": dataset,
            "group": group,
            "config": json.loads(config),
            "trials": len(trials),
            "valid_trials": len(valid),
            "failed_trials": len(trials) - len(valid),
            "metrics": {},
        }
        for metric in METRICS:
            values = [row[metric] for row in valid if row.get(metric) is not None]
            if values:
                item["metrics"][metric] = {
                    "n": len(values),
                    "mean": statistics.mean(values),
                    "median": statistics.median(values),
                    "min": min(values),
                    "max": max(values),
                    "stdev": statistics.stdev(values) if len(values) > 1 else 0,
                }
        summaries.append(item)
    return summaries


def select(rows, group=None, **config):
    return [
        row
        for row in rows
        if (group is None or row["group"] == group)
        and all(row["config"].get(key) == value for key, value in config.items())
    ]


def require_matching(rows, keys):
    for key in keys:
        values = {row["config"].get(key) for row in rows}
        if len(values) > 1:
            raise ValueError(
                f"cannot compare trials with different {key}: {values}; analyze matching campaigns separately"
            )


def validate_plot_controls(rows):
    controls = ("duration", "rate", "mbps", "messages", "timeout_ms")
    for profile in ("comparison", "survey", "confirmation"):
        trials = [row for row in rows if row["valid"] and row["profile"] == profile]
        require_matching(trials, controls)
        if profile != "survey":
            require_matching(trials, ("clients", "io", "sndbuf", "rcvbuf"))
        else:
            require_matching(trials, ("write", "buffering", "nodelay"))
    # Payload charts overlay the two profiles, so their shared controls must also match.
    payload = [
        row
        for row in rows
        if row["valid"]
        and (
            (row["profile"] == "survey" and row["group"] == "payload")
            or (
                row["profile"] == "comparison"
                and row["config"]["nodelay"] == 0
                and row["config"]["write"] == "split"
                and row["config"]["buffering"] == "allocate"
            )
        )
    ]
    require_matching(payload, (*controls, "clients", "io", "sndbuf", "rcvbuf", "nodelay"))


def series(ax, rows, x, metric, label, **style):
    groups = defaultdict(list)
    for row in rows:
        if row.get(metric) is not None:
            groups[row["config"][x]].append(row[metric])
    if not groups:
        return
    positions = sorted(groups)
    means = [statistics.mean(groups[position]) for position in positions]
    errors = [
        [mean - min(groups[position]) for position, mean in zip(positions, means)],
        [max(groups[position]) - mean for position, mean in zip(positions, means)],
    ]
    ax.errorbar(positions, means, yerr=errors, label=label, marker="o", capsize=3, **style)


def bars(ax, groups):
    labels, means, lower, upper = [], [], [], []
    missing = []
    for label, rows in groups:
        values = [row["rtt_mean_us"] for row in rows if row.get("rtt_mean_us") is not None]
        if not values:
            missing.append(label.replace("\n", "/"))
            continue
        mean = statistics.mean(values)
        labels.append(label)
        means.append(mean)
        lower.append(mean - min(values))
        upper.append(max(values) - mean)
    if means:
        ax.bar(labels, means, yerr=[lower, upper], capsize=3, color="#2b9c9c")
        ax.set_yscale("log")
    if missing:
        ax.text(
            0.02,
            0.95,
            "No successful trials: " + ", ".join(missing),
            transform=ax.transAxes,
            va="top",
            fontsize=8,
            wrap=True,
        )
    ax.grid(True, axis="y", alpha=0.2)


def save(fig, output, name, caption):
    fig.text(0.5, 0.015, caption, ha="center", fontsize=8)
    fig.tight_layout(rect=(0, 0.055, 1, 1))
    fig.savefig(output / f"{name}.png", dpi=160)
    fig.savefig(output / f"{name}.svg")
    plt.close(fig)


def plot_payload(survey, comparison, output):
    optimized = select(survey, group="payload")
    baseline = select(comparison, nodelay=0, write="split", buffering="allocate")
    if not optimized and not baseline:
        return
    config = (optimized or baseline)[0]["config"]
    for metric, name, ylabel in (
        ("rtt_mean_us", "latency_payload", "Mean logical RTT (µs)"),
        ("goodput_mbps", "throughput_payload", "Application goodput (Mbps)"),
    ):
        fig, ax = plt.subplots(figsize=(8, 4.8))
        for protocol in ("tcp", "udp"):
            series(
                ax,
                select(optimized, protocol=protocol),
                "payload",
                metric,
                f"{protocol.upper()} coalesced/reuse",
            )
        series(
            ax,
            baseline,
            "payload",
            metric,
            "TCP split/allocate (controlled baseline)",
            linestyle="--",
        )
        ax.set_xscale("log", base=2)
        if metric == "rtt_mean_us":
            ax.set_yscale("log")
        ax.set(
            xlabel="Logical payload (bytes)",
            ylabel=ylabel,
            title=f"Payload sweep: {config['clients']} client(s), request budget {config['mbps']:g} Mbps",
        )
        ax.grid(True, alpha=0.25)
        ax.legend(fontsize=8)
        save(
            fig,
            output,
            name,
            "Bars: min–max trial means. UDP >1,200 B uses paced fragment echoes; logical RTT includes pacing.",
        )


def plot_concurrency(survey, output):
    rows = select(survey, group="concurrency_io")
    if not rows:
        return
    fig, ax = plt.subplots(figsize=(8, 4.8))
    for protocol in ("tcp", "udp"):
        for mode in ("blocking", "nonblocking"):
            series(
                ax,
                select(rows, protocol=protocol, io=mode),
                "clients",
                "goodput_mbps",
                f"{protocol.upper()} {mode}",
            )
    ax.set(
        xlabel="Concurrent clients",
        ylabel="Application goodput (Mbps)",
        xticks=sorted({row["config"]["clients"] for row in rows}),
        title=f"Concurrency: {rows[0]['config']['payload']:,} B payload, same aggregate rate budget",
    )
    ax.grid(True, alpha=0.25)
    ax.legend(fontsize=8)
    save(
        fig,
        output,
        "throughput_clients",
        "Mean and min–max across trials. Nonblocking mode retains one worker per TCP connection.",
    )


def plot_cpu(survey, comparison, output):
    if survey:
        fig, ax = plt.subplots(figsize=(8, 4.8))
        for protocol in ("tcp", "udp"):
            rows = select(survey, protocol=protocol)
            ax.scatter(
                [row["goodput_mbps"] for row in rows],
                [row["client_cpu_percent"] for row in rows],
                alpha=0.55,
                s=24,
                label=protocol.upper(),
            )
        ax.set(
            xlabel="Application goodput (Mbps)",
            ylabel="Client CPU (% of one core)",
            title="CPU vs. goodput across survey workloads",
        )
        ax.grid(True, alpha=0.25)
        ax.legend()
        save(
            fig,
            output,
            "cpu_throughput",
            "Each point is a trial. Payload/concurrency/buffers vary; this scatter does not isolate a causal factor.",
        )
    if comparison:
        fig, ax = plt.subplots(figsize=(8, 4.8))
        for buffering in ("allocate", "reuse"):
            rows = [
                dict(
                    row,
                    client_cpu_us_per_message=row["client_cpu_s"] * 1e6 / row["completed_messages"],
                )
                for row in select(comparison, buffering=buffering, write="coalesced", nodelay=1)
            ]
            series(ax, rows, "payload", "client_cpu_us_per_message", buffering)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set(
            xlabel="Payload (bytes)",
            ylabel="Client CPU time / completed message (µs)",
            title="Reusable buffers: CPU trade-off under the same paced workload",
        )
        ax.grid(True, alpha=0.25)
        if ax.get_legend_handles_labels()[0]:
            ax.legend()
        save(
            fig,
            output,
            "buffer_cpu",
            "CPU includes worker work and pacing syscalls; mean and min–max across trials, not isolated allocator cost.",
        )


def plot_optimizations(comparison, output):
    if not comparison:
        return
    variants = [("split", "allocate"), ("coalesced", "allocate"), ("coalesced", "reuse")]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.8))
    for ax, size in zip(axes, (64, 1048576)):
        groups = [
            (
                f"{write}\n{buffering}",
                select(comparison, payload=size, nodelay=0, write=write, buffering=buffering),
            )
            for write, buffering in variants
        ]
        bars(ax, groups)
        ax.set(ylabel="Mean RTT (µs, log scale)", title=f"TCP {size:,} B; NODELAY off")
    save(
        fig,
        output,
        "optimizations",
        "Same workload except write/buffer options; bars show mean and min–max of trial means.",
    )

    rows = select(comparison, payload=64, nodelay=0)
    if rows:
        fig, ax = plt.subplots(figsize=(8, 4.8))
        for offset, (write, buffering) in enumerate(variants):
            trials = select(rows, write=write, buffering=buffering)
            ax.scatter(
                [row["repeat"] + offset * 0.04 for row in trials],
                [row["rtt_mean_us"] for row in trials],
                label=f"{write}/{buffering}",
            )
        ax.set_yscale("log")
        ax.set(
            xlabel="Repetition index (randomized execution order)",
            ylabel="Per-trial mean RTT (µs)",
            title="Repeated trials: 64-byte TCP, NODELAY off",
        )
        ax.grid(True, alpha=0.25)
        ax.legend()
        save(
            fig,
            output,
            "trial_variation",
            "Tail quantiles from the slow split-write case have few samples; each point is a trial mean.",
        )


def plot_socket_buffers(survey, output):
    rows = select(survey, group="buffers")
    if not rows:
        return
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.8))
    for protocol, ax in zip(("tcp", "udp"), axes):
        series(ax, select(rows, protocol=protocol), "sndbuf", "rtt_mean_us", protocol.upper())
        ax.set_xscale("log", base=2)
        ax.set(
            xlabel="Requested send AND receive buffer (bytes)",
            ylabel="Mean RTT (µs)",
            title=protocol.upper(),
        )
        ax.grid(True, alpha=0.25)
    save(
        fig,
        output,
        "socket_buffers",
        f"{rows[0]['config']['payload']:,} B payload. Effective Linux buffers are recorded separately in raw JSON.",
    )


def plot_confirmation(rows, output):
    if not rows:
        return
    cases = [
        (
            64,
            [
                (0, "split", "allocate"),
                (0, "coalesced", "allocate"),
                (1, "split", "allocate"),
                (1, "coalesced", "allocate"),
            ],
            ["split\nND off", "coalesced\nND off", "split\nND on", "coalesced\nND on"],
        ),
        (1048576, [(1, "coalesced", "allocate"), (1, "coalesced", "reuse")], ["allocate", "reuse"]),
    ]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.8))
    for ax, (size, variants, labels) in zip(axes, cases):
        groups = [
            (
                label,
                select(
                    rows,
                    protocol="tcp",
                    payload=size,
                    nodelay=nodelay,
                    write=write,
                    buffering=buffering,
                ),
            )
            for label, (nodelay, write, buffering) in zip(labels, variants)
        ]
        bars(ax, groups)
        ax.set(ylabel="Mean RTT (µs, log scale)", title=f"Confirmation: TCP {size:,} B")
    save(
        fig,
        output,
        "confirmation",
        f"Trial duration {rows[0]['config']['duration']:g} s; mean and min–max trial means. ND = TCP_NODELAY.",
    )


def plots(rows, output):
    valid = [row for row in rows if row["valid"]]
    survey = [row for row in valid if row["profile"] == "survey"]
    comparison = [row for row in valid if row["profile"] == "comparison"]
    confirmation = [row for row in valid if row["profile"] == "confirmation"]
    plot_payload(survey, comparison, output)
    plot_concurrency(survey, output)
    plot_cpu(survey, comparison, output)
    plot_optimizations(comparison, output)
    plot_socket_buffers(survey, output)
    plot_confirmation(confirmation, output)


def write_tables(rows, output):
    summaries = aggregate(rows)
    (output / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
    with (output / "summary.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "dataset",
                "group",
                "config",
                "trials",
                "valid_trials",
                "failed_trials",
                "metric",
                "mean",
                "min",
                "max",
                "stdev",
            ]
        )
        for item in summaries:
            for metric, values in item["metrics"].items():
                writer.writerow(
                    [
                        item["dataset"],
                        item["group"],
                        json.dumps(item["config"], sort_keys=True),
                        item["trials"],
                        item["valid_trials"],
                        item["failed_trials"],
                        metric,
                        values["mean"],
                        values["min"],
                        values["max"],
                        values["stdev"],
                    ]
                )
    counts = {}
    for row in rows:
        count = counts.setdefault(row["dataset"], {"total": 0, "failed": 0})
        count["total"] += 1
        count["failed"] += int(not row["valid"])
    (output / "counts.json").write_text(json.dumps(counts, indent=2) + "\n")
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=Path("results/plots"))
    args = parser.parse_args()
    rows = load(args.directories)
    try:
        validate_plot_controls(rows)
    except ValueError as error:
        parser.error(str(error))
    args.output.mkdir(parents=True, exist_ok=True)
    counts = write_tables(rows, args.output)
    plots(rows, args.output)
    print(json.dumps(counts, indent=2))
    print(f"Plots and summary tables: {args.output}")


if __name__ == "__main__":
    main()
