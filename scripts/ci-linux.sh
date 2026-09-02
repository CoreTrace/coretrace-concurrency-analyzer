#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Runs the CMake/CTest suite the way the Linux CI job does, inside a container.
#
# The analyzer reads types and mangled names a standard library produces, and
# libstdc++ lowers several of them differently from libc++. A macOS checkout
# cannot see those differences, so a change that passes locally can still fail
# CI. This reproduces that environment before pushing.
#
# Usage:
#   scripts/ci-linux.sh                 # host architecture
#   CI_PLATFORM=linux/amd64 scripts/ci-linux.sh   # match the CI runner exactly
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${CI_IMAGE:-coretrace-ci-linux:llvm20}"
DOCKERFILE="${REPO_ROOT}/docker/ci-linux.Dockerfile"

# An empty array expands unsafely under `set -u` in bash 3.2, which macOS ships.
platform_args=()
if [ -n "${CI_PLATFORM:-}" ]; then
    platform_args=(--platform "${CI_PLATFORM}")
fi
expand_platform_args() {
    if [ "${#platform_args[@]}" -gt 0 ]; then
        printf '%s\n' "${platform_args[@]}"
    fi
}

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "Building ${IMAGE} (first run only)..."
    docker build $(expand_platform_args) -t "${IMAGE}" -f "${DOCKERFILE}" "${REPO_ROOT}/docker"
fi

# Each image needs its own build tree: object files from one distribution or
# architecture are not reusable by another.
BUILD_DIR="/tmp/build-$(printf '%s' "${IMAGE}${CI_PLATFORM:-}" | tr -c 'A-Za-z0-9' '-')"

# The compiler dependency is fetched from GitHub by FetchContent. Doing that on
# every run makes the harness fail for reasons that have nothing to do with the
# code: an unauthenticated fetch is rate-limited, and the error it produces is a
# credential prompt, which reads like a permission problem rather than a quota.
# A checkout that already exists on the host is reused instead, so a run says
# something about the code or nothing at all.
# Every `<name>-src` a host build already produced is offered back, including the
# ones a dependency fetches for itself: resolving one of them locally only moves
# the failure to the next.
DEPS_DIR="${CI_DEPS_DIR:-${REPO_ROOT}/build/_deps}"
CC_MOUNT_ARGS=()
CC_SOURCE_OPTION=""
for source in "${DEPS_DIR}"/*-src; do
    [ -d "${source}" ] || continue
    name="$(basename "${source}" -src)"
    CC_MOUNT_ARGS+=(-v "${source}":"/deps/${name}":ro)
    # FetchContent reads the variable uppercased, whatever case the project declared.
    CC_SOURCE_OPTION="${CC_SOURCE_OPTION} -DFETCHCONTENT_SOURCE_DIR_$(printf '%s' "${name}" | tr '[:lower:]' '[:upper:]')=/deps/${name}"
done

if [ ${#CC_MOUNT_ARGS[@]} -eq 0 ]; then
    echo "No prebuilt sources under ${DEPS_DIR}; dependencies will be fetched." >&2
fi

# Runs unprivileged: several tests assert that unreadable paths fail, and root
# reads them regardless. As a non-root user the suite behaves exactly as in CI,
# so any failure here is a real one.
docker run --rm --user 1000:1000 $(expand_platform_args) -v "${REPO_ROOT}":/src \
    "${CC_MOUNT_ARGS[@]}" -e "BUILD_DIR=${BUILD_DIR}" \
    -e "CC_SOURCE_OPTION=${CC_SOURCE_OPTION}" "${IMAGE}" bash -c '
    set -e
    cmake -S /src -B "${BUILD_DIR}" -G Ninja \
        ${CC_SOURCE_OPTION} \
        -DLLVM_DIR="${LLVM_DIR}" \
        -DClang_DIR="${Clang_DIR}" \
        -DCLANG_EXECUTABLE="${CLANG_EXECUTABLE}" \
        -DCLANG_RESOURCE_DIR="$(${CLANG_EXECUTABLE} -print-resource-dir)" >/dev/null
    cmake --build "${BUILD_DIR}" --parallel
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
'
