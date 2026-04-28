# Scalpel C Benchmark Flow

This benchmark flow compares two real Scalpel C source trees by building two worker
images and running the installed `scalpel2` binary against the same image and config.

## Inputs

- baseline source subdirectory
- optimized source subdirectory
- disk image path
- scalpel config path (optional, omitted 시 `worker/scalpel2.conf` -> `worker/scalpel.conf` 순서로 자동 선택)
- Linux page cache drop before each run (default on `run_compare.sh`, disable with `--skip-drop-caches`)

## Outputs

Per run:

- raw GNU `time -v` output
- command stdout/stderr
- parsed JSON summary with:
  - elapsed/user/system/cpu time
  - peak and average RSS
  - page faults and context switches
  - file system I/O counters reported by GNU `time`
  - carved file count / carved output size
  - lazy work-queue utilization metrics parsed from optimized stdout

Per result root:

- `comparison.json` aggregate comparing baseline vs optimized means/min/max/stdev

## Metric Notes

- `elapsed_sec`: 전체 wall-clock 실행 시간. 사용자가 체감하는 "총 얼마나 빨랐는가"를 나타냄.
- `user_time_sec`: 사용자 영역 CPU 계산 시간. 알고리즘 bookkeeping이나 추가 연산이 늘면 증가할 수 있음.
- `system_time_sec`: 커널 영역 CPU 시간. 파일 처리, 메모리 관리, 스케줄링 등 시스템 호출 부담의 간접 신호.
- `cpu_time_sec`: `user_time_sec + system_time_sec`. 총 CPU 소모량.
- `cpu_percent`: 경과 시간 대비 CPU를 얼마나 적극적으로 사용했는지. 멀티스레드일 때 100%를 넘을 수 있음.
- `max_rss_mb`: 실행 중 최대 resident memory. 메모리 피크 비교의 핵심 지표.
- `avg_rss_mb`: 평균 resident memory. GNU time 환경에 따라 0으로 나올 수 있어 보조지표로만 사용.
- `major_page_faults`: 디스크 I/O가 필요한 page fault. 높으면 실제 메모리/스토리지 부담 가능성을 의심.
- `minor_page_faults`: 메모리 내 페이지 재매핑 수준의 fault. 보통 major보다 덜 치명적이지만 메모리 활동 신호로 봄.
- `voluntary_context_switches`: 스레드가 자발적으로 CPU를 양보한 횟수. 대기/동기화 패턴의 힌트.
- `involuntary_context_switches`: 스케줄러에 의해 강제로 전환된 횟수. 경쟁이나 대기 병목 감소 여부를 볼 때 유용.
- `file_system_inputs`, `file_system_outputs`: GNU time이 본 파일 시스템 I/O 카운터. 컨테이너/캐시 환경에 따라 0으로 보일 수 있음.
- `files_carved`: scalpel stdout이 보고한 carve 완료 파일 수.
- `carved_file_count`: 출력 디렉터리에서 실제 확인한 파일 수. `files_carved`와 교차검증용.
- `carved_total_mb`: 실제 carved output 총 크기. 같은 양을 더 빨리 썼는지 비교할 때 사용.
- `initialized_queue_count`: lazy queue 전략에서 실제 초기화된 work queue 수.
- `queue_table_count`: 파일 크기 기준으로 만들 수 있는 전체 queue 슬롯 수.
- `queue_initialization_ratio`: `initialized_queue_count / queue_table_count`. 필요한 블록만 활성화됐는지 보여주는 지표.
- `continue_carve_entries`: 중간 블록에 배치된 `CONTINUECARVE` 작업 수. carve 범위가 넓을수록 커지는 경향이 있음.
- `delta`: `optimized - baseline`. 음수면 optimized가 더 낮음.
- `delta_pct`: baseline 대비 변화율(%). 표와 본문에서 가장 자주 인용할 값.
- `stdev`: 반복 실험 간 변동성. 작을수록 결과가 안정적.

## Example

Windows / PowerShell:

```powershell
pwsh bench\scalpel_bench\run_compare.ps1 `
  -BaselineSourceSubdir vendor/scalpel-baseline `
  -OptimizedSourceSubdir vendor/scalpel-optimized `
  -ImagePath C:\cases\disk.dd `
  -OutputRoot C:\repo\bench\scalpel_bench\results
```

Ubuntu / Bash:

```bash
./bench/scalpel_bench/run_compare.sh \
  --baseline-source-subdir vendor/scalpel-baseline \
  --optimized-source-subdir vendor/scalpel-optimized \
  --image-path /data/cases/disk.dd \
  --output-root ./bench/scalpel_bench/results
```

If you only want to validate builds first, add `-BuildOnly`.

On Ubuntu/Linux, `run_compare.sh` drops the page cache before each benchmark run by
default so repeated measurements are less affected by warmed filesystem cache. This
requires root privileges to write `/proc/sys/vm/drop_caches`, so run the script as
root or make sure `sudo -n` works for the current user. If you want to keep the
current cache state, add `--skip-drop-caches`.

## Repeated runs

To reduce noise for paper figures, run repeated measurements:

Windows / PowerShell:

```powershell
pwsh bench\scalpel_bench\run_compare.ps1 `
  -BaselineSourceSubdir vendor/scalpel-baseline `
  -OptimizedSourceSubdir vendor/scalpel-optimized `
  -ImagePath C:\cases\disk.dd `
  -OutputRoot C:\repo\bench\scalpel_bench\results `
  -RepeatCount 5
```

Ubuntu / Bash:

```bash
./bench/scalpel_bench/run_compare.sh \
  --baseline-source-subdir vendor/scalpel-baseline \
  --optimized-source-subdir vendor/scalpel-optimized \
  --image-path /data/cases/disk.dd \
  --output-root ./bench/scalpel_bench/results \
  --repeat-count 5
```

This writes per-run folders such as `baseline-run-01` and `optimized-run-01`,
then generates `comparison.json` with aggregate statistics and short
observations that are easier to reuse in the paper draft.

## Buffer sweep

To isolate whether larger read buffers are driving the speedup, run the same
optimized source with several `SIZE_OF_BUFFER` values:

```powershell
pwsh bench\scalpel_bench\run_buffer_sweep.ps1 `
  -SourceSubdir vendor/scalpel-optimized `
  -ImagePath C:\cases\disk.dd `
  -OutputRoot C:\repo\bench\scalpel_bench\results\buffer-sweep `
  -BufferSizesMB 64,32,16,10 `
  -RepeatCount 3
```

This script:

- keeps generated variant sources under `bench/scalpel_bench/generated_sources/buffer-sweep`
- rewrites `src/common.h` so each clone has a different `SIZE_OF_BUFFER`
- builds one worker image per buffer size
- writes per-run outputs such as `buffer-064mb-run-01`
- generates `buffer_sweep_summary.json` ranking buffer sizes by mean elapsed time

Use the largest buffer as the reference point, then inspect how `elapsed_sec`,
`pass1_scan_sec`, `pass2_read_sec`, and `max_rss_mb` move as the buffer shrinks.

Ubuntu / Bash:

```bash
nohup ./bench/scalpel_bench/run_buffer_sweep.sh \
  --source-subdir vendor/scalpel-optimized \
  --image-path /media/jwlee-server/SSS_24TB_8/512G_disk_delete.img \
  --output-root /media/jwlee-server/SSS_24TB_8/results/test4-buffer-sweep \
  --buffer-sizes-mb 64,32,16,10 \
  --repeat-count 3 &
```

This produces per-buffer run directories plus `buffer_sweep_summary.json` under
the chosen output root, so you can compare each reduced buffer directly against
the 64 MB reference.
