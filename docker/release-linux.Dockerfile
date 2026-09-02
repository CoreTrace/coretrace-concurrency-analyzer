# SPDX-License-Identifier: Apache-2.0
#
# The image published to GHCR for a release tag.
#
# Distinct from ci-linux.Dockerfile, which reproduces a CI job and stops at a
# configured build environment. This one builds the analyzer and ships it.
#
# Two stages, because what the build needs and what the run needs differ. The
# build needs llvm-20-dev, libclang-20-dev and a C++ toolchain. The run needs
# clang-20 itself: the analyzer compiles the sources it is handed before it
# analyses them, and the compiler backend was told at configure time where
# clang lives. libstdc++-14-dev is there for the same reason -- analysing C++
# means parsing its standard headers, so they have to exist in the image.
ARG BASE_IMAGE=ubuntu:24.04

FROM ${BASE_IMAGE} AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git gnupg lsb-release ninja-build \
        software-properties-common wget \
 && wget -q https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh 20 \
 && apt-get update && apt-get install -y --no-install-recommends \
        clang-20 libclang-20-dev llvm-20-dev libstdc++-14-dev \
 && rm -rf /var/lib/apt/lists/* llvm.sh

WORKDIR /src
COPY . .

# The same four cache variables every other build of this project sets; they
# are what the compiler backend resolves clang against.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/coretrace \
        -DBUILD_TESTING=OFF \
        -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm \
        -DClang_DIR=/usr/lib/llvm-20/lib/cmake/clang \
        -DCLANG_EXECUTABLE=/usr/bin/clang-20 \
        -DCLANG_RESOURCE_DIR="$(/usr/bin/clang-20 -print-resource-dir)" \
 && cmake --build build --parallel \
 && cmake --install build

FROM ${BASE_IMAGE} AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates gnupg lsb-release software-properties-common wget \
 && wget -q https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh 20 \
 && apt-get update && apt-get install -y --no-install-recommends \
        clang-20 libstdc++-14-dev \
 && apt-get purge -y --auto-remove gnupg software-properties-common wget \
 && rm -rf /var/lib/apt/lists/* llvm.sh

COPY --from=build /opt/coretrace/bin/coretrace_concurrency_analyzer /usr/local/bin/

# Sources to analyse are mounted here, so paths in a report stay relative to
# something the caller recognises: docker run -v "$PWD:/work" <image> file.c --analyze
WORKDIR /work

ENTRYPOINT ["/usr/local/bin/coretrace_concurrency_analyzer"]
CMD ["--help"]
