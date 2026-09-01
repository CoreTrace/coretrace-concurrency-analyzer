# SPDX-License-Identifier: Apache-2.0
#
# Reproduces the Linux job of .github/workflows/cmake-ctest.yml locally.
# That workflow is the source of truth: when it changes distribution or LLVM
# version, change them here too, or this image quietly stops reproducing CI.
#
# Ubuntu runners ship a C++ compiler; a bare image does not, hence g++.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ gnupg lsb-release ninja-build python3 \
        software-properties-common wget \
 && wget -q https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh 20 \
 && apt-get update && apt-get install -y --no-install-recommends \
        clang-20 libclang-20-dev libstdc++-14-dev llvm-20-dev \
 && rm -rf /var/lib/apt/lists/* llvm.sh

ENV LLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm \
    Clang_DIR=/usr/lib/llvm-20/lib/cmake/clang \
    CLANG_EXECUTABLE=/usr/bin/clang-20

WORKDIR /src
