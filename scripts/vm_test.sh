#!/bin/sh
# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0
#
# Run a native_sim executable against dummy_hcd in a KVM virtual machine that
# boots the kernel of this host, so that neither root access nor a module on
# the host is needed.
#
# usage: vm_test.sh <zephyr.exe> [guest test script]
#
# VM_TEST_ARGS holds the arguments of the executable, --rt-period=1e-3 when
# not set. VM_TEST_DUMMY_HCD holds the parameters of the dummy_hcd module,
# for example is_high_speed=0 for a full speed USB host.

set -eu

if [ $# -lt 1 ]; then
	sed -n '5,14s/^# \{0,1\}//p' "$0"
	exit 2
fi

HERE=$(dirname "$(readlink -f "$0")")
APP=$(readlink -f "$1")
TEST=$(readlink -f "${2:-$HERE/vm_test_echo.sh}")
KVER=$(uname -r)
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/zephyr-native-sim-usb/$KVER
PATH=$PATH:/usr/sbin:/sbin

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ROOT=$WORK/root

mkdir -p "$ROOT/bin" "$ROOT/lib/modules" "$ROOT/proc" "$ROOT/sys" "$ROOT/dev" "$ROOT/tmp" \
	"$ROOT/app"

# Add a module of this kernel and the ones it needs, in the order to load them
add_module() {
	modprobe --show-depends "$1" | while read -r kind path _; do
		[ "$kind" = insmod ] || continue
		name=$(basename "$path")
		name=${name%%.ko*}
		[ -f "$ROOT/lib/modules/$name.ko" ] && continue
		case "$path" in
		*.xz) xz -dc "$path" ;;
		*.zst) zstd -dc "$path" ;;
		*.gz) gzip -dc "$path" ;;
		*) cat "$path" ;;
		esac > "$ROOT/lib/modules/$name.ko"
		echo "$name" >> "$ROOT/lib/modules/order"
	done
}

add_module dummy_hcd
add_module cdc_acm
add_module usb_storage

if modprobe --show-depends raw_gadget > /dev/null 2>&1; then
	add_module raw_gadget
else
	# Not every distribution ships raw_gadget: build it for this kernel
	if [ ! -f "$CACHE/raw_gadget.ko" ]; then
		mkdir -p "$CACHE"
		version=$(echo "$KVER" | sed 's/[^0-9.].*//')
		for tag in "v$version" "v${version%.*}"; do
			curl -sSf -o "$CACHE/raw_gadget.c" \
				"https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/usb/gadget/legacy/raw_gadget.c?h=$tag" &&
				break
		done
		echo 'obj-m := raw_gadget.o' > "$CACHE/Makefile"
		if ! make -C "/lib/modules/$KVER/build" M="$CACHE" modules > "$CACHE/build.log" 2>&1; then
			echo "Failed to build raw_gadget, see $CACHE/build.log" >&2
			exit 1
		fi
	fi

	cp "$CACHE/raw_gadget.ko" "$ROOT/lib/modules/"
	echo raw_gadget >> "$ROOT/lib/modules/order"
fi

# The executable with its libraries, and the one that glibc loads to end a thread
cp "$APP" "$ROOT/app/zephyr.exe"
for lib in $(ldd "$APP" | awk '$2 == "=>" && $3 ~ /^\// {print $3} $1 ~ /^\// {print $1}') \
	$(cc -print-file-name=libgcc_s.so.1); do
	mkdir -p "$ROOT$(dirname "$lib")"
	cp -L "$lib" "$ROOT$lib"
done

cp "$(command -v busybox)" "$ROOT/bin/busybox"
cc -o "$ROOT/bin/vm_port_power" "$HERE/vm_port_power.c"
cp "$TEST" "$ROOT/test.sh"
echo "${VM_TEST_ARGS---rt-period=1e-3}" > "$ROOT/app/args"
echo "${VM_TEST_DUMMY_HCD-}" > "$ROOT/lib/modules/dummy_hcd.args"

cat > "$ROOT/init" <<'EOI'
#!/bin/busybox sh
/bin/busybox --install -s /bin
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /tmp
while read -r module; do
	# shellcheck disable=SC2046
	insmod "/lib/modules/$module.ko" $(cat "/lib/modules/$module.args" 2> /dev/null) ||
		echo "RESULT insmod $module: FAIL"
done < /lib/modules/order
echo "== guest: controller $(ls /sys/class/udc)"
sh /test.sh
echo "== guest: done"
poweroff -f
EOI
chmod +x "$ROOT/init"

(cd "$ROOT" && find . | cpio -o -H newc --quiet | gzip -1) > "$WORK/initrd.gz"

timeout "${VM_TEST_TIMEOUT:-300}" qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1024 \
	-display none -monitor none -serial "file:$WORK/console.log" -no-reboot \
	-kernel "/boot/vmlinuz-$KVER" -initrd "$WORK/initrd.gz" \
	-append "console=ttyS0 rdinit=/init panic=-1 loglevel=4" || true

tr -d '\r' < "$WORK/console.log" | sed 's/\x1b\[[0-9;]*m//g' | sed -n '/^== guest: controller/,$p'

grep -q '^== guest: done' "$WORK/console.log" || { echo "The guest did not finish" >&2; exit 1; }
grep -q '^RESULT ' "$WORK/console.log" || { echo "The guest ran no test" >&2; exit 1; }
! grep -q '^RESULT .*: FAIL' "$WORK/console.log"
