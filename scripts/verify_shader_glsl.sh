#!/bin/bash
# Verify the emitted GLSL actually COMPILES: run the shader tests with
# GLSLANG_VALIDATOR set so test_shader_translator compiles its emitted shader
# with glslang and asserts SPIR-V output.
#
# Usage: scripts/verify_shader_glsl.sh [path/to/glslangValidator]
# Defaults to GLSLANG_VALIDATOR env, then /tmp/glslang-src/build/glslangValidator.
set -euo pipefail
cd "$(dirname "$0")/.."

VAL="${GLSLANG_VALIDATOR:-${1:-/tmp/glslang-src/build/glslangValidator}}"
if [ ! -x "$VAL" ]; then
    echo "glslangValidator not found at: $VAL"
    echo "Build it:  git clone --depth 1 https://github.com/KhronosGroup/glslang /tmp/glslang-src"
    echo "           cmake -B /tmp/glslang-src/build -DENABLE_OPT=OFF /tmp/glslang-src"
    echo "           make -C /tmp/glslang-src/build -j4 glslang-standalone"
    exit 1
fi
echo "== validating emitted GLSL with: $VAL"
export GLSLANG_VALIDATOR="$VAL"
(cd build && ./tests/test_shader_translator)
echo "ok: emitted shaders compile to SPIR-V via glslang"