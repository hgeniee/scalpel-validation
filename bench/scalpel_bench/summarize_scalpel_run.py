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

    summary = parse_text(time_path.read_text(encoding="utf-8", errors="replace"))
    summary.update(parse_stdout(stdout_path.read_text(encoding="utf-8", errors="replace")))
    summary.update(summarize_output_dir(output_dir))

    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
