#!/bin/sh
set -eu

FETCH_MODE="${SCALPEL_FETCH_MODE:-local}"
SOURCE_SUBDIR="${SCALPEL_SOURCE_SUBDIR:-vendor/scalpel}"
GIT_URL="${SCALPEL_GIT_URL:-https://github.com/nolaforensix/scalpel-2.02.git}"
BUILD_ROOT="/tmp/scalpel-src"

rm -rf "${BUILD_ROOT}"
mkdir -p "${BUILD_ROOT}"

if [ "${FETCH_MODE}" = "local" ]; then
    LOCAL_SOURCE="/src/${SOURCE_SUBDIR}"
    if [ ! -d "${LOCAL_SOURCE}" ]; then
        echo "Local Scalpel source directory not found: ${LOCAL_SOURCE}" >&2
        exit 1
    fi

    if [ ! -f "${LOCAL_SOURCE}/configure.ac" ] && [ ! -f "${LOCAL_SOURCE}/configure.in" ]; then
        echo "Local Scalpel source directory does not look like an autotools project: ${LOCAL_SOURCE}" >&2
        exit 1
    fi

    cp -R "${LOCAL_SOURCE}/." "${BUILD_ROOT}/"
else
    git clone "${GIT_URL}" "${BUILD_ROOT}"
fi

cd "${BUILD_ROOT}"
autoreconf -i
./configure
make
make install

rm -rf "${BUILD_ROOT}"
