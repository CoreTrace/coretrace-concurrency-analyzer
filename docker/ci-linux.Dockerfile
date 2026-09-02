# SPDX-License-Identifier: Apache-2.0
#
# Reproduces the Linux job of .github/workflows/cmake-ctest.yml locally, and
# serves the wider matrix scripts/ci-matrix.sh runs.
#
# That workflow is the source of truth for the distribution and LLVM version
# the CI job uses; when it changes, change BASE_IMAGE's default here too, or
# this image quietly stops reproducing CI.
#
# The base is a build argument so the same recipe covers several distributions.
# What the analyzer is sensitive to is the standard library implementation and
# version, and distributions are how that varies: Ubuntu 24.04 carries
# libstdc++ 14, Debian 12 carries libstdc++ 12.
#
# CI runners ship a C++ compiler; a bare image does not, hence g++.
#
# STDLIB_PACKAGE pins which libstdc++ headers clang finds. GitHub's runner has
# libstdc++ 14 available and clang-20 selects it, so leaving the distribution
# default (13 on Ubuntu 24.04) would quietly stop reproducing CI. Override it to
# vary the standard library version deliberately.
#
# The project needs libstdc++ 13 or newer: a dependency includes <format>.
ARG BASE_IMAGE=ubuntu:24.04
ARG STDLIB_PACKAGE=libstdc++-14-dev
FROM ${BASE_IMAGE}
ARG STDLIB_PACKAGE

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git gnupg lsb-release ninja-build python3 \
        software-properties-common wget \
 && wget -q https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh 20 \
 && apt-get update && apt-get install -y --no-install-recommends \
        clang-20 libclang-20-dev llvm-20-dev ${STDLIB_PACKAGE} \
 && rm -rf /var/lib/apt/lists/* llvm.sh

ENV LLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm \
    Clang_DIR=/usr/lib/llvm-20/lib/cmake/clang \
    CLANG_EXECUTABLE=/usr/bin/clang-20

WORKDIR /src
