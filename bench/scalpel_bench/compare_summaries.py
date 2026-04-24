from __future__ import annotations

import json
import math
import statistics
import sys
from pathlib import Path


BASELINE_PREFIX = "baseline"
OPTIMIZED_PREFIX = "optimized"
# Core metrics to emphasize in the paper/report.
METRICS = [
    "elapsed_sec",
    "user_time_sec",
    "system_time_sec",
    "cpu_time_sec",
    "cpu_percent",
    "max_rss_mb",
    "minor_page_faults",
    "major_page_faults",
    "voluntary_context_switches",
    "involuntary_context_switches",
    "file_system_inputs",
    "file_system_outputs",
    "carved_file_count",
    "carved_total_mb",
    "initialized_queue_count",
    "queue_table_count",
    "queue_initialization_ratio",
    "continue_carve_entries",
    "initialized_block_count",
    "event_table_count",
    "block_activation_ratio",
    "continue_span_block_count",
    "pass1_scan_sec",
    "queue_build_sec",
    "pass2_read_sec",
    "pass2_write_sec",
    "pass2_open_close_sec",
    "pass2_total_sec",
    "handle_open_count",
    "handle_reopen_count",
    "handle_close_count",
    "handle_eviction_count",
    "peak_open_handles",
    "peak_active_carves",
]
# Metrics that are usually better when lower.
# user_time_sec, cpu_percent, carved_total_mb, etc. remain contextual because
# they need interpretation rather than simple "lower is better" scoring.
LOWER_IS_BETTER = {
    "elapsed_sec",
    "system_time_sec",
    "max_rss_mb",
    "minor_page_faults",
    "major_page_faults",
    "voluntary_context_switches",
    "involuntary_context_switches",
    "pass1_scan_sec",
    "queue_build_sec",
    "pass2_read_sec",
    "pass2_write_sec",
    "pass2_open_close_sec",
    "pass2_total_sec",
}


def load_variant_runs(root: Path, prefix: str) -> list[dict[str, object]]:
    runs: list[dict[str, object]] = []
    for child in sorted(root.iterdir()):
        if not child.is_dir() or not child.name.startswith(prefix):
            continue
        summary_path = child / "summary.json"
        if not summary_path.exists():
            continue
        payload = json.loads(summary_path.read_text(encoding="utf-8-sig"))
        payload["label"] = child.name
        runs.append(payload)
    return runs


def numeric_values(runs: list[dict[str, object]], metric: str) -> list[float]:
    values: list[float] = []
    for run in runs:
        value = run.get(metric)
        if isinstance(value, (int, float)):
            values.append(float(value))
    return values


def summarize_metric(runs: list[dict[str, object]], metric: str) -> dict[str, object] | None:
    values = numeric_values(runs, metric)
    if not values:
        return None
    summary: dict[str, object] = {
        "n": len(values),
        # mean: default value for tables and body text
        "mean": round(statistics.mean(values), 4),
        "min": round(min(values), 4),
        "max": round(max(values), 4),
    }
    if len(values) >= 2:
        # stdev: run-to-run stability
        summary["stdev"] = round(statistics.stdev(values), 4)
    return summary


def compare_metrics(
    baseline_runs: list[dict[str, object]], optimized_runs: list[dict[str, object]]
) -> dict[str, dict[str, object]]:
    comparison: dict[str, dict[str, object]] = {}
    for metric in METRICS:
        baseline = summarize_metric(baseline_runs, metric)
        optimized = summarize_metric(optimized_runs, metric)
        if not baseline or not optimized:
            continue
        baseline_mean = float(baseline["mean"])
        optimized_mean = float(optimized["mean"])
        delta = round(optimized_mean - baseline_mean, 4)
        delta_pct = None
        if baseline_mean != 0:
            delta_pct = round((delta / baseline_mean) * 100, 2)
        comparison[metric] = {
            "baseline": baseline,
            "optimized": optimized,
            # delta: optimized - baseline
            "delta": delta,
            "delta_pct": delta_pct,
            "direction": "lower_is_better" if metric in LOWER_IS_BETTER else "contextual",
        }
    return comparison


def build_observations(comparison: dict[str, dict[str, object]]) -> list[str]:
    # Generate short first-pass interpretations for quick review.
    # Detailed causal analysis still belongs in the paper text.
    observations: list[str] = []

    elapsed = comparison.get("elapsed_sec")
    if elapsed and elapsed.get("delta_pct") is not None:
        delta_pct = float(elapsed["delta_pct"])
        if delta_pct < 0:
            observations.append(
                f"Optimized elapsed time improved by {abs(delta_pct):.2f}% versus baseline."
            )
        elif delta_pct > 0:
            observations.append(
                f"Optimized elapsed time regressed by {delta_pct:.2f}% versus baseline."
            )

    user_time = comparison.get("user_time_sec")
    if user_time and user_time.get("delta_pct") is not None:
        delta_pct = float(user_time["delta_pct"])
        if delta_pct > 0:
            observations.append(
                f"User CPU time increased by {delta_pct:.2f}%, which is consistent with extra bookkeeping replacing broader carve work."
            )

    rss = comparison.get("max_rss_mb")
    if rss and rss.get("delta_pct") is not None:
        delta_pct = float(rss["delta_pct"])
        observations.append(
            f"Peak RSS changed by {delta_pct:.2f}%; this suggests the optimization mostly redistributes work instead of materially changing the peak live data footprint."
        )

    queues = comparison.get("queue_initialization_ratio")
    if queues and queues.get("delta_pct") is not None:
        baseline_mean = float(queues["baseline"]["mean"])
        optimized_mean = float(queues["optimized"]["mean"])
        if not math.isclose(baseline_mean, optimized_mean):
            observations.append(
                f"Initialized queue ratio moved from {baseline_mean:.4f} to {optimized_mean:.4f}, showing how much of the queue table was truly needed."
            )

    involuntary = comparison.get("involuntary_context_switches")
    if involuntary and involuntary.get("delta_pct") is not None:
        delta_pct = float(involuntary["delta_pct"])
        if delta_pct < 0:
            observations.append(
                f"Involuntary context switches dropped by {abs(delta_pct):.2f}%, which often aligns with shorter wait-heavy sections."
            )

    return observations


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: python compare_summaries.py <results_root>", file=sys.stderr)
        return 1

    root = Path(sys.argv[1])
    baseline_runs = load_variant_runs(root, BASELINE_PREFIX)
    optimized_runs = load_variant_runs(root, OPTIMIZED_PREFIX)
    if not baseline_runs or not optimized_runs:
        print(
            json.dumps(
                {
                    "error": "Could not find both baseline and optimized summary.json files under the results root."
                },
                ensure_ascii=False,
                indent=2,
            )
        )
        return 2

    comparison = compare_metrics(baseline_runs, optimized_runs)
    payload = {
        "baseline_run_count": len(baseline_runs),
        "optimized_run_count": len(optimized_runs),
        "baseline_labels": [run["label"] for run in baseline_runs],
        "optimized_labels": [run["label"] for run in optimized_runs],
        "metrics": comparison,
        "observations": build_observations(comparison),
    }
    print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
