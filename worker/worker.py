import json
import os
import time
from datetime import datetime

import redis
from sqlalchemy import create_engine, text

from engine.ai_scorer import AIScorer
from engine.scalpel_engine import ScalpelEngine
from engine.validator import Validator


REDIS_URL = os.getenv("REDIS_URL", "redis://redis:6379")
DATABASE_URL = os.environ["DATABASE_URL"]
STORAGE_PATH = os.getenv("STORAGE_PATH", "/app/storage")

r = redis.from_url(REDIS_URL)
db_engine = create_engine(DATABASE_URL)
engine = ScalpelEngine()
validator = Validator()
scorer = AIScorer()


def update_job(job_id: str, **fields) -> None:
    if not fields:
        return

    assignments = ", ".join(f"{key} = :{key}" for key in fields)
    params = {"job_id": job_id, **fields}
    with db_engine.begin() as conn:
        conn.execute(
            text(f"UPDATE jobs SET {assignments} WHERE id = :job_id"),
            params,
        )


def default_scalpel_config() -> str | None:
    for candidate in ("/app/scalpel2.conf", "/app/scalpel.conf"):
        if os.path.exists(candidate):
            return candidate
    return None


print("Worker started, waiting for jobs...")

while True:
    try:
        raw_data = r.blpop("job_queue")
        if not raw_data:
            continue

        _, raw = raw_data
        job = json.loads(raw)

        job_id = job.get("job_id")
        file_path = job.get("file_path") or job.get("image_path")
        output_path = job.get("output_path")
        params = dict(job.get("params", {}))

        if not job_id or not file_path:
            print(f"Skipping malformed job payload: {job}")
            continue

        print(f"Processing job: {job_id}")

        output_dir = output_path or os.path.join(STORAGE_PATH, "outputs", str(job_id))
        update_job(
            job_id,
            status="running",
            current_stage="carving",
            started_at=datetime.utcnow(),
            error_msg=None,
        )

        if "config" not in params:
            config_path = default_scalpel_config()
            if config_path:
                params["config"] = config_path

        result = engine.run(file_path, output_dir, params)

        if result["returncode"] != 0:
            update_job(
                job_id,
                status="failed",
                current_stage="carving_failed",
                finished_at=datetime.utcnow(),
                error_msg=result["stderr"][:65535],
            )
            print(f"Job {job_id} failed in Scalpel stage: {result['stderr']}")
            continue

        processed_files = []
        for root, _, files in os.walk(output_dir):
            for file in files:
                if file == "audit.txt":
                    continue

                f_path = os.path.join(root, file)
                validated = validator.validate(f_path)
                score = scorer.score(f_path)

                processed_files.append(
                    {
                        "file_name": file,
                        "file_path": f_path,
                        "is_valid": validated.is_valid,
                        "score": score["score"],
                        "label": score["label"],
                    }
                )

        update_job(
            job_id,
            status="done",
            current_stage="completed",
            finished_at=datetime.utcnow(),
            error_msg=None,
        )
        print(f"Job {job_id} completed. Recovered {len(processed_files)} files.")

    except Exception as e:
        print(f"Worker main loop error: {e}")
        time.sleep(1)
