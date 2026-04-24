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
CLEAR_CACHES=1

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
  [--skip-drop-caches] \
  [--build-only]

By default, the script runs `sync` and drops the Linux page cache
(`echo 3 > /proc/sys/vm/drop_caches`) before each baseline/optimized run.
Use `--skip-drop-caches` only when you intentionally want warm-cache runs.
EOF
}

resolve_existing_path() {
  local target="$1"

  if [[ ! -e "$target" ]]; then
    echo "Path not found: $target" >&2
    exit 1
  fi

  (
    cd "$(dirname "$target")"
    printf '%s/%s\n' "$(pwd -P)" "$(basename "$target")"
  )
}

resolve_config_path() {
  if [[ -n "$CONFIG_PATH" ]]; then
    resolve_existing_path "$CONFIG_PATH"
    return
  fi

  if [[ -f "./worker/scalpel2.conf" ]]; then
    resolve_existing_path "./worker/scalpel2.conf"
    return
  fi

  if [[ -f "./worker/scalpel.conf" ]]; then
    resolve_existing_path "./worker/scalpel.conf"
    return
  fi

  echo "Config file not found. Checked: ./worker/scalpel2.conf, ./worker/scalpel.conf" >&2
  exit 1
}

resolve_output_root() {
  mkdir -p "$OUTPUT_ROOT"
  (
    cd "$OUTPUT_ROOT"
    pwd -P
  )
}

build_worker_image() {
  local tag="$1"
  local source_subdir="$2"

  echo "Building $tag from $source_subdir"
  docker build \
    -f worker/Dockerfile \
    --build-arg SCALPEL_FETCH_MODE=local \
    --build-arg SCALPEL_SOURCE_SUBDIR="$source_subdir" \
    -t "$tag" \
    .
}

detect_python() {
  if command -v python3 >/dev/null 2>&1; then
    echo "python3"
    return
  fi

  if command -v python >/dev/null 2>&1; then
    echo "python"
    return
  fi

  echo "python3 or python is required to run the summary scripts." >&2
  exit 1
}

normalize_text_log() {
  local path="$1"

  if [[ ! -f "$path" ]]; then
    return
  fi

  perl -0pi -e 's/\r\n/\n/g; s/\r/\n/g' "$path"
}

drop_linux_page_cache() {
  local label="$1"

  if [[ "$CLEAR_CACHES" -ne 1 ]]; then
    return
  fi

  if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Skipping cache drop before $label: only supported on Linux." >&2
    return
  fi

  echo "Dropping Linux page cache before $label"

  if [[ -w /proc/sys/vm/drop_caches ]]; then
    sync
    echo 3 > /proc/sys/vm/drop_caches
    return
  fi

  if command -v sudo >/dev/null 2>&1; then
    if sudo -n sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'; then
      return
    fi
    echo "Failed to drop caches before $label: need root or passwordless sudo. Use --skip-drop-caches to continue without cache clearing." >&2
    exit 1
  fi

  echo "Failed to drop caches before $label: need root access to write /proc/sys/vm/drop_caches. Use --skip-drop-caches to continue without cache clearing." >&2
  exit 1
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

  if ! docker run --rm \
    -v "$image_dir:/input:ro" \
    -v "$config_dir:/config:ro" \
    -v "$out_dir:/output" \
    --entrypoint sh \
    "$tag" \
    -lc "$command" \
    >"$stdout_file" 2>"$stderr_file"; then
    tail -n 20 "$stderr_file" >&2 || true
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
    --baseline-source-subdir)
      BASELINE_SOURCE_SUBDIR="$2"
      shift 2
      ;;
    --optimized-source-subdir)
      OPTIMIZED_SOURCE_SUBDIR="$2"
      shift 2
      ;;
    --image-path)
      IMAGE_PATH="$2"
      shift 2
      ;;
    --config-path)
      CONFIG_PATH="$2"
      shift 2
      ;;
    --output-root)
      OUTPUT_ROOT="$2"
      shift 2
      ;;
    --baseline-tag)
      BASELINE_TAG="$2"
      shift 2
      ;;
    --optimized-tag)
      OPTIMIZED_TAG="$2"
      shift 2
      ;;
    --repeat-count)
      REPEAT_COUNT="$2"
      shift 2
      ;;
    --skip-drop-caches)
      CLEAR_CACHES=0
      shift
      ;;
    --build-only)
      BUILD_ONLY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$BASELINE_SOURCE_SUBDIR" || -z "$OPTIMIZED_SOURCE_SUBDIR" || -z "$IMAGE_PATH" ]]; then
  usage >&2
  exit 1
fi

IMAGE_PATH="$(resolve_existing_path "$IMAGE_PATH")"
CONFIG_PATH="$(resolve_config_path)"
OUTPUT_ROOT="$(resolve_output_root)"
PYTHON_BIN="$(detect_python)"

if [[ "$CLEAR_CACHES" -eq 1 ]]; then
  echo "Cache policy: cold-cache runs enabled (drop caches before each run on Linux)."
else
  echo "Cache policy: warm-cache runs enabled (--skip-drop-caches)."
fi

build_worker_image "$BASELINE_TAG" "$BASELINE_SOURCE_SUBDIR"
build_worker_image "$OPTIMIZED_TAG" "$OPTIMIZED_SOURCE_SUBDIR"

if [[ "$BUILD_ONLY" -eq 1 ]]; then
  echo "Build completed. Skipping benchmark runs because --build-only was set."
  exit 0
fi

if [[ "$REPEAT_COUNT" -le 1 ]]; then
  invoke_scalpel_run "$BASELINE_TAG" "baseline" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
  invoke_scalpel_run "$OPTIMIZED_TAG" "optimized" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
else
  for ((run_index = 1; run_index <= REPEAT_COUNT; run_index++)); do
    baseline_label=$(printf 'baseline-run-%02d' "$run_index")
    optimized_label=$(printf 'optimized-run-%02d' "$run_index")
    invoke_scalpel_run "$BASELINE_TAG" "$baseline_label" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
    invoke_scalpel_run "$OPTIMIZED_TAG" "$optimized_label" "$IMAGE_PATH" "$CONFIG_PATH" "$OUTPUT_ROOT" "$PYTHON_BIN"
  done
fi

"$PYTHON_BIN" bench/scalpel_bench/compare_summaries.py "$OUTPUT_ROOT" >"$OUTPUT_ROOT/comparison.json"
