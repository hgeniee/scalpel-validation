#!/usr/bin/env bash

set -euo pipefail

SOURCE_SUBDIR=""
IMAGE_PATH=""
CONFIG_PATH=""
OUTPUT_ROOT="./bench/scalpel_bench/results/buffer-sweep"
GENERATED_SOURCES_ROOT="./bench/scalpel_bench/generated_sources/buffer-sweep"
TAG_PREFIX="scalpel-buffer-sweep"
REPEAT_COUNT=1
BUILD_ONLY=0
BUFFER_SIZES_MB=(64 32 16 10)

usage() {
  cat <<'EOF'
Usage: ./bench/scalpel_bench/run_buffer_sweep.sh \
  --source-subdir vendor/scalpel-optimized \
  --image-path /absolute/path/to/disk.dd \
  [--config-path ./worker/scalpel2.conf] \
  [--output-root ./bench/scalpel_bench/results/buffer-sweep] \
  [--generated-sources-root ./bench/scalpel_bench/generated_sources/buffer-sweep] \
  [--tag-prefix scalpel-buffer-sweep] \
  [--buffer-sizes-mb 64,32,16,10] \
  [--repeat-count 3] \
  [--build-only]

This script clones the optimized source tree under the output root, rewrites
src/common.h so each clone uses a different SIZE_OF_BUFFER, then builds and
runs one worker image per buffer size.

Note: Cache dropping is disabled in this script.
Please run 'sudo sync && echo 3 | sudo tee /proc/sys/vm/drop_caches' manually if needed.
EOF
}

resolve_existing_path() {
  local target="$1"
  if [[ ! -e "$target" ]]; then
    echo "Path not found: $target" >&2
    exit 1
  fi
  ( cd "$(dirname "$target")" && printf '%s/%s\n' "$(pwd -P)" "$(basename "$target")" )
}

resolve_config_path() {
  if [[ -n "$CONFIG_PATH" ]]; then
    resolve_existing_path "$CONFIG_PATH"
    return
  fi

  local candidate
  for candidate in "./worker/scalpel2.conf" "./worker/scalpel.conf"; do
    if [[ -f "$candidate" ]]; then
      resolve_existing_path "$candidate"
      return
    fi
  done

  echo "Config file not found." >&2
  exit 1
}

resolve_output_root() {
  mkdir -p "$OUTPUT_ROOT"
  ( cd "$OUTPUT_ROOT" && pwd -P )
}

detect_python() {
  local cmd
  for cmd in python3 python; do
    if command -v "$cmd" >/dev/null 2>&1; then
      echo "$cmd"
      return
    fi
  done

  echo "Python required." >&2
  exit 1
}

normalize_text_log() {
  local path="$1"
  [[ -f "$path" ]] && perl -0pi -e 's/\r\n/\n/g; s/\r/\n/g' "$path"
}

drop_linux_page_cache() {
  echo "Notice: Skipping automatic cache drop (Manual control mode)."
}

parse_buffer_sizes() {
  local raw="$1"
  local previous_ifs="$IFS"
  local token
  IFS=',' read -r -a BUFFER_SIZES_MB <<<"$raw"
  IFS="$previous_ifs"

  if [[ "${#BUFFER_SIZES_MB[@]}" -eq 0 ]]; then
    echo "Specify at least one value in --buffer-sizes-mb" >&2
    exit 1
  fi

  for token in "${BUFFER_SIZES_MB[@]}"; do
    if [[ ! "$token" =~ ^[0-9]+$ ]] || [[ "$token" -le 0 ]]; then
      echo "Invalid buffer size: $token" >&2
      exit 1
    fi
  done
}

prepare_variant_source() {
  local source_path="$1"
  local generated_sources_root="$2"
  local buffer_mb="$3"
  local variant_name
  local variant_path
  local common_header

  variant_name="$(printf 'optimized-buffer-%03dmb' "$buffer_mb")"
  variant_path="$generated_sources_root/$variant_name"

  rm -rf "$variant_path"
  mkdir -p "$variant_path"
  cp -a "$source_path/." "$variant_path/"

  common_header="$variant_path/src/common.h"
  if [[ ! -f "$common_header" ]]; then
    echo "common.h not found under $variant_path" >&2
    exit 1
  fi

  "$PYTHON_BIN" - "$common_header" "$buffer_mb" <<'PY'
from pathlib import Path
import re
import sys

header = Path(sys.argv[1])
buffer_mb = sys.argv[2]
raw = header.read_text(encoding="utf-8")
updated = re.sub(
    r"#define SIZE_OF_BUFFER\s+\([0-9]+\s+\*\s+MEGABYTE\)",
    f"#define SIZE_OF_BUFFER            ({buffer_mb} * MEGABYTE)",
    raw,
    count=1,
)
if updated == raw:
    raise SystemExit(f"Failed to replace SIZE_OF_BUFFER in {header}")
header.write_text(updated, encoding="utf-8")
PY

  printf '%s\n' "$variant_path"
}

build_worker_image() {
  local tag="$1"
  local source_subdir="$2"
  echo "Building $tag from $source_subdir"
  docker build -f worker/Dockerfile --build-arg SCALPEL_FETCH_MODE=local \
    --build-arg SCALPEL_SOURCE_SUBDIR="$source_subdir" -t "$tag" .
}

invoke_scalpel_run() {
  local tag="$1"
  local label="$2"
  local image_path="$3"
  local config_path="$4"
  local output_root="$5"
  local python_bin="$6"
  local run_root="$output_root/$label"
  local out_dir="$run_root/carved"
  local time_file="$run_root/time.txt"
  local stdout_file="$run_root/stdout.txt"
  local stderr_file="$run_root/stderr.txt"
  local summary_file="$run_root/summary.json"
  local image_dir
  local image_name
  local config_dir
  local config_name
  local command

  drop_linux_page_cache "$label"

  rm -rf "$run_root"
  mkdir -p "$out_dir"

  image_dir="$(dirname "$image_path")"
  image_name="$(basename "$image_path")"
  config_dir="$(dirname "$config_path")"
  config_name="$(basename "$config_path")"
  command="/usr/bin/time -v scalpel2 -c /config/$config_name -o /output /input/$image_name"

  if ! docker run --rm -v "$image_dir:/input:ro" -v "$config_dir:/config:ro" -v "$out_dir:/output" \
    --entrypoint sh "$tag" -lc "$command" >"$stdout_file" 2>"$stderr_file"; then
    echo "docker run failed for $label" >&2
    exit 1
  fi

  normalize_text_log "$stdout_file"
  normalize_text_log "$stderr_file"
  grep ':' "$stderr_file" >"$time_file" || true
  "$python_bin" bench/scalpel_bench/summarize_scalpel_run.py "$time_file" "$stdout_file" "$out_dir" >"$summary_file"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-subdir) SOURCE_SUBDIR="$2"; shift 2 ;;
    --image-path) IMAGE_PATH="$2"; shift 2 ;;
    --config-path) CONFIG_PATH="$2"; shift 2 ;;
    --output-root) OUTPUT_ROOT="$2"; shift 2 ;;
    --generated-sources-root) GENERATED_SOURCES_ROOT="$2"; shift 2 ;;
    --tag-prefix) TAG_PREFIX="$2"; shift 2 ;;
    --buffer-sizes-mb) parse_buffer_sizes "$2"; shift 2 ;;
    --repeat-count) REPEAT_COUNT="$2"; shift 2 ;;
    --build-only) BUILD_ONLY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ -z "$SOURCE_SUBDIR" || -z "$IMAGE_PATH" ]]; then
  usage >&2
  exit 1
fi

IMAGE_PATH="$(resolve_existing_path "$IMAGE_PATH")"
CONFIG_PATH="$(resolve_config_path)"
OUTPUT_ROOT="$(resolve_output_root)"
PYTHON_BIN="$(detect_python)"
SOURCE_PATH="$(resolve_existing_path "$SOURCE_SUBDIR")"
GENERATED_SOURCES_ROOT="$(mkdir -p "$GENERATED_SOURCES_ROOT" && cd "$GENERATED_SOURCES_ROOT" && pwd -P)"
mkdir -p "$GENERATED_SOURCES_ROOT"

declare -a VARIANT_LABELS=()
declare -a VARIANT_TAGS=()

for buffer_mb in "${BUFFER_SIZES_MB[@]}"; do
  variant_path="$(prepare_variant_source "$SOURCE_PATH" "$GENERATED_SOURCES_ROOT" "$buffer_mb")"
  relative_source_subdir="$("$PYTHON_BIN" - "$variant_path" "$(pwd -P)" <<'PY'
from pathlib import Path
import sys

variant = Path(sys.argv[1])
workspace = Path(sys.argv[2])
try:
    print(variant.relative_to(workspace).as_posix())
except ValueError as exc:
    raise SystemExit(
        f"Generated source path must stay inside the workspace so docker build can see it: {variant}"
    ) from exc
PY
)"
  variant_label="$(printf 'buffer-%03dmb' "$buffer_mb")"
  variant_tag="${TAG_PREFIX}:${buffer_mb}mb"

  build_worker_image "$variant_tag" "$relative_source_subdir"
  VARIANT_LABELS+=("$variant_label")
  VARIANT_TAGS+=("$variant_tag")
done

if [[ "$BUILD_ONLY" -eq 1 ]]; then
  echo "Build completed. Skipping benchmark runs because --build-only was set."
  exit 0
fi

for i in "${!VARIANT_LABELS[@]}"; do
  variant_label="${VARIANT_LABELS[$i]}"
  variant_tag="${VARIANT_TAGS[$i]}"

  if [[ "$REPEAT_COUNT" -le 1 ]]; then
    invoke_scalpel_run "$variant_tag" "$variant_label" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
    continue
  fi

  for ((run_index=1; run_index<=REPEAT_COUNT; run_index++)); do
    run_label="$(printf '%s-run-%02d' "$variant_label" "$run_index")"
    invoke_scalpel_run "$variant_tag" "$run_label" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
  done
done

reference_buffer_mb="$(printf '%s\n' "${BUFFER_SIZES_MB[@]}" | sort -nr | head -n1)"
"$PYTHON_BIN" bench/scalpel_bench/buffer_sweep_summary.py "$OUTPUT_ROOT" "$reference_buffer_mb" >"$OUTPUT_ROOT/buffer_sweep_summary.json"
echo "Buffer sweep completed. Summary written to $OUTPUT_ROOT/buffer_sweep_summary.json"
