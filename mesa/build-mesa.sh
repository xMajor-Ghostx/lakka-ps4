#!/bin/bash
# PS4 (Liverpool/Gladius) Mesa 25.1.9 for Lakka 6.1: radeonsi + RADV + software fallback.
# Same version and the same GBM/EGL/glvnd, no-window-system layout as Lakka's own Mesa, so
# the whole set below replaces Lakka's in the read-only PS4 layer (SYSTEM stays untouched).
# Output: out/usr/lib/{libgallium-25.1.9.so, libEGL_mesa.so.0.0.0, libgbm.so.1.0.0,
#         gbm/dri_gbm.so, libvulkan_radeon.so}
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
L=/mnt/build/lakka-ps4
LAKKA_LLVM=$L/work/mesa-orig/usr/lib/libLLVM.so.19.1   # extracted from Lakka's SYSTEM
IMG=lakka-ps4-mesa:debian12
[ -f "$LAKKA_LLVM" ] || { echo "missing $LAKKA_LLVM" >&2; exit 1; }

# fresh, patched source
rm -rf "$HERE/src" "$HERE/build" "$HERE/out"
mkdir -p "$HERE/src" "$HERE/out/usr/lib/gbm"
tar xf "$HERE/dl/mesa-25.1.9.tar.xz" -C "$HERE/src" --strip-components=1
(cd "$HERE/src" && patch -p1 -s < "$HERE/mesa-25.1.9-ps4-liverpool.patch")
cp "$LAKKA_LLVM" "$HERE/lakka-libLLVM.so.19.1"

docker build -q -t "$IMG" "$HERE" >/dev/null
docker run --rm -v "$HERE:/work" "$IMG" bash -euc '
  # hand everything back to the user even if the build fails (root in the container)
  trap "chown -R '"$(id -u):$(id -g)"' /work/build /work/src 2>/dev/null || true" EXIT
  # link against Lakka'"'"'s exact libLLVM (same soname + LLVM_19.1 symbol versions)
  cp /work/lakka-libLLVM.so.19.1 /usr/lib/llvm-19/lib/libLLVM.so.19.1
  cd /work/src
  meson setup /work/build --prefix=/usr --libdir=lib --buildtype=release -Db_ndebug=true \
    -Dgallium-drivers=radeonsi,softpipe -Dvulkan-drivers=amd \
    -Dplatforms= -Dglx=disabled -Dglvnd=enabled -Dgbm=enabled -Degl=enabled \
    -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
    -Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=false \
    -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-rusticl=false \
    -Dgallium-extra-hud=false -Dvideo-codecs= -Dshader-cache=enabled \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
    -Dmicrosoft-clc=disabled -Dtools= \
    -Dc_args="-march=x86-64 -mtune=generic" -Dcpp_args="-march=x86-64 -mtune=generic"
  ninja -C /work/build
  DESTDIR=/work/build/staging ninja -C /work/build install >/dev/null
'
S="$HERE/build/staging/usr/lib"
cp -L "$S/libgallium-25.1.9.so" "$HERE/out/usr/lib/"
cp -L "$S/libEGL_mesa.so.0" "$HERE/out/usr/lib/libEGL_mesa.so.0.0.0"
cp -L "$S/libgbm.so.1" "$HERE/out/usr/lib/libgbm.so.1.0.0"
cp -L "$S/gbm/dri_gbm.so" "$HERE/out/usr/lib/gbm/"
cp -L "$S/libvulkan_radeon.so" "$HERE/out/usr/lib/"
rm -f "$HERE/lakka-libLLVM.so.19.1"

cd "$HERE/out/usr/lib"
echo "== built"; find . -type f -printf '%10s %p\n'
for f in $(find . -type f); do echo "-- $f NEEDED: $(readelf -d "$f" | awk '/NEEDED/{print $NF}' | tr -d '[]' | tr '\n' ' ')"; done
echo "== max GLIBC: $(find . -type f -exec objdump -T {} \; | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1) (Lakka has 2.38)"
echo "== PS4 chips in radeonsi: $(strings libgallium-25.1.9.so | grep -c -x -E 'LIVERPOOL|GLADIUS'), RADV: $(strings libvulkan_radeon.so | grep -c -x -E 'LIVERPOOL|GLADIUS')"
