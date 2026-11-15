#!/bin/sh
# The udev rule that lets Thunderbolt devices in. All a test can do with it
# is have udev read it and see that it still says what the kernel's guide
# says: nothing here has a Thunderbolt port, let alone a device on it. What
# it cannot show: that a real dock comes up with the rule and without boltd.
# $1 = the data/ directory of the source tree.
set -eu
rules=$1/tw-thunderbolt.rules

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# The rule as one line, comments and the line break taken out.
rule=$(grep -v '^#' "$rules" | tr -d '\\\n' | tr -s ' ')
[ "$rule" = 'ACTION=="add", SUBSYSTEM=="thunderbolt", ATTRS{iommu_dma_protection}=="1", ATTR{authorized}=="0", ATTR{authorized}="1"' ] ||
	fail "the rule is no longer the kernel guide's: $rule"

# It lets a device in only where the kernel protects its memory, and only a
# device that is waiting: without either condition it would let in what the
# machine's owner or its firmware keeps out.
case $rule in
*'ATTRS{iommu_dma_protection}=="1"'*'ATTR{authorized}=="0"'*) ;;
*) fail "the rule lost a condition" ;;
esac

if udevadm verify --help >/dev/null 2>&1; then
	udevadm verify --no-style "$rules" >/dev/null 2>&1 ||
		fail "udev does not accept the rule: $(udevadm verify --no-style "$rules" 2>&1)"
fi

echo "ok: the Thunderbolt rule is the kernel guide's and udev accepts it"
