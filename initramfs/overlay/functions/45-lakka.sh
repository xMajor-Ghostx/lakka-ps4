#!/bin/sh

# Lakka (LibreELEC 6.1) on PS4. Lakka's SYSTEM squashfs is used exactly as released and
# mounted read-only; PS4 additions (Liverpool-patched Mesa) live in a separate read-only
# layer; only STORAGE (an ext4 image file) is written to. Updates = a newer SYSTEM.
#
# Files in /data/linux/boot (copied by the Lakka for PS4 app):
#   SYSTEM                    Lakka's system image (squashfs), unmodified
#   lakka-ps4-layer.sqfs      PS4 layer: usr/lib = Mesa 25.1.9 + Liverpool patch
#   lakka-storage.img         STORAGE (ext4), made from the space the app reserved:
#   lakka-storage.img.alloc   zeros written by the app from OrbisOS (fast); renamed here
#   install-choice            the app's choice (size_gib=N), removed after the install
#   lakka-installed           written once STORAGE exists: the app stops asking/reserving
LAKKA_BOOT=/ps4hdd/data/linux/boot
LAKKA_SYSTEM=${LAKKA_BOOT}/SYSTEM
LAKKA_LAYER=${LAKKA_BOOT}/lakka-ps4-layer.sqfs
LAKKA_STORAGE=${LAKKA_BOOT}/lakka-storage.img
LAKKA_ALLOC=${LAKKA_STORAGE}.alloc
LAKKA_CHOICE=${LAKKA_BOOT}/install-choice
LAKKA_DONE=${LAKKA_BOOT}/lakka-installed
LAKKA_DEFAULT_GIB=32
LAKKA_KEEP_FREE_GIB=10

lakka_say() {
	# User-facing message, shown even in quiet (release) mode.
	kernel_msg_lvl 4 "$*"
	echo -ne "\033[1;33m>>> $*\033[0m\n" >&2
}

# Warnings are never hidden, even in quiet (release) mode: a failed boot must say why.
ewarn() {
	kernel_msg_lvl 4 "${*}"
	echo -ne "\033[1;30m>\033[0;33m>\033[1;33m> \033[0m${*}\n" >&2
}

# ---------------------------------------------------------------- PS4 drive
# Same handling as ChromeOS for PS4 (brunch-ps4 functions/45-chromeos.sh), see there for
# the reasons: the key comes from the loader; UFS allocation state is only written at
# umount, so every allocation happens before lakka_ps4hdd_commit and nothing allocates
# on the PS4 drive afterwards (STORAGE only rewrites blocks of a fully written file).
lakka_check_hdd_key() {
	local k=/key/eap_hdd_key.bin
	if [ ! -s "${k}" ] && [ -s /sys/firmware/ps4/eap_hdd_key ]; then
		mkdir -p /key && cat /sys/firmware/ps4/eap_hdd_key > "${k}"
	fi
	[ -s "${k}" ] || { eerror "No PS4 drive key: the Linux loader did not pass /key/eap_hdd_key.bin."; return 1; }
	[ "$(stat -c %s "${k}")" = 32 ] || { eerror "PS4 drive key has the wrong size ($(stat -c %s "${k}") bytes, expected 32)."; return 1; }
	[ -n "$(od -An -tx1 "${k}" | tr -d ' \n0')" ] || { eerror "PS4 drive key is all zeros: the loader could not read it on this firmware."; return 1; }
	return 0
}

LAKKA_UFS_OPTS=ufstype=ufs2,noatime
lakka_ps4hdd_mount() {
	mkdir -p /ps4hdd
	mount -t ufs -o "${LAKKA_UFS_OPTS}" /dev/mapper/ps4hdd /ps4hdd || return 1
	if awk '$2 == "/ps4hdd" { split($4, o, ","); if (o[1] == "ro") f = 1 } END { exit !f }' /proc/mounts; then
		eerror "The PS4 drive file system is not clean (Linux mounted it read-only)."
		eerror "Boot OrbisOS once and shut it down normally, then try again."
		umount /ps4hdd
		return 2
	fi
	if [ ! -d /ps4hdd/app ] || [ ! -d /ps4hdd/data ]; then
		umount /ps4hdd
		return 1
	fi
	return 0
}

ps4hdd_open() {
	mountpoint -q /ps4hdd && return 0
	lakka_check_hdd_key || return 1
	local part skip rc
	case "$(ps4_southbridge)" in
	Aeolia) skip="" ;;
	Belize|Baikal) skip="--skip 111669149696" ;;
	*) eerror "Unknown PS4 southbridge (PCI 00:14.4 = $(cat /sys/bus/pci/devices/0000:00:14.4/device 2>/dev/null)): not opening the drive."; return 1 ;;
	esac
	if [ -b /dev/mapper/ps4hdd ]; then
		lakka_ps4hdd_mount && return 0
		cryptsetup remove ps4hdd 2>/dev/null
	fi
	for part in /dev/sd?27; do
		[ -b "${part}" ] || continue
		kernel_msg "Opening ${part} as /dev/mapper/ps4hdd"
		# shellcheck disable=SC2086
		cryptsetup -d /key/eap_hdd_key.bin --cipher aes-xts-plain64 -s 256 --offset 0 ${skip} \
			create ps4hdd "${part}" || { ewarn "cryptsetup failed on ${part}."; continue; }
		lakka_ps4hdd_mount; rc=$?
		[ "${rc}" -eq 0 ] && { kernel_msg "Mounted ${part} (PS4 drive) on /ps4hdd"; return 0; }
		cryptsetup remove ps4hdd 2>/dev/null
		[ "${rc}" -eq 2 ] && return 1
		ewarn "${part} is not the PS4 drive (no UFS2 with app/ and data/ under this key)."
	done
	ewarn "No PS4 internal drive found (no /dev/sd?27)."
	return 1
}

lakka_ps4hdd_commit() {
	# Write UFS allocation state to disk: umount + mount. Before any loop device opens.
	sync
	umount /ps4hdd || { ewarn "Could not unmount /ps4hdd to commit it."; return 0; }
	lakka_ps4hdd_mount || { eerror "Could not mount the PS4 drive again."; return 1; }
	kernel_msg "PS4 drive committed (umount + mount)"
}

lakka_port_wait() {
	local i=0
	while [ "${i}" -lt "$2" ]; do
		[ "$(cat "$1/state" 2>/dev/null)" = configured ] && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

lakka_port_cycle() {
	echo 1 > "$1/disable" 2>/dev/null || return 1
	sleep "$2"
	echo 0 > "$1/disable" 2>/dev/null
}

lakka_aeolia_hdd_kick() {
	# Aeolia (Fat CUH-10xx/11xx): the HDD's USB-SATA bridge fails SuperSpeed addressing after
	# the loader's OrbisOS shutdown. Port off -> it comes up on USB 2 -> USB 2 port off -> back
	# on SuperSpeed. Verified on a CUH-1115A (see brunch-ps4 45-chromeos.sh).
	[ "$(ps4_southbridge)" = Aeolia ] || return 1
	local b hub n=0 mid="" p peer rc=1
	for b in 1 2 3 4 5 6 7 8 9 10 11 12; do
		hub="/sys/bus/usb/devices/usb${b}"
		[ -e "${hub}" ] || continue
		readlink -f "${hub}" | grep -q '0000:00:14\.7/' || continue
		[ "$(cat "${hub}/speed" 2>/dev/null)" = 5000 ] || continue
		n=$((n + 1))
		[ "${n}" -eq 2 ] && { mid="${hub}"; break; }
	done
	[ -n "${mid}" ] || return 1
	for p in "${mid}"/*-0:1.0/usb*-port*; do
		[ -e "${p}/disable" ] || continue
		[ "$(cat "${p}/state" 2>/dev/null)" = configured ] && continue
		peer="$(readlink -f "${p}/peer" 2>/dev/null)"
		[ -n "${peer}" ] && [ -e "${peer}/disable" ] || continue
		[ "$(cat "${peer}/state" 2>/dev/null)" = configured ] && continue
		kernel_msg "Aeolia HDD: SuperSpeed link down on $(basename "${p}"), resetting the bridge"
		lakka_port_cycle "${p}" 2 || continue
		if lakka_port_wait "${peer}" 8; then
			lakka_port_cycle "${peer}" 3
			lakka_port_wait "${p}" 10 && kernel_msg "Aeolia HDD: SuperSpeed link up" \
				|| kernel_msg "Aeolia HDD: staying on USB 2"
		fi
		rc=0
	done
	return "${rc}"
}

lakka_wait_ps4_drive() {
	local i=0
	ls /dev/sd?27 >/dev/null 2>&1 && return 0
	lakka_say "Waiting for the PS4 drive ..."
	while [ "${i}" -lt "$1" ]; do
		sleep 1
		i=$((i + 1))
		ls /dev/sd?27 >/dev/null 2>&1 && return 0
		[ "${i}" -ge 5 ] && [ $(( (i - 5) % 10 )) -eq 0 ] && lakka_aeolia_hdd_kick
	done
	ls /dev/sd?27 >/dev/null 2>&1
}

# ---------------------------------------------------------------- STORAGE
lakka_choice_gib() {
	sed -n 's/^size_gib=//p' "${LAKKA_CHOICE}" 2>/dev/null | head -n 1 | tr -cd '0-9'
}

lakka_create_storage() {
	# STORAGE file does not exist yet. Before the commit (it allocates on the PS4 drive).
	if [ -f "${LAKKA_ALLOC}" ]; then
		# Reserved by the app from OrbisOS: same partition, a rename moves no data.
		mv "${LAKKA_ALLOC}" "${LAKKA_STORAGE}" || { eerror "Could not rename ${LAKKA_ALLOC}."; return 1; }
		kernel_msg "STORAGE: using the space reserved by the app ($(( $(stat -c %s "${LAKKA_STORAGE}") >> 30 )) GB)"
	else
		# No reservation (the app could not jailbreak): write the zeros here. Slow (~13 MB/s).
		local gib free_gib
		gib="$(lakka_choice_gib)"; gib=${gib:-${LAKKA_DEFAULT_GIB}}
		free_gib=$(( $(df -k /ps4hdd | awk 'NR==2 {print $4}') / 1048576 ))
		if [ $((gib + LAKKA_KEEP_FREE_GIB)) -gt "${free_gib}" ]; then
			eerror "Not enough space on the PS4 drive for ${gib} GB of Lakka storage (${free_gib} GB free, ${LAKKA_KEEP_FREE_GIB} GB stay free)."
			return 1
		fi
		lakka_say "Preparing ${gib} GB of Lakka storage on the PS4 drive (one time, slow: about $((gib * 1024 / 13 / 60 + 1)) min) ..."
		ps4_change_led orange
		local i=0
		: > "${LAKKA_STORAGE}.part" || return 1
		while [ "${i}" -lt "${gib}" ]; do
			dd if=/dev/zero of="${LAKKA_STORAGE}.part" bs=1M count=1024 seek=$((i * 1024)) conv=notrunc 2>/dev/null \
				|| { eerror "Writing the storage failed at ${i} GB."; rm -f "${LAKKA_STORAGE}.part"; return 1; }
			i=$((i + 1))
			printf '\r  %d / %d GB' "${i}" "${gib}" >&2
		done
		echo >&2
		mv "${LAKKA_STORAGE}.part" "${LAKKA_STORAGE}" || return 1
		ps4_change_led white
	fi
	rm -f "${LAKKA_CHOICE}" "${LAKKA_ALLOC}.part"
	echo "Lakka for PS4 storage: ${LAKKA_STORAGE}" > "${LAKKA_DONE}"
}

lakka_format_storage() {
	# Formats only a fresh STORAGE: no ext2/3/4 superblock (magic 0xEF53 at byte 1080) AND the
	# first 2 MiB all zeros (the app's reservation). Anything else is never formatted, also
	# not after a failed mount. (busybox blkid exits 0 even without a file system, so it can't
	# be used for this.)
	local dev="$1"
	[ "$(od -An -tx1 -j1080 -N2 "${dev}" | tr -d ' \n')" = 53ef ] && return 0
	if [ -n "$(dd if="${dev}" bs=1M count=2 2>/dev/null | od -An -tx1 | tr -d ' 0\n*')" ]; then
		eerror "The Lakka storage has no ext4 file system but is not empty either: not formatting it."
		eerror "To start fresh, delete lakka-storage.img and lakka-installed in /data/linux/boot (erases saves)."
		return 1
	fi
	lakka_say "Formatting Lakka storage (one time) ..."
	ps4_change_led orange
	# Inode tables and journal written in full, no discard: zero-out/discard through loop on
	# the PS4's UFS can't be trusted (see brunch-ps4 cros_reset_state).
	mkfs.ext4 -q -F -L STORAGE -b 4096 -E lazy_itable_init=0,lazy_journal_init=0,nodiscard "${dev}" \
		|| { eerror "mkfs.ext4 on the Lakka storage failed."; return 1; }
	sync
	ps4_change_led white
}

# ---------------------------------------------------------------- mount
lakka_mount_inner() {
	has_cmdline "nolakka" && return 1
	lakka_check_hdd_key || return 1
	lakka_wait_ps4_drive 40
	if ! ps4hdd_open; then
		eerror "The PS4 drive could not be opened (southbridge: $(ps4_southbridge))."
		return 1
	fi
	[ -f "${LAKKA_SYSTEM}" ] || { eerror "${LAKKA_SYSTEM#/ps4hdd} is missing: launch the Lakka for PS4 app again."; return 1; }

	# All changes to the PS4 drive's allocation first, then commit.
	if [ ! -f "${LAKKA_STORAGE}" ]; then
		lakka_create_storage || return 1
	else
		# Installed already: a reservation left by the app is not needed.
		[ -f "${LAKKA_ALLOC}" ] || [ -f "${LAKKA_ALLOC}.part" ] && { kernel_msg "Removing unused ${LAKKA_ALLOC}"; rm -f "${LAKKA_ALLOC}" "${LAKKA_ALLOC}.part"; }
		[ -f "${LAKKA_DONE}" ] || echo "Lakka for PS4 storage: ${LAKKA_STORAGE}" > "${LAKKA_DONE}"
		rm -f "${LAKKA_CHOICE}"
	fi
	lakka_ps4hdd_commit || return 1

	# SYSTEM (read-only) on /newroot.
	mount -t squashfs -o ro,loop "${LAKKA_SYSTEM}" /newroot || { eerror "Cannot mount Lakka's SYSTEM image."; return 1; }
	[ -x /newroot/usr/lib/systemd/systemd ] || { eerror "SYSTEM has no /usr/lib/systemd/systemd."; umount /newroot; return 1; }

	# STORAGE (read-write) on /newroot/storage.
	local dev
	dev="$(losetup -f)" && losetup "${dev}" "${LAKKA_STORAGE}" \
		|| { eerror "losetup ${LAKKA_STORAGE} failed."; umount /newroot; return 1; }
	lakka_format_storage "${dev}" || { losetup -d "${dev}"; umount /newroot; return 1; }
	e2fsck -p "${dev}" >/dev/null 2>&1
	mount -t ext4 -o rw,noatime "${dev}" /newroot/storage || { eerror "Cannot mount the Lakka storage."; losetup -d "${dev}"; umount /newroot; return 1; }

	# /flash: Lakka's boot partition. Here a small read-only tmpfs (kernel and SYSTEM live
	# on the PS4 drive and are updated by the app, not from inside Lakka).
	mount -t tmpfs -o ro,mode=755,size=1m lakka-flash /newroot/flash

	ps4_lakka=1
	init=/usr/lib/systemd/systemd
	einfo "$(sed -n 's/^PRETTY_NAME=//p' /newroot/etc/os-release | tr -d '"') (PS4 drive, storage $(( $(stat -c %s "${LAKKA_STORAGE}") >> 30 )) GB)."
}

lakka_mount() {
	lakka_mount_inner
}

# ---------------------------------------------------------------- before switch_root
# Read-only PS4 layer over SYSTEM's /usr/lib (Mesa with Liverpool support), on its own
# tmpfs-free loop mount so switch_root keeps it. SYSTEM itself is never modified.
lakka_ps4_layer() {
	[ -n "${ps4_lakka}" ] || return 0
	if [ -f "${LAKKA_LAYER}" ]; then
		mkdir -p /lakkalayer
		if mount -t squashfs -o ro,loop "${LAKKA_LAYER}" /lakkalayer; then
			mount -t overlay -o ro,lowerdir=/lakkalayer/usr/lib:/newroot/usr/lib lakka-ps4 /newroot/usr/lib \
				&& einfo "PS4 layer: $(cat /lakkalayer/LAYER_VERSION 2>/dev/null)" \
				|| ewarn "Could not stack the PS4 layer over /usr/lib (no PS4 graphics)."
		else
			ewarn "Could not mount ${LAKKA_LAYER}."
		fi
	else
		ewarn "No ${LAKKA_LAYER#/ps4hdd}: PS4 graphics support missing."
	fi
	# What Lakka's own init does before switch_root.
	chroot /newroot /usr/sbin/kernel-overlays-setup >/dev/null 2>&1
	# Updates come as a new PKG (new SYSTEM), not from inside Lakka.
	echo "" > /newroot/dev/.update_disabled 2>/dev/null
	lakka_ps4_audio
}

# HDMI audio: the GPU's HDA codec has the HDMI pin (0x03) and an S/PDIF pin (0x07) that
# reports "plugged" although the Slim has no optical port. Mark it "not connected" on the
# Slim and the Fat (HDMI-only, as in ChromeOS for PS4). Pro: untested, left alone.
lakka_ps4_audio() {
	local sys=/sys hw gpu sb model
	[ -d /newroot/sys/class ] && sys=/newroot/sys
	hw=${sys}/class/sound/hwC0D0
	[ -w "${hw}/user_pin_configs" ] && [ -w "${hw}/reconfig" ] || return 0
	[ "$(cat "${hw}/vendor_id" 2>/dev/null)" = "0x1002aa01" ] || return 0
	sb="$(cat "${sys}/bus/pci/devices/0000:00:14.4/device" 2>/dev/null)"
	gpu="$(cat "${sys}/bus/pci/devices/0000:00:01.0/device" 2>/dev/null)"
	case "${sb}" in
		0x90a1|0x90cc) model="Fat" ;;
		0x90db) [ "${gpu}" = "0x9923" ] || return 0; model="Slim" ;;
		*) return 0 ;;
	esac
	echo "0x07 0x411111f0" > "${hw}/user_pin_configs" && echo 1 > "${hw}/reconfig" \
		&& einfo "PS4 ${model}: S/PDIF pin disabled, HDMI is the only audio output."
}
