#!/usr/bin/env bash
set -e

BUILD_DIR="build"

if [ ! -d "${BUILD_DIR}" ]; then
    ./scripts/build.sh Debug
fi

cd "${BUILD_DIR}"
ctest --output-on-failure
./src/app/papaya --test-kvm
