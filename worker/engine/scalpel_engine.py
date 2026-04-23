# worker/engine/scalpel_engine.py
import subprocess
import time
import psutil
import os
import logging
from typing import Dict, Any


logger = logging.getLogger(__name__)
SCALPEL_BINARY = os.getenv("SCALPEL_BINARY", "scalpel2")

if not logger.handlers:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s - %(message)s"
    )


class ScalpelEngine:
    def __init__(self, version: str = "2.02"):
        logger.info("[ScalpelEngine.__init__] init start: version=%s", version)
        self.version = version
        logger.info("[ScalpelEngine.__init__] init done")

    def run(self, image_path: str, output_dir: str, params: dict) -> Dict[str, Any]:
        logger.info(
            "[ScalpelEngine.run] start: image_path=%s, output_dir=%s, params=%s",
            image_path, output_dir, params
        )

        try:
            logger.info("[ScalpelEngine.run] creating output directory")
            os.makedirs(output_dir, exist_ok=True)

            logger.info("[ScalpelEngine.run] building command")
            cmd = self._build_command(image_path, output_dir, params)
            logger.info("[ScalpelEngine.run] command built: %s", cmd)

            start = time.time()

            logger.info("[ScalpelEngine.run] launching scalpel process")
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            logger.info(
                "[ScalpelEngine.run] process started: pid=%s",
                proc.pid
            )

            cpu_samples = []
            peak_memory_mb = 0.0
            io_read_bytes = 0
            io_write_bytes = 0

            try:
                logger.info("[ScalpelEngine.run] attaching psutil to pid=%s", proc.pid)
                ps_proc = psutil.Process(proc.pid)
            except Exception:
                logger.exception("[ScalpelEngine.run] failed to attach psutil process")
                ps_proc = None

            logger.info("[ScalpelEngine.run] entering monitoring loop")
            while proc.poll() is None:
                if ps_proc is not None:
                    try:
                        cpu = ps_proc.cpu_percent(interval=0.5)
                        cpu_samples.append(cpu)

                        mem_info = ps_proc.memory_info()
                        rss_mb = mem_info.rss / (1024 * 1024)
                        peak_memory_mb = max(peak_memory_mb, rss_mb)

                        io_stats = ps_proc.io_counters()
                        io_read_bytes = io_stats.read_bytes
                        io_write_bytes = io_stats.write_bytes

                        logger.debug(
                            "[ScalpelEngine.run] monitor sample: cpu=%.2f, peak_memory_mb=%.2f, read_bytes=%d, write_bytes=%d",
                            cpu, peak_memory_mb, io_read_bytes, io_write_bytes
                        )
                    except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                        logger.exception("[ScalpelEngine.run] psutil monitoring failed during loop")
                        ps_proc = None
                    except Exception:
                        logger.exception("[ScalpelEngine.run] unexpected monitoring error")
                        ps_proc = None
                else:
                    time.sleep(0.5)

            logger.info("[ScalpelEngine.run] process exited, collecting stdout/stderr")
            stdout, stderr = proc.communicate()
            elapsed = time.time() - start

            try:
                stderr_decoded = stderr.decode(errors="replace")
            except Exception:
                logger.exception("[ScalpelEngine.run] stderr decode failed")
                stderr_decoded = "<stderr decode failed>"

            try:
                stdout_decoded = stdout.decode(errors="replace")
            except Exception:
                logger.exception("[ScalpelEngine.run] stdout decode failed")
                stdout_decoded = "<stdout decode failed>"

            result = {
                "returncode": proc.returncode,
                "exec_time_sec": elapsed,
                "cpu_usage_avg": sum(cpu_samples) / len(cpu_samples) if cpu_samples else 0,
                "peak_memory_mb": peak_memory_mb,
                "io_read_bytes": io_read_bytes,
                "io_write_bytes": io_write_bytes,
                "output_dir": output_dir,
                "stdout": stdout_decoded,
                "stderr": stderr_decoded,
            }

            logger.info(
                "[ScalpelEngine.run] done: returncode=%s, exec_time_sec=%.3f, cpu_usage_avg=%.3f, peak_memory_mb=%.3f",
                result["returncode"],
                result["exec_time_sec"],
                result["cpu_usage_avg"],
                result["peak_memory_mb"],
            )
            return result

        except FileNotFoundError:
            logger.exception(
                "[ScalpelEngine.run] scalpel executable not found. Check PATH or binary location."
            )
            return {
                "returncode": -1,
                "exec_time_sec": 0,
                "cpu_usage_avg": 0,
                "peak_memory_mb": 0,
                "io_read_bytes": 0,
                "io_write_bytes": 0,
                "output_dir": output_dir,
                "stdout": "",
                "stderr": "scalpel executable not found"
            }

        except Exception as e:
            logger.exception("[ScalpelEngine.run] unexpected error")
            return {
                "returncode": -1,
                "exec_time_sec": 0,
                "cpu_usage_avg": 0,
                "peak_memory_mb": 0,
                "io_read_bytes": 0,
                "io_write_bytes": 0,
                "output_dir": output_dir,
                "stdout": "",
                "stderr": str(e)
            }

    def _build_command(self, image_path: str, output_dir: str, params: dict):
        logger.info(
            "[ScalpelEngine._build_command] start: image_path=%s, output_dir=%s, params=%s",
            image_path, output_dir, params
        )

        try:
            # 현재 vendored 소스는 scalpel 바이너리명이 아니라 scalpel2로 설치된다.
            cmd = [SCALPEL_BINARY]

            if params.get("config"):
                logger.info(
                    "[ScalpelEngine._build_command] using config: %s",
                    params["config"]
                )
                cmd += ["-c", params["config"]]

            cmd += ["-o", output_dir, image_path]

            logger.info("[ScalpelEngine._build_command] built command: %s", cmd)
            return cmd

        except Exception:
            logger.exception("[ScalpelEngine._build_command] failed to build command")
            raise
