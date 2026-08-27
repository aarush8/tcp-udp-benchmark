# Recorded experiments

These directories retain actual local measurements and the source versions that produced them.

| Dataset | Configurations × repetitions | Duration setting | Measured trials |
|---|---:|---:|---:|
| `baseline/` | 19 × 3 | 0.5 s | 57 |
| `comparison/` | 24 × 5 | 0.6 s | 120 |
| `survey/` | 28 × 3 | 0.6 s | 84 |
| `confirmation/` | 8 × 5 | 3 s | 40 |

Each dataset contains:

- `trials.jsonl`: original per-trial measurements, commands, stdout/stderr, and exit status.
- `warmups.jsonl`: separate warmup runs, excluded from the measured trial count.
- `environment.json` and `plan.json`: machine/build details and campaign configuration.
- `source.tar.gz`: historical implementation and scripts for that campaign.

All 301 recorded trials completed successfully. This does not mean every implementation approach improved performance. The initial baseline has a documented timing-window issue; its records remain intact. See [analysis](../docs/ANALYSIS.md) and [methodology](../docs/METHODOLOGY.md).

`plots/` contains selected PNG figures and trial counts. `diagnostics/` contains separate syscall traces; ptrace timings are not included in benchmark comparisons. Archives are deliberately retained because the measured implementations predate subsequent hardening and refactoring. Current source is not silently substituted for a historical snapshot.

CSV duplicates, SVG exports, aggregate summary tables, local validation logs, progress logs, and newly named local campaigns are ignored. They remain on disk when generated. To regenerate plots and aggregate tables from the retained records:

```bash
python3 scripts/analyze.py results/baseline results/comparison results/survey \
  results/confirmation --output results/local-plots
```

Run that command from the repository root with matplotlib installed. No new benchmark run is needed to analyze these records. For future measurements, use a fresh output directory and the commands in the main README; timings will vary by machine and load.
