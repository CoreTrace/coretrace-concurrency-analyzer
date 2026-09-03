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

# apt.llvm.org is dual-stack, and the network a container gets during this
# build resolves its AAAA record without having a route to it. wget does not
# fall back to IPv4 once it has picked that address, so it fails outright --
# while apt, which does fall back, pulls packages from the same host in the
# same layer without trouble. The runners themselves are fine: the same
# llvm.sh runs unaided in the build workflows, outside any container.
#
# The setting goes in /etc/wgetrc rather than on the command line because the
# call that actually fails is not ours: llvm.sh probes the repository with its
# own `wget --method=HEAD`, and reads an unreachable host as "your distribution
# is not supported". A flag here would never reach that invocation.
#
# The retries cover a genuinely transient failure, and -nv keeps the reason in
# the log: with -q this failed silently behind an exit code.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git gnupg lsb-release ninja-build \
        software-properties-common wget \
 && echo 'inet4_only = on' >> /etc/wgetrc \
 && wget -nv --tries=5 --waitretry=10 --retry-connrefused --timeout=30 \
        https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh 20 \
 && apt-get update && apt-get install -y --no-install-recommends \
        clang-20 libclang-20-dev llvm-20-dev libstdc++-14-dev \
 && rm -rf /var/lib/apt/lists/* llvm.sh

WORKDIR /src
COPY . .

# FetchContent clones the compiler backend, and it in turn clones the logger.
# Git negotiates those over HTTP/2 by default, which several container network
# stacks truncate mid-pack: the ref listing succeeds, the transfer dies as
# "remote end hung up", and git then misreads the truncated response as an auth
# challenge and asks for a username -- for a public repository. HTTP/1.1 carries
# the same objects and costs nothing here, being a handful of clones at image
# build time.
RUN git config --global http.version HTTP/1.1

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
 && echo 'inet4_only = on' >> /etc/wgetrc \
 && wget -nv --tries=5 --waitretry=10 --retry-connrefused --timeout=30 \
        https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh 20 \
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
