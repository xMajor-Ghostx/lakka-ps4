#!/bin/bash
# Build a PS4 Linux loader PKG from any kernel + initramfs (+ extra files) combo.
#   ./make-pkg.sh [pkg.conf]
# Output: out/<CONTENT_ID>.pkg
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CONF=$(realpath "${1:-$HERE/pkg.conf}")
[ -f "$CONF" ] || { echo "no $CONF" >&2; exit 1; }
EXTRA=(); ICON=; RESERVE_NAME=; RESERVE_EXTRA_MB=0; KEEP_FREE_GB=10; SIZES="32 64 128 256"
MIN_GB=8; RECOMMENDED_GB=32; ALLOW_USB=1; DONE_MARKER=install-done
cd "$(dirname "$CONF")"
# shellcheck source=/dev/null
. "$CONF"
export OO_PS4_TOOLCHAIN=${OO_PS4_TOOLCHAIN:-$HOME/OpenOrbis/PS4Toolchain}
P=$HERE/installer
A=$P/assets/linux
die() { echo "make-pkg: $*" >&2; exit 1; }

# --- config checks
[[ $TITLE_ID =~ ^[A-Z]{4}[0-9]{5}$ ]] || die "TITLE_ID must be 4 capital letters + 5 digits (got '$TITLE_ID')"
[[ $CONTENT_LABEL =~ ^[A-Z0-9]{16}$ ]] || die "CONTENT_LABEL must be exactly 16 capital letters/digits"
[[ $VERSION =~ ^[0-9]{2}\.[0-9]{2}$ ]] || die "VERSION must look like 01.00"
[[ $TITLE != *\'* && $TITLE != *\"* ]] || die "TITLE must not contain quotes"
CONTENT_ID="IV0000-${TITLE_ID}_00-${CONTENT_LABEL}"
[ -d "$OO_PS4_TOOLCHAIN/bin/linux" ] || die "OpenOrbis toolchain not found at $OO_PS4_TOOLCHAIN"

# --- collect "source -> installed name"
declare -a SRCS NAMES
add() {   # add <source> <name>
  local src=$1 name=$2
  [ -f "$src" ] || die "missing file: $src"
  [[ $name =~ ^[A-Za-z0-9._-]+$ && ${#name} -lt 64 ]] || die "bad installed name '$name' (letters, digits, . _ - only)"
  case $name in manifest.txt|build-id|*.part|.pkg-probe|install-choice|pkg-dryrun) die "'$name' is a reserved name" ;; esac
  for n in "${NAMES[@]:-}"; do [ "$n" != "$name" ] || die "two files install as '$name'"; done
  SRCS+=("$(realpath "$src")"); NAMES+=("$name")
}
add "$KERNEL" bzImage
add "$INITRAMFS" initramfs.cpio.gz
for e in "${EXTRA[@]}"; do
  if [[ $e == *:* ]]; then add "${e%%:*}" "${e##*:}"; else add "$e" "$(basename "$e")"; fi
done
[ "${#NAMES[@]}" -le 32 ] || die "at most 32 files"
[ -f "$PAYLOAD" ] || die "missing payload: $PAYLOAD"
PAYLOAD_NAME=$(basename "$PAYLOAD")
[[ $PAYLOAD_NAME =~ ^[A-Za-z0-9._-]+$ ]] || die "bad payload file name '$PAYLOAD_NAME'"
for n in "${NAMES[@]}"; do [ "$n" != "$PAYLOAD_NAME" ] || die "payload name clashes with '$n'"; done
[ "$(stat -c %s "$PAYLOAD")" -le 524288 ] || die "payload is larger than 512 KB"
if [ -n "$RESERVE_NAME" ]; then
  [[ $RESERVE_NAME =~ ^[A-Za-z0-9._-]+$ ]] || die "bad RESERVE_NAME '$RESERVE_NAME'"
  [[ $DONE_MARKER =~ ^[A-Za-z0-9._-]+$ ]] || die "bad DONE_MARKER '$DONE_MARKER'"
  for n in "${NAMES[@]}" "$PAYLOAD_NAME"; do
    [ "$n" != "$RESERVE_NAME" ] || die "RESERVE_NAME clashes with a copied file"
    [ "$n" != "$DONE_MARKER" ] || die "DONE_MARKER clashes with a copied file"
  done
  for v in RESERVE_EXTRA_MB MIN_GB RECOMMENDED_GB KEEP_FREE_GB; do [[ ${!v} =~ ^[0-9]+$ ]] || die "$v must be a number"; done
  [[ $SIZES =~ ^[0-9]+( [0-9]+){0,7}$ ]] || die "SIZES: up to 8 numbers separated by spaces"
  [[ $ALLOW_USB =~ ^[01]$ ]] || die "ALLOW_USB must be 0 or 1"
fi
# Template dummies start with this marker: such a PKG never boots anything (see main.c).
DUMMY=0
for f in "${SRCS[@]}" "$PAYLOAD"; do head -c 19 "$f" | grep -q '^PS4-LINUX-PKG-DUMMY' && DUMMY=1; done
[ "$DUMMY" = 1 ] && echo "note: dummy files found -> template build (installs to /data/linux/boot-template, never boots)" >&2
# light sanity checks, warnings only: any combo is allowed
[ "$DUMMY" = 1 ] || head -c 4096 "${SRCS[0]}" | grep -q 'HdrS' || echo "warning: $KERNEL does not look like a bzImage" >&2
[ "$DUMMY" = 1 ] || case $(head -c 6 "${SRCS[1]}" | od -An -tx1 | tr -d ' \n') in
  1f8b*|fd377a585a00*|28b52ffd*|303730373031|303730373032|02214c18*) ;;
  *) echo "warning: $INITRAMFS is not gzip/xz/zstd/lz4/plain cpio" >&2 ;;
esac

# --- stage (hard links on the same filesystem, else reflink/copy)
rm -rf "$A"; mkdir -p "$A"
stage() { ln -f "$1" "$2" 2>/dev/null || cp --reflink=auto "$1" "$2"; }
{
  echo "payload=$PAYLOAD_NAME"
  for n in "${NAMES[@]}"; do echo "file=$n"; done
  if [ -n "$RESERVE_NAME" ]; then
    echo "reserve=$RESERVE_NAME"
    echo "done_marker=$DONE_MARKER"
    echo "reserve_extra_mb=$RESERVE_EXTRA_MB"
    echo "sizes=${SIZES// /,}"
    echo "min_gb=$MIN_GB"
    echo "recommended_gb=$RECOMMENDED_GB"
    echo "usb=$ALLOW_USB"
  fi
  echo "keep_free_gb=$KEEP_FREE_GB"
} > "$A/manifest.txt"
for i in "${!NAMES[@]}"; do stage "${SRCS[$i]}" "$A/${NAMES[$i]}"; done
cp "$PAYLOAD" "$A/$PAYLOAD_NAME"
# build stamp: the app skips copying when the console already has exactly these files
(cd "$A" && sha256sum manifest.txt "$PAYLOAD_NAME" "${NAMES[@]}" | sha256sum | cut -c1-32) > "$A/build-id"

# --- icon
if [ -n "$ICON" ]; then
  magick "$ICON" -resize 512x512! -depth 8 -type TrueColor -define png:color-type=2 "$P/sce_sys/icon0.png"
else
  magick -size 512x512 xc:'#2b2f36' -fill '#f2f2f2' -gravity center -font DejaVu-Sans-Bold \
    -pointsize 120 -annotate +0-40 'PS4' -pointsize 64 -annotate +0+80 'Linux' \
    -depth 8 -type TrueColor -define png:color-type=2 "$P/sce_sys/icon0.png"
fi

# --- build
cd "$P"
mapfile -t ASSET_LIST < <(cd "$P" && ls -1 assets/linux | sed 's|^|assets/linux/|')
MK=(TITLE="$TITLE" VERSION="$VERSION" TITLE_ID="$TITLE_ID" CONTENT_ID="$CONTENT_ID" ASSETS="${ASSET_LIST[*]}")
[ "$DUMMY" = 1 ] && MK+=(EXTRA_CFLAGS=-DTEMPLATE_DUMMY)
make -s "${MK[@]}" clean >/dev/null
make -s "${MK[@]}" 2>&1 | grep -v -E '^\s*\[|parallel|^$' || true
[ -f "$P/$CONTENT_ID.pkg" ] || die "PKG build failed"
mkdir -p "$HERE/out"
mv -f "$P/$CONTENT_ID.pkg" "$HERE/out/"
rm -rf "$A"   # staged links/copies are not needed any more
echo "PKG: $HERE/out/$CONTENT_ID.pkg ($(stat -c %s "$HERE/out/$CONTENT_ID.pkg") bytes)"
DSTDIR=/data/linux/boot; [ "$DUMMY" = 1 ] && DSTDIR=/data/linux/boot-template
echo "  installs to $DSTDIR: ${NAMES[*]} + $PAYLOAD_NAME"
USBTXT=; [ "$ALLOW_USB" = 1 ] && USBTXT=" or USB"
[ -n "$RESERVE_NAME" ] && echo "  install menu: PS4 drive$USBTXT, sizes $SIZES GB (min $MIN_GB, recommended $RECOMMENDED_GB) -> $RESERVE_NAME"
[ "$DUMMY" = 1 ] && echo "  TEMPLATE BUILD: never sends the payload or boots"
true
