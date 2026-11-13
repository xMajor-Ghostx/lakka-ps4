#!/bin/sh

# Release build: no rescue shell (nobody has a keyboard there, and a shell hides why the
# boot failed). Show the error, save it to the PS4 drive (readable over FTP), close the
# PS4 drive cleanly and restart into OrbisOS. Same behaviour as ChromeOS for PS4.
LAKKA_ERROR_FILE=${LAKKA_BOOT}/lakka-error.txt
LAKKA_ERROR_WAIT=60

lakka_tty() {
	if [ -n "${console}" ] && [ -c "/dev/${console}" ]; then echo "/dev/${console}"
	elif [ -c /dev/tty1 ]; then echo /dev/tty1
	else echo /dev/console; fi
}

lakka_error_log() {
	echo "Lakka for PS4 - boot error"
	echo "date: $(date)  kernel: $(uname -r)  southbridge: $(ps4_southbridge 2>/dev/null)"
	echo
	echo "--- initramfs messages"
	dmesg | grep '\[initramfs-ps4\]' | tail -n 100
	echo
	echo "--- kernel log (last 150 lines)"
	dmesg | tail -n 150
	echo
	echo "--- init.log"
	cat /init.log 2>/dev/null
	echo
	echo "--- mounts"
	cat /proc/mounts
}

lakka_release_ps4hdd_close() {
	# Everything stacked on the PS4 drive goes first (storage, SYSTEM, layer and their loop
	# devices), so the umount can commit UFS.
	local m l
	for m in /newroot/usr/lib /newroot/storage /newroot/flash /newroot /lakkalayer; do
		mountpoint -q "${m}" 2>/dev/null && umount "${m}" 2>/dev/null
	done
	for l in $(losetup -a 2>/dev/null | grep '/ps4hdd/' | cut -d: -f1); do
		losetup -d "${l}" 2>/dev/null
	done
	sync
}

rescueshell() {
	local saved="" tty i
	initramfs_quiet=false
	tty="$(lakka_tty)"
	ps4_change_led red 2>/dev/null || ps4_change_led purple 2>/dev/null
	eerror "Lakka for PS4 could not start (the reason is above)."
	if mountpoint -q /ps4hdd 2>/dev/null; then
		lakka_release_ps4hdd_close
		# A plain file in an existing directory (never mkdir on the PS4 drive).
		if [ -d "${LAKKA_BOOT}" ] && lakka_error_log > "${LAKKA_ERROR_FILE}" 2>/dev/null; then
			sync
			saved=1
		fi
		umount /ps4hdd 2>/dev/null || { sync; ewarn "The PS4 drive could not be unmounted (it was synced)."; }
	fi
	if [ -n "${saved}" ]; then
		lakka_say "The details were saved to /data/linux/boot/lakka-error.txt (read it over FTP)."
	else
		ewarn "The details could not be saved (the PS4 drive is not open)."
	fi
	lakka_say "Take a photo of this screen if you want to report it."
	i=${LAKKA_ERROR_WAIT}
	while [ "${i}" -gt 0 ]; do
		printf '\r  The PS4 restarts into OrbisOS in %2d s ...' "${i}" > "${tty}"
		sleep 1
		i=$((i - 1))
	done
	echo > "${tty}"
	sync
	reboot -f
	while :; do sleep 3600; done
}
