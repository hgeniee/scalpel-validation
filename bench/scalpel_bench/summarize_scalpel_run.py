from __future__ import annotations

import json
import re
import sys
from pathlib import Path

from parse_time_output import parse_text


# Extract algorithm-specific metrics from scalpel stdout that GNU time cannot see.
SCALPEL_PATTERNS = {
    "files_carved": re.compile(r"files carved\s*=\s*(\d+)"),
    "scalpel_reported_elapsed_sec": re.compile(r"elapsed\s*=\s*(\d+)\s+secs"),
    "initialized_queues": re.compile(
        r"Work queues built using (\d+)/(\d+) initialized queues, CONTINUECARVE entries = (\d+)\."
    ),
    "event_schedule": re.compile(
        r"Event schedule built using (\d+)/(\d+) initialized blocks, CONTINUE spans avoided = (\d+)\."
    ),
    "performance_metrics": re.compile(
        r"Performance metrics:\s+pass1_scan_sec=([0-9.]+)\s+queue_build_sec=([0-9.]+)\s+pass2_read_sec=([0-9.]+)\s+pass2_write_sec=([0-9.]+)\s+pass2_open_close_sec=([0-9.]+)\s+pass2_total_sec=([0-9.]+)"
    ),
    "performance_counters": re.compile(
        r"Performance counters:\s+handle_open_count=(\d+)\s+handle_reopen_count=(\d+)\s+handle_close_count=(\d+)\s+handle_eviction_count=(\d+)\s+peak_open_handles=(\d+)\s+peak_active_carves=(\d+)\s+pass2_bytes_written=(\d+)"
    ),
}


def parse_stdout(text: str) -> dict[str, object]:
    result: dict[str, object] = {}
    files_carved = SCALPEL_PATTERNS["files_carved"].search(text)
    if files_carved:
        result["files_carved"] = int(files_carved.group(1))

    elapsed = SCALPEL_PATTERNS["scalpel_reported_elapsed_sec"].search(text)
    if elapsed:
        result["scalpel_reported_elapsed_sec"] = int(elapsed.group(1))

    queues = SCALPEL_PATTERNS["initialized_queues"].search(text)
    if queues:
        # initialized_queue_count / queue_table_count:
        # how many queue slots were actually activated out of the full table.
        # This shows how much lazy allocation avoided unnecessary queue setup.
        result["initialized_queue_count"] = int(queues.group(1))
        result["queue_table_count"] = int(queues.group(2))
        # continue_carve_entries:
        # number of CONTINUECARVE jobs placed on intermediate blocks.
        # This tends to grow as carve spans get wider.
        result["continue_carve_entries"] = int(queues.group(3))
        result["queue_initialization_ratio"] = round(
            int(queues.group(1)) / int(queues.group(2)), 4
        )

    events = SCALPEL_PATTERNS["event_schedule"].search(text)
    if events:
        result["initialized_block_count"] = int(events.group(1))
        result["event_table_count"] = int(events.group(2))
        result["continue_span_block_count"] = int(events.group(3))
        result["block_activation_ratio"] = round(
            int(events.group(1)) / int(events.group(2)), 4
        )

    perf = SCALPEL_PATTERNS["performance_metrics"].search(text)
    if perf:
        result["pass1_scan_sec"] = float(perf.group(1))
        result["queue_build_sec"] = float(perf.group(2))
        result["pass2_read_sec"] = float(perf.group(3))
        result["pass2_write_sec"] = float(perf.group(4))
        result["pass2_open_close_sec"] = float(perf.group(5))
        result["pass2_total_sec"] = float(perf.group(6))

    counters = SCALPEL_PATTERNS["performance_counters"].search(text)
    if counters:
        result["handle_open_count"] = int(counters.group(1))
        result["handle_reopen_count"] = int(counters.group(2))
        result["handle_close_count"] = int(counters.group(3))
        result["handle_eviction_count"] = int(counters.group(4))
        result["peak_open_handles"] = int(counters.group(5))
        result["peak_active_carves"] = int(counters.group(6))
        result["pass2_bytes_written"] = int(counters.group(7))

    return result


def summarize_output_dir(path: Path) -> dict[str, object]:
    files = [candidate for candidate in path.rglob("*") if candidate.is_file()]
    total_bytes = sum(candidate.stat().st_size for candidate in files)
    return {
        # Actual file count found in the output directory.
        # This is useful as a cross-check against stdout's files_carved.
        "carved_file_count": len(files),
        "carved_total_bytes": total_bytes,
        # Total output volume written by carving.
        # Combined with elapsed, this helps explain effective throughput.
        "carved_total_mb": round(total_bytes / (1024 * 1024), 3),
    }


def read_log_text(path: Path) -> str:
    raw = path.read_bytes()
    for encoding in ("utf-8-sig", "utf-16", "utf-16-le", "utf-16-be", "utf-8"):
        try:
            text = raw.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    else:
        text = raw.decode("utf-8", errors="replace")

    return text.replace("\x00", "")


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "Usage: python summarize_scalpel_run.py <time_output_file> <stdout_file> <output_dir>",
            file=sys.stderr,
        )
        return 1

    time_path = Path(sys.argv[1])
    stdout_path = Path(sys.argv[2])
    output_dir = Path(sys.argv[3])

    summary = parse_text(read_log_text(time_path))
    summary.update(parse_stdout(read_log_text(stdout_path)))
    summary.update(summarize_output_dir(output_dir))

    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
