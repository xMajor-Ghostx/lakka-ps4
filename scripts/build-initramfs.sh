#!/bin/bash
# Build initramfs.cpio.gz for Lakka on PS4 from the PS4 Linux initramfs.
# Adds: Lakka boot (functions/45-lakka.sh: SYSTEM + PS4 layer + STORAGE), release error
# screen, network clock. Same structure as the ChromeOS for PS4 initramfs (release build).
set -euo pipefail
L=/mnt/build/lakka-ps4
ORIG=${ORIG:-"$HOME/Downloads/ps4 linux/initramfs.cpio.gz"}
STAGE=$L/initramfs/stage
OUT=${1:-$L/out/initramfs-lakka.cpio.gz}

rm -rf "$STAGE"; mkdir -p "$STAGE"
(cd "$STAGE" && zcat "$ORIG" | cpio -idm --quiet --no-absolute-filenames)
cp -a "$L/initramfs/overlay/." "$STAGE/"
cp -a "$L/initramfs/overlay-release/." "$STAGE/"
chmod +x "$STAGE"/hooks/*/*
echo release > "$STAGE/PS4_VARIANT"
# No remote access, no 6 s RAM-disk speed test on every boot.
rm -f "$STAGE/bin/dropbear" "$STAGE/bin/dropbearkey" "$STAGE/hooks/early/2-ramdisk-test"

# Lakka first, before the external/internal Linux installs of the stock initramfs.
python3 -I - "$STAGE/functions/40-install.sh" "$STAGE/init" <<'PY'
import sys
inst, init = sys.argv[1], sys.argv[2]
s = open(inst).read()
old = "ps4_locate_install() {\n\tif mountpoint -q /newroot && ! umount /newroot; then"
new = ("ps4_locate_install() {\n\tif lakka_mount; then\n\t\treturn 0\n\tfi\n"
       "\tif mountpoint -q /newroot && ! umount /newroot; then")
assert s.count(old) == 1, "40-install.sh: ps4_locate_install anchor not found"
open(inst, "w").write(s.replace(old, new))
s = open(init).read()
old = "\nps4_prepare_newroot\n"
assert s.count(old) == 1, "init: ps4_prepare_newroot anchor not found"
open(init, "w").write(s.replace(old, '\n[ -n "${ps4_lakka}" ] || ps4_prepare_newroot\n'))
PY

# GPU / Wi-Fi / BT firmware is built into the kernel; the initramfs carries none
# (regulatory.db this early deadlocks the MediaTek Wi-Fi driver on an Aeolia Fat).
if find "$STAGE/lib/firmware" -type f ! -name .keep 2>/dev/null | grep . >/dev/null; then   # not grep -q: SIGPIPE + pipefail could hide a hit
  echo "initramfs must not carry firmware files:" >&2; find "$STAGE/lib/firmware" -type f ! -name .keep >&2; exit 1
fi

# Every tool 45-lakka.sh calls must be there (busybox applets or binaries).
have() { [ -e "$STAGE/bin/$1" ] || [ -e "$STAGE/sbin/$1" ] || [ -e "$STAGE/usr/bin/$1" ] || [ -e "$STAGE/usr/sbin/$1" ] \
         || "$STAGE/bin/busybox" --list 2>/dev/null | grep -x "$1" >/dev/null; }   # not grep -q: SIGPIPE + pipefail
for t in cryptsetup mkfs.ext4 e2fsck blkid losetup mount umount mountpoint df stat dd od awk sed chroot reboot udhcpc ntpd; do
  have "$t" || { echo "initramfs lacks $t" >&2; exit 1; }
done

for f in "$STAGE"/init "$STAGE"/functions/*.sh "$STAGE"/hooks/*/*; do sh -n "$f"; done
for f in bin/dropbear functions/46-debug-ssh.sh; do
  [ ! -e "$STAGE/$f" ] || { echo "release initramfs must not contain $f" >&2; exit 1; }
done
grep -q 'if lakka_mount; then' "$STAGE/functions/40-install.sh"

mkdir -p "$(dirname "$OUT")"
(cd "$STAGE" && find . -print0 | cpio --null -o -H newc -R 0:0 --quiet | gzip -9) > "$OUT"
echo "initramfs (lakka): $OUT ($(stat -c %s "$OUT") bytes)"
