#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Runs the CMake/CTest suite across the platforms the analyzer is sensitive to.
#
# It reads types and mangled names a standard library produces, and those differ
# between implementations and versions: libstdc++ lowers std::mutex to an unnamed
# struct where libc++ names it, which silently disabled lock recognition until the
# difference was reproduced. A single local checkout cannot see that.
#
# Axes, in the order they have actually caught defects:
#   standard library  libc++ on the macOS host, libstdc++ in these images
#   architecture      arm64 vs amd64, emulated through buildx when not native
#
# The default matrix holds one image because that is what reproduces CI. Adding
# distributions is cheap through CI_MATRIX, but buys little today: the project
# needs libstdc++ 13 or newer for <format>, which rules out Debian 12 and Ubuntu
# 22.04, and the images above that all carry 13 or 14. Vary STDLIB_PACKAGE in
# the Dockerfile to move that axis deliberately.
#
# Architecture has not yet changed a verdict; it is kept because emulation makes
# the check cheap to ask for, not because it is expected to fail.
#
# Usage:
#   scripts/ci-matrix.sh                    # default matrix, host architecture
#   scripts/ci-matrix.sh --with-emulated    # add the non-native architecture
#   CI_MATRIX="ubuntu:24.04" scripts/ci-matrix.sh   # restrict the images
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGES="${CI_MATRIX:-ubuntu:24.04}"
HOST_ARCH="$(uname -m)"
case "${HOST_ARCH}" in
    arm64 | aarch64) NATIVE_PLATFORM="linux/arm64"; OTHER_PLATFORM="linux/amd64" ;;
    *) NATIVE_PLATFORM="linux/amd64"; OTHER_PLATFORM="linux/arm64" ;;
esac

platforms=("${NATIVE_PLATFORM}")
if [ "${1:-}" = "--with-emulated" ]; then
    platforms+=("${OTHER_PLATFORM}")
fi

failures=0
results=()

for image in ${IMAGES}; do
    for platform in "${platforms[@]}"; do
        tag="coretrace-ci:$(echo "${image}-${platform}" | tr ':/.' '---')"
        label="${image} ${platform}"

        echo "==> Building ${label}"
        if ! docker buildx build --platform "${platform}" --build-arg "BASE_IMAGE=${image}" \
                --load -t "${tag}" -f "${REPO_ROOT}/docker/ci-linux.Dockerfile" \
                "${REPO_ROOT}/docker" >/dev/null 2>&1; then
            results+=("BUILD-FAILED  ${label}")
            failures=$((failures + 1))
            continue
        fi

        echo "==> Testing ${label}"
        if CI_IMAGE="${tag}" CI_PLATFORM="${platform}" "${REPO_ROOT}/scripts/ci-linux.sh"; then
            results+=("PASS          ${label}")
        else
            results+=("FAIL          ${label}")
            failures=$((failures + 1))
        fi
    done
done

echo
echo "===== matrix summary ====="
printf '%s\n' "${results[@]}"
exit $(( failures > 0 ? 1 : 0 ))
