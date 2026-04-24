#!/usr/bin/env bash

set -euo pipefail

BASELINE_SOURCE_SUBDIR=""
OPTIMIZED_SOURCE_SUBDIR=""
IMAGE_PATH=""
CONFIG_PATH=""
OUTPUT_ROOT="./bench/scalpel_bench/results"
BASELINE_TAG="scalpel-worker:baseline"
OPTIMIZED_TAG="scalpel-worker:optimized"
REPEAT_COUNT=1
BUILD_ONLY=0

usage() {
  cat <<'EOF'
Usage: ./bench/scalpel_bench/run_compare.sh \
  --baseline-source-subdir vendor/scalpel-baseline \
  --optimized-source-subdir vendor/scalpel-optimized \
  --image-path /absolute/path/to/disk.dd \
  [--config-path ./worker/scalpel2.conf] \
  [--output-root ./bench/scalpel_bench/results] \
  [--baseline-tag scalpel-worker:baseline] \
  [--optimized-tag scalpel-worker:optimized] \
  [--repeat-count 5] \
  [--build-only]

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
  if [[ -n "$CONFIG_PATH" ]]; then resolve_existing_path "$CONFIG_PATH"; return; fi
  for p in "./worker/scalpel2.conf" "./worker/scalpel.conf"; do
    if [[ -f "$p" ]]; then resolve_existing_path "$p"; return; fi
  done
  echo "Config file not found." >&2; exit 1
}

resolve_output_root() {
  mkdir -p "$OUTPUT_ROOT"
  ( cd "$OUTPUT_ROOT" && pwd -P )
}

build_worker_image() {
  local tag="$1"
  local source_subdir="$2"
  echo "Building $tag from $source_subdir"
  docker build -f worker/Dockerfile --build-arg SCALPEL_FETCH_MODE=local \
    --build-arg SCALPEL_SOURCE_SUBDIR="$source_subdir" -t "$tag" .
}

detect_python() {
  for cmd in python3 python; do
    if command -v "$cmd" >/dev/null 2>&1; then echo "$cmd"; return; fi
  done
  echo "Python required." >&2; exit 1
}

normalize_text_log() {
  local path="$1"
  [[ -f "$path" ]] && perl -0pi -e 's/\r\n/\n/g; s/\r/\n/g' "$path"
}

# [수정] 아무 일도 하지 않도록 비활성화
drop_linux_page_cache() {
  echo "Notice: Skipping automatic cache drop (Manual control mode)."
}

invoke_scalpel_run() {
  local tag="$1" label="$2" image_path="$3" config_path="$4" output_root="$5" python_bin="$6"
  local run_root="$output_root/$label"
  local out_dir="$run_root/carved"
  local time_file="$run_root/time.txt"
  local stdout_file="$run_root/stdout.txt"
  local stderr_file="$run_root/stderr.txt"
  local summary_file="$run_root/summary.json"

  # 캐시 삭제 로직은 실행되지만 위에서 비활성화됨
  drop_linux_page_cache "$label"

  rm -rf "$run_root" && mkdir -p "$out_dir"

  local image_dir="$(dirname "$image_path")"
  local image_name="$(basename "$image_path")"
  local config_dir="$(dirname "$config_path")"
  local config_name="$(basename "$config_path")"
  local command="/usr/bin/time -v scalpel2 -c /config/$config_name -o /output /input/$image_name"

  if ! docker run --rm -v "$image_dir:/input:ro" -v "$config_dir:/config:ro" -v "$out_dir:/output" \
    --entrypoint sh "$tag" -lc "$command" >"$stdout_file" 2>"$stderr_file"; then
    echo "docker run failed for $label" >&2; exit 1
  fi

  normalize_text_log "$stdout_file"
  normalize_text_log "$stderr_file"
  grep ':' "$stderr_file" >"$time_file" || true
  "$python_bin" bench/scalpel_bench/summarize_scalpel_run.py "$time_file" "$stdout_file" "$out_dir" >"$summary_file"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --baseline-source-subdir) BASELINE_SOURCE_SUBDIR="$2"; shift 2 ;;
    --optimized-source-subdir) OPTIMIZED_SOURCE_SUBDIR="$2"; shift 2 ;;
    --image-path) IMAGE_PATH="$2"; shift 2 ;;
    --config-path) CONFIG_PATH="$2"; shift 2 ;;
    --output-root) OUTPUT_ROOT="$2"; shift 2 ;;
    --baseline-tag) BASELINE_TAG="$2"; shift 2 ;;
    --optimized-tag) OPTIMIZED_TAG="$2"; shift 2 ;;
    --repeat-count) REPEAT_COUNT="$2"; shift 2 ;;
    --build-only) BUILD_ONLY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ -z "$BASELINE_SOURCE_SUBDIR" || -z "$OPTIMIZED_SOURCE_SUBDIR" || -z "$IMAGE_PATH" ]]; then
  usage >&2; exit 1
fi

IMAGE_PATH="$(resolve_existing_path "$IMAGE_PATH")"
CONFIG_PATH="$(resolve_config_path)"
OUTPUT_ROOT="$(resolve_output_root)"
PYTHON_BIN="$(detect_python)"

build_worker_image "$BASELINE_TAG" "$BASELINE_SOURCE_SUBDIR"
build_worker_image "$OPTIMIZED_TAG" "$OPTIMIZED_SOURCE_SUBDIR"

[[ "$BUILD_ONLY" -eq 1 ]] && exit 0

if [[ "$REPEAT_COUNT" -le 1 ]]; then
  invoke_scalpel_run "$BASELINE_TAG" "baseline" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
  invoke_scalpel_run "$OPTIMIZED_TAG" "optimized" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
else
  for ((i=1; i<=REPEAT_COUNT; i++)); do
    invoke_scalpel_run "$BASELINE_TAG" "$(printf 'baseline-run-%02d' $i)" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
    invoke_scalpel_run "$OPTIMIZED_TAG" "$(printf 'optimized-run-%02d' $i)" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
  done
fi

"$PYTHON_BIN" bench/scalpel_bench/compare_summaries.py "$OUTPUT_ROOT" >"$OUTPUT_ROOT/comparison.json"
