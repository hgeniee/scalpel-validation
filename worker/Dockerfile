FROM python:3.11-slim

ARG SCALPEL_FETCH_MODE=local
ARG SCALPEL_SOURCE_SUBDIR=vendor/scalpel
ARG SCALPEL_GIT_URL=https://github.com/nolaforensix/scalpel-2.02.git

# 빌드 및 실행 의존성 설치
RUN apt-get update && apt-get install -y \
    git \
    time \
    build-essential \
    autoconf \
    automake \
    libtool \
    libtre-dev \
    libssl-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

COPY . /src

RUN chmod +x /src/worker/scripts/build_scalpel.sh && \
    /src/worker/scripts/build_scalpel.sh

WORKDIR /app
COPY worker/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY worker/ /app/

# 실행 시 라이브러리 경로 설정
ENV LD_LIBRARY_PATH=/usr/local/lib

CMD ["python", "worker.py"]
