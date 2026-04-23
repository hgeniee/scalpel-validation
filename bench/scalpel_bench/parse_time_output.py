from __future__ import annotations

import json
import re
import sys
from datetime import timedelta
from pathlib import Path


# Map raw GNU time -v lines into normalized summary.json keys.
# The main analysis axes are:
# - elapsed/user/system/cpu_percent: runtime and CPU usage pattern
# - rss: memory peak
# - page_faults/context_switches: indirect memory/scheduling pressure signals
# - file_system_*: file I/O counters observed by the kernel
PATTERNS = {
    "command": re.compile(r'Command being timed: "(.*)"$'),
    "elapsed_wall_clock": re.compile(r"Elapsed \(wall clock\) time .*: (.+)$"),
    "max_rss_kb": re.compile(r"Maximum resident set size \(kbytes\): (\d+)$"),
    "avg_rss_kb": re.compile(r"Average resident set size \(kbytes\): (\d+)$"),
    "user_time_sec": re.compile(r"User time \(seconds\): ([\d.]+)$"),
    "system_time_sec": re.compile(r"System time \(seconds\): ([\d.]+)$"),
    "cpu_percent": re.compile(r"Percent of CPU this job got: (\d+)%$"),
    "avg_shared_text_kb": re.compile(r"Average shared text size \(kbytes\): (\d+)$"),
    "avg_unshared_data_kb": re.compile(r"Average unshared data size \(kbytes\): (\d+)$"),
    "avg_stack_kb": re.compile(r"Average stack size \(kbytes\): (\d+)$"),
    "avg_total_size_kb": re.compile(r"Average total size \(kbytes\): (\d+)$"),
    "major_page_faults": re.compile(r"Major \(requiring I/O\) page faults: (\d+)$"),
    "minor_page_faults": re.compile(r"Minor \(reclaiming a frame\) page faults: (\d+)$"),
    "voluntary_context_switches": re.compile(r"Voluntary context switches: (\d+)$"),
    "involuntary_context_switches": re.compile(r"Involuntary context switches: (\d+)$"),
    "swaps": re.compile(r"Swaps: (\d+)$"),
    "file_system_inputs": re.compile(r"File system inputs: (\d+)$"),
    "file_system_outputs": re.compile(r"File system outputs: (\d+)$"),
    "socket_messages_sent": re.compile(r"Socket messages sent: (\d+)$"),
    "socket_messages_received": re.compile(r"Socket messages received: (\d+)$"),
    "signals_delivered": re.compile(r"Signals delivered: (\d+)$"),
    "page_size_bytes": re.compile(r"Page size \(bytes\): (\d+)$"),
    "exit_status": re.compile(r"Exit status: (\d+)$"),
}


# Fields parsed as integers from GNU time -v.
INT_FIELDS = {
    "avg_rss_kb",
    "avg_shared_text_kb",
    "avg_stack_kb",
    "avg_total_size_kb",
    "avg_unshared_data_kb",
    "cpu_percent",
    "exit_status",
    "file_system_inputs",
    "file_system_outputs",
    "major_page_faults",
    "max_rss_kb",
    "minor_page_faults",
    "involuntary_context_switches",
    "page_size_bytes",
    "signals_delivered",
    "socket_messages_received",
    "socket_messages_sent",
    "swaps",
    "voluntary_context_switches",
}

# Fields parsed as floats from GNU time -v.
FLOAT_FIELDS = {"system_time_sec", "user_time_sec"}


def wall_clock_to_seconds(raw: str) -> float:
    parts = raw.split(":")
    if len(parts) == 2:
        minutes, seconds = parts
        delta = timedelta(minutes=int(minutes), seconds=float(seconds))
    elif len(parts) == 3:
        hours, minutes, seconds = parts
        delta = timedelta(hours=int(hours), minutes=int(minutes), seconds=float(seconds))
    else:
        raise ValueError(f"Unsupported elapsed wall clock format: {raw}")
    return round(delta.total_seconds(), 3)


def parse_text(text: str) -> dict[str, object]:
    # Keep only the metrics needed for downstream aggregation and paper analysis.
    result: dict[str, object] = {}
    for line in text.splitlines():
        for key, pattern in PATTERNS.items():
            match = pattern.search(line)
            if not match:
                continue
            value = match.group(1)
            if key in INT_FIELDS:
                result[key] = int(value)
            elif key in FLOAT_FIELDS:
                result[key] = float(value)
            else:
                result[key] = value
    if "max_rss_kb" in result:
        # Peak resident memory. This is the first metric to compare when
        # explaining why memory usage stayed nearly the same.
        result["max_rss_mb"] = round(result["max_rss_kb"] / 1024, 2)
    if "avg_rss_kb" in result:
        result["avg_rss_mb"] = round(result["avg_rss_kb"] / 1024, 2)
    if "elapsed_wall_clock" in result:
        # End-to-end wall-clock time observed by the user.
        result["elapsed_sec"] = wall_clock_to_seconds(str(result["elapsed_wall_clock"]))
    if "user_time_sec" in result and "system_time_sec" in result:
        # cpu_time_sec = user + system.
        # If this grows while elapsed shrinks, we can explain the result as
        # more CPU bookkeeping but less waiting / better overlap.
        result["cpu_time_sec"] = round(
            float(result["user_time_sec"]) + float(result["system_time_sec"]), 3
        )
    return result


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: python parse_time_output.py <time_output_file>", file=sys.stderr)
        return 1

    path = Path(sys.argv[1])
    parsed = parse_text(path.read_text(encoding="utf-8", errors="replace"))
    print(json.dumps(parsed, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
