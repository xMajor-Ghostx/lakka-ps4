#!/bin/bash
# Read-only PS4 layer for Lakka 6.1: out/lakka-ps4-layer.sqfs, stacked by the initramfs over
# SYSTEM's /usr/lib. Contents: Mesa 25.1.9 with Liverpool support (mesa/build-mesa.sh),
# replacing Lakka's own Mesa files of the same version. SYSTEM itself is never modified.
set -euo pipefail
L=/mnt/build/lakka-ps4
M=$L/mesa/out/usr/lib
OUT=$L/out/lakka-ps4-layer.sqfs
FILES=(libgallium-25.1.9.so libEGL_mesa.so.0.0.0 libgbm.so.1.0.0 gbm/dri_gbm.so libvulkan_radeon.so)

for f in "${FILES[@]}"; do [ -f "$M/$f" ] || { echo "missing $M/$f (run mesa/build-mesa.sh)" >&2; exit 1; }; done
[ "$(strings "$M/libgallium-25.1.9.so" | grep -c -x -E 'LIVERPOOL|GLADIUS')" = 2 ] || { echo "Mesa build lacks the PS4 chips" >&2; exit 1; }
# Each file must replace one that exists in Lakka's SYSTEM (same name, same place).
LS=$(unsquashfs -l "$L/dl/Lakka-Generic.x86_64-6.1/target/SYSTEM" 2>/dev/null)
for f in "${FILES[@]}"; do grep -qxF "squashfs-root/usr/lib/$f" <<<"$LS" || { echo "SYSTEM has no /usr/lib/$f" >&2; exit 1; }; done

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/usr/lib/gbm"
for f in "${FILES[@]}"; do install -m 644 "$M/$f" "$T/usr/lib/$f"; done
patch_id=$(sha256sum "$L/mesa/mesa-25.1.9-ps4-liverpool.patch" | cut -c1-12)
echo "Lakka 6.1 PS4 layer $(date +%F): Mesa 25.1.9 + Liverpool patch $patch_id" > "$T/LAYER_VERSION"
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
mksquashfs "$T" "$OUT" -comp zstd -Xcompression-level 19 -all-root -no-xattrs -quiet >/dev/null
echo "layer: $OUT ($(stat -c %s "$OUT") bytes): $(cat "$T/LAYER_VERSION")"
