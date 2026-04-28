from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path


LABEL_PREFIX = "buffer-"
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


def parse_buffer_mb(label: str) -> int | None:
    if not label.startswith(LABEL_PREFIX):
        return None
    token = label[len(LABEL_PREFIX) :]
    if "-run-" in token:
        token = token.split("-run-", 1)[0]
    if not token.endswith("mb"):
        return None
    number = token[:-2]
    if not number.isdigit():
        return None
    return int(number)


def load_runs(root: Path) -> dict[int, list[dict[str, object]]]:
    grouped: dict[int, list[dict[str, object]]] = {}
    for child in sorted(root.iterdir()):
        if not child.is_dir():
            continue
        buffer_mb = parse_buffer_mb(child.name)
        if buffer_mb is None:
            continue
        summary_path = child / "summary.json"
        if not summary_path.exists():
            continue
        payload = json.loads(summary_path.read_text(encoding="utf-8-sig"))
        payload["label"] = child.name
        grouped.setdefault(buffer_mb, []).append(payload)
    return grouped


def summarize_metric(runs: list[dict[str, object]], metric: str) -> dict[str, object] | None:
    values = [float(run[metric]) for run in runs if isinstance(run.get(metric), (int, float))]
    if not values:
        return None
    result: dict[str, object] = {
        "n": len(values),
        "mean": round(statistics.mean(values), 4),
        "min": round(min(values), 4),
        "max": round(max(values), 4),
    }
    if len(values) >= 2:
        result["stdev"] = round(statistics.stdev(values), 4)
    return result


def build_variant_summary(
    buffer_mb: int,
    runs: list[dict[str, object]],
    reference_metrics: dict[str, dict[str, object]] | None,
) -> dict[str, object]:
    metrics: dict[str, dict[str, object]] = {}
    for metric in METRICS:
        summary = summarize_metric(runs, metric)
        if not summary:
            continue
        if reference_metrics and metric in reference_metrics:
            reference_mean = float(reference_metrics[metric]["mean"])
            delta = round(float(summary["mean"]) - reference_mean, 4)
            summary["delta_vs_reference"] = delta
            summary["delta_pct_vs_reference"] = (
                round((delta / reference_mean) * 100, 2) if reference_mean else None
            )
            summary["direction"] = (
                "lower_is_better" if metric in LOWER_IS_BETTER else "contextual"
            )
        metrics[metric] = summary

    return {
        "buffer_mb": buffer_mb,
        "run_count": len(runs),
        "labels": [run["label"] for run in runs],
        "metrics": metrics,
    }


def build_observations(variants: list[dict[str, object]], reference_buffer_mb: int) -> list[str]:
    observations: list[str] = []
    ranked = []
    for variant in variants:
        elapsed = variant["metrics"].get("elapsed_sec")
        if not elapsed:
            continue
        ranked.append((variant["buffer_mb"], float(elapsed["mean"])))

    ranked.sort(key=lambda item: item[1])
    if ranked:
        best_buffer, best_elapsed = ranked[0]
        observations.append(
            f"Fastest mean elapsed time was {best_elapsed:.4f}s at {best_buffer} MB."
        )

    reference = next((variant for variant in variants if variant["buffer_mb"] == reference_buffer_mb), None)
    if reference:
        reference_elapsed = reference["metrics"].get("elapsed_sec")
        if reference_elapsed:
            reference_mean = float(reference_elapsed["mean"])
            for variant in variants:
                if variant["buffer_mb"] == reference_buffer_mb:
                    continue
                elapsed = variant["metrics"].get("elapsed_sec")
                if not elapsed:
                    continue
                delta_pct = elapsed.get("delta_pct_vs_reference")
                if delta_pct is None:
                    continue
                observations.append(
                    f"{variant['buffer_mb']} MB changed mean elapsed time by {delta_pct:.2f}% versus the {reference_buffer_mb} MB reference ({reference_mean:.4f}s)."
                )

    return observations


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(
            "Usage: python buffer_sweep_summary.py <results_root> [reference_buffer_mb]",
            file=sys.stderr,
        )
        return 1

    root = Path(sys.argv[1])
    reference_buffer_mb = int(sys.argv[2]) if len(sys.argv) == 3 else 0

    grouped = load_runs(root)
    if not grouped:
        print(
            json.dumps(
                {"error": "Could not find any buffer-XXXmb summary.json files under the results root."},
                ensure_ascii=False,
                indent=2,
            )
        )
        return 2

    available_buffers = sorted(grouped)
    if reference_buffer_mb == 0:
        reference_buffer_mb = max(available_buffers)
    if reference_buffer_mb not in grouped:
        print(
            json.dumps(
                {"error": f"Reference buffer {reference_buffer_mb} MB was not found in the results root."},
                ensure_ascii=False,
                indent=2,
            )
        )
        return 3

    reference_metrics: dict[str, dict[str, object]] = {}
    for metric in METRICS:
        summary = summarize_metric(grouped[reference_buffer_mb], metric)
        if summary:
            reference_metrics[metric] = summary

    variants = [
        build_variant_summary(buffer_mb, grouped[buffer_mb], reference_metrics)
        for buffer_mb in available_buffers
    ]

    ranked_by_elapsed = []
    for variant in variants:
        elapsed = variant["metrics"].get("elapsed_sec")
        if not elapsed:
            continue
        ranked_by_elapsed.append(
            {
                "buffer_mb": variant["buffer_mb"],
                "mean_elapsed_sec": float(elapsed["mean"]),
                "delta_pct_vs_reference": elapsed.get("delta_pct_vs_reference"),
            }
        )
    ranked_by_elapsed.sort(key=lambda item: item["mean_elapsed_sec"])

    payload = {
        "reference_buffer_mb": reference_buffer_mb,
        "available_buffer_sizes_mb": available_buffers,
        "variants": variants,
        "ranked_by_elapsed_sec": ranked_by_elapsed,
        "observations": build_observations(variants, reference_buffer_mb),
    }
    print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
