#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly BUILD_ROOT="${SNOWSEEK_BUILD_ROOT:-${PROJECT_ROOT}/build-matrix}"
readonly BUILD_JOBS="${SNOWSEEK_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')}"

require_command() {
        local command_name="$1"
        if ! command -v "${command_name}" >/dev/null 2>&1; then
                printf 'error: required command not found: %s\n' "${command_name}" >&2
                return 1
        fi
}

build_and_test() {
        local compiler_name="$1"
        local compiler_path="$2"
        local build_type="$3"
        local build_name="$4"
        local build_directory="${BUILD_ROOT}/${build_name}"

        printf '\n[%s %s] configure\n' "${compiler_name}" "${build_type}"
        cmake -S "${PROJECT_ROOT}" -B "${build_directory}" \
                -DCMAKE_BUILD_TYPE="${build_type}" \
                -DCMAKE_CXX_COMPILER="${compiler_path}" \
                -DSNOWSEEK_BUILD_TESTS=ON \
                -DSNOWSEEK_BUILD_BENCHMARKS=OFF \
                -DSNOWSEEK_WARNINGS_AS_ERRORS=ON

        printf '[%s %s] build\n' "${compiler_name}" "${build_type}"
        cmake --build "${build_directory}" --parallel "${BUILD_JOBS}"

        printf '[%s %s] test\n' "${compiler_name}" "${build_type}"
        (
                cd -- "${build_directory}"
                ctest --output-on-failure
        )
}

require_command cmake
require_command ctest
require_command g++
require_command clang++

printf 'SnowSeek build matrix\n'
printf 'source: %s\n' "${PROJECT_ROOT}"
printf 'build root: %s\n' "${BUILD_ROOT}"
printf 'parallel jobs: %s\n' "${BUILD_JOBS}"

build_and_test GCC g++ Debug gcc-debug
build_and_test GCC g++ Release gcc-release
build_and_test Clang clang++ Debug clang-debug
build_and_test Clang clang++ Release clang-release

printf '\nAll GCC/Clang Debug/Release builds and tests passed.\n'
