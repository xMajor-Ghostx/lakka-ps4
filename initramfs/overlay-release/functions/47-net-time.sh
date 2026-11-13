#!/bin/sh
# Release build: set the clock over the network (the PS4 RTC reads 1970, which breaks
# TLS for netplay, RetroAchievements and the online updater). DHCP on eth0 then one NTP query,
# in the background so the boot never waits for it. No servers, nothing listening.
ps4_net_time() {
	setsid sh -c '
		ip link set lo up
		i=0
		while [ ! -e /sys/class/net/eth0 ] && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done
		ip link set eth0 up
		udhcpc -i eth0 -s /bin/udhcpc.script -n -q -t 10 -T 3 >/dev/null 2>&1 || exit 0
		ntpd -n -q -p pool.ntp.org -p 216.239.35.0 -p 216.239.35.4 >/dev/null 2>&1
	' </dev/null >/dev/null 2>&1 &
}
