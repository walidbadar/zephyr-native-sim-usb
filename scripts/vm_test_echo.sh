#!/bin/sh
# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0
#
# Runs in the guest of vm_test.sh: the USB host stack of Linux enumerates the
# CDC ACM device of the echo application and compares what comes back.

TTY=/dev/ttyACM0

centiseconds() {
	cut -d' ' -f1 /proc/uptime | tr -d .
}

# $1: 1 to wait for the serial port, 0 to wait for it to go
wait_tty() {
	i=0
	while [ $i -lt 150 ]; do
		if [ -e $TTY ]; then
			[ "$1" = 1 ] && return 0
		else
			[ "$1" = 0 ] && return 0
		fi
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

# $1: name of the test, $2: KiB to send
echo_test() {
	stty -F $TTY raw -echo 115200
	exec 3<> $TTY
	: > /tmp/rx
	cat <&3 > /tmp/rx &
	reader=$!
	sleep 1

	dd if=/dev/urandom of=/tmp/tx bs=1024 count="$2" 2> /dev/null
	size=$(wc -c < /tmp/tx)
	start=$(centiseconds)
	cat /tmp/tx >&3

	i=0
	while [ "$(wc -c < /tmp/rx)" -lt "$size" ] && [ $i -lt 600 ]; do
		sleep 0.05
		i=$((i + 1))
	done

	if cmp -s /tmp/tx /tmp/rx; then
		echo "RESULT $1: PASS ($size bytes in $(( ($(centiseconds) - start) * 10 )) ms)"
	else
		echo "RESULT $1: FAIL ($(wc -c < /tmp/rx) of $size bytes)"
	fi

	kill $reader 2> /dev/null
	wait $reader 2> /dev/null
	exec 3>&-
}

# shellcheck disable=SC2046
/app/zephyr.exe $(cat /app/args) > /tmp/zephyr.log 2>&1 &
app=$!

if wait_tty 1; then
	echo "RESULT enumeration: PASS ($(cat /sys/bus/usb/devices/1-1/product), speed $(cat /sys/bus/usb/devices/1-1/speed))"
	echo_test echo_1k 1
	echo_test echo_256k 256

	# The port of the device loses its power and gets it back, as with a cable
	# that is pulled and plugged again
	suspends=$(grep -c 'Device suspended' /tmp/zephyr.log)
	devnum=$(cat /sys/bus/usb/devices/1-1/devnum)
	vm_port_power /dev/bus/usb/001/001 1 0
	sleep 0.5
	vm_port_power /dev/bus/usb/001/001 1 1

	i=0
	while [ $i -lt 150 ]; do
		[ -e $TTY ] && [ "$(cat /sys/bus/usb/devices/1-1/devnum 2> /dev/null)" != "$devnum" ] &&
			break
		sleep 0.1
		i=$((i + 1))
	done

	if [ "$(grep -c 'Device suspended' /tmp/zephyr.log)" -gt "$suspends" ]; then
		echo "RESULT disconnect_reported: PASS"
	else
		echo "RESULT disconnect_reported: FAIL"
	fi

	if [ $i -lt 150 ]; then
		echo "RESULT reconnect: PASS (speed $(cat /sys/bus/usb/devices/1-1/speed))"
		echo_test echo_after_reconnect 4
	else
		echo "RESULT reconnect: FAIL"
	fi

	kill -TERM $app
	if wait_tty 0; then
		echo "RESULT disconnect_on_exit: PASS"
	else
		echo "RESULT disconnect_on_exit: FAIL"
	fi
else
	echo "RESULT enumeration: FAIL"
fi

echo "== guest: application log"
sed 's/\x1b\[[0-9;]*m//g' /tmp/zephyr.log | head -${LOG_LINES:-40}
echo "== guest: kernel log"
dmesg | grep -i -E 'usb 1-1|gadget|dummy' | tail -${LOG_LINES:-10}
