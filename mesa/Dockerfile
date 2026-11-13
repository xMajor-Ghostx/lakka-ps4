# Build environment for PS4-patched Mesa 25.1.9 that drops into Lakka 6.1.
# Debian 12 = glibc 2.36 (older than Lakka's 2.38, so the result runs there).
# LLVM 19 headers/tools from apt.llvm.org; the link uses Lakka's own libLLVM.so.19.1
# (copied in by build-mesa.sh). libdrm 2.4.125 = Lakka's version.
FROM debian:12
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential pkg-config ninja-build flex bison ca-certificates wget gnupg xz-utils \
      python3 python3-pip python3-mako python3-yaml python3-ply python3-packaging \
      libexpat1-dev zlib1g-dev libzstd-dev libelf-dev libudev-dev glslang-tools libglvnd-dev \
    && rm -rf /var/lib/apt/lists/*
RUN wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo 'deb http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-19 main' > /etc/apt/sources.list.d/llvm19.list \
    && apt-get update && apt-get install -y --no-install-recommends llvm-19-dev libllvm19 \
    && rm -rf /var/lib/apt/lists/*
RUN pip3 install --break-system-packages 'meson==1.5.2'
RUN wget -q https://dri.freedesktop.org/libdrm/libdrm-2.4.125.tar.xz \
    && tar xf libdrm-2.4.125.tar.xz && cd libdrm-2.4.125 \
    && meson setup build --prefix=/usr --libdir=lib/x86_64-linux-gnu -Damdgpu=enabled -Dradeon=enabled \
         -Dintel=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Dtests=false -Dman-pages=disabled -Dvalgrind=disabled \
    && ninja -C build install && cd / && rm -rf libdrm-2.4.125*
ENV PATH=/usr/lib/llvm-19/bin:$PATH
