# Build environment for bg3le, based on the Steam Linux Runtime 3 (sniper) SDK
# image -- the same Debian 11 / glibc 2.31 container the game actually runs
# in via SteamLinuxRuntime_sniper. Building here (rather than on the host
# distro) keeps the shared library's glibc/SDL2/Vulkan symbol versions in
# lockstep with what LD_PRELOAD will inject it into at runtime.
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest

ENV DEBIAN_FRONTEND=noninteractive

# bullseye is fully EOL; the SDK image's bullseye-security entries (still
# pointed at the live deb.debian.org pool) 404 on the pinned point-release
# files, and archive.debian.org never mirrored a bullseye-security suite of
# its own. We only need build tooling here, not security patches, so drop
# both security lines rather than chase a working mirror for them.
RUN sed -i '/debian-security/d' /etc/apt/sources.list

# The SDK image ships Debian 11's toolchain (gcc/old clang). bg3le needs a
# clang recent enough for -std=gnu++23 plus libc++/libc++abi static archives;
# neither is default on bullseye, so pull them from apt.llvm.org directly
# (skipping llvm.sh's use of add-apt-repository, which drags in
# software-properties-common/packagekit -- themselves unavailable now that
# bullseye-security is archived).
ARG LLVM_VERSION=18
RUN apt-get update && apt-get install -y --no-install-recommends \
        wget gnupg ca-certificates \
    && wget -qO /usr/share/keyrings/llvm.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && gpg --dearmor < /usr/share/keyrings/llvm.asc > /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-${LLVM_VERSION} lld-${LLVM_VERSION} \
        libc++-${LLVM_VERSION}-dev libc++abi-${LLVM_VERSION}-dev \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-${LLVM_VERSION} 100 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-${LLVM_VERSION} 100 \
    && rm -rf /var/lib/apt/lists/*

# Build tooling + bg3le's own runtime/build-time deps (SDL2, Vulkan) +
# fetch-externals.sh needs (git, curl, bsdtar from libarchive-tools) +
# find_slots.py/recover_symbols.py need python3.
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build git curl unzip pkg-config \
        libsdl2-dev libvulkan-dev python3 python3-pip \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang
ENV CXX=clang++

WORKDIR /work
