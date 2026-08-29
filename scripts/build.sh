#!/usr/bin/env bash
set -e

BUILD_TYPE="${1:-Release}"
BUILD_DIR="build"

echo "=== Building Project Papaya (${BUILD_TYPE}) ==="
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -GNinja ..
ninja -j$(nproc)

echo "=== Build Complete ==="
echo "Executable located at: ${BUILD_DIR}/src/app/papaya"
