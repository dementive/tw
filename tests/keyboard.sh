#!/bin/sh
# The udev rule that lets a laptop's built-in keyboard sleep between keys.
# A test can have udev read it and check its conditions; whether a real
# keyboard sleeps with it, and wakes without losing the key, is seen only on
# a real machine.
# $1 = the data/ directory of the source tree.
set -eu
rules=$1/tw-keyboard.rules

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# The rule as one line, comments and the line break taken out.
rule=$(grep -v '^#' "$rules" | tr -d '\\\n' | tr -s ' \t' ' ')

# Only a built-in device, only a keyboard, and only where the kernel offers
# the switch. Without the first two it would put to sleep a keyboard or a
# mouse plugged in, which may not wake on its own.
for condition in 'SUBSYSTEM=="usb"' 'ENV{DEVTYPE}=="usb_device"' 'ATTR{removable}=="fixed"' \
	'ENV{ID_USB_INTERFACES}=="*:030101:*"' 'TEST=="power/control"'; do
	case $rule in
	*"$condition"*) ;;
	*) fail "the rule lost the condition $condition: $rule" ;;
	esac
done
case $rule in
*'ATTR{power/control}="auto"') ;;
*) fail "the rule no longer lets the keyboard sleep: $rule" ;;
esac

if udevadm verify --help >/dev/null 2>&1; then
	udevadm verify --no-style "$rules" >/dev/null 2>&1 ||
		fail "udev does not accept the rule: $(udevadm verify --no-style "$rules" 2>&1)"
fi

echo "ok: the keyboard rule touches only a built-in keyboard and udev accepts it"
