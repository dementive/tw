#!/bin/sh
# tw-power and its root helper, run against a made-up machine: a directory
# standing in for /sys with the three settings a power profile is made of,
# and a stand-in for systemctl that runs the helper the way the unit does.
# No setting of the real machine is read or written. What it cannot show:
# that the real kernel takes these values, what they do to the fans and the
# draw, or that the polkit rule lets the units be started.
# $1 = the data/ directory of the source tree.
set -eu
data=$1
tmp=$(mktemp -d "$PWD/profile-test.XXXXXX")
trap 'chmod -R u+w "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT
sys=$tmp/sys
bin=$tmp/bin
log=$tmp/log
journal=$tmp/journal # what the root helper says, which on a real machine is logged
mkdir -p "$bin"

fail() {
	echo "FAIL: $*" >&2
	[ -s "$log" ] && sed 's/^/  log: /' "$log" >&2
	[ -s "$journal" ] && sed 's/^/  journal: /' "$journal" >&2
	[ -s "$tmp/out" ] && sed 's/^/  out: /' "$tmp/out" >&2
	exit 1
}
said() { grep -q "$1" "$tmp/out"; }
logged() { grep -q "$1" "$journal" 2>/dev/null; }

# What tw-power reaches the root helper through: the unit, which runs the
# helper with the word from its name.
cat >"$bin/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >>"$log"
case "\$*" in
"start tw-power@"*".service") word=\${2#tw-power@}; word=\${word%.service} ;;
*) exit 0 ;;
esac
[ -e "$tmp/denied" ] && exit 1
TW_SYS="$sys" TW_RUN="$tmp" "$data/tw-power-root" "\$word" 2>>"$journal"
EOF
chmod +x "$bin/systemctl"

# --- The made-up machine ------------------------------------------------------
platform=$sys/firmware/acpi
cpus=$sys/devices/system/cpu

machine() { # a laptop with four CPUs, on the balanced profile
	chmod -R u+w "$sys" 2>/dev/null || true
	rm -rf "$sys" "$journal" "$tmp/out" "$tmp/denied"
	: >"$log"
	mkdir -p "$platform" "$cpus/cpufreq" "$cpus/cpuidle"
	echo "quiet balanced performance" >"$platform/platform_profile_choices"
	echo balanced >"$platform/platform_profile"
	for n in 0 1 2 3; do
		mkdir -p "$cpus/cpufreq/policy$n" "$cpus/cpu$n/power"
		echo "default performance balance_performance balance_power power" \
			>"$cpus/cpufreq/policy$n/energy_performance_available_preferences"
		echo balance_performance >"$cpus/cpufreq/policy$n/energy_performance_preference"
		echo 6 >"$cpus/cpu$n/power/energy_perf_bias"
	done
}
no_platform() { rm -rf "$sys/firmware"; }
no_preference() { rm -f "$cpus"/cpufreq/policy*/energy_performance_*; }
no_bias() { rm -rf "$cpus"/cpu[0-9]*/power; }

# The three settings as they stand: the platform's, the CPUs' and the hint,
# each once if all CPUs agree.
settings() {
	echo "$(cat "$platform/platform_profile" 2>/dev/null)" \
		"$(cat "$cpus"/cpufreq/policy*/energy_performance_preference 2>/dev/null | sort -u | tr '\n' ',')" \
		"$(cat "$cpus"/cpu[0-9]*/power/energy_perf_bias 2>/dev/null | sort -u | tr '\n' ',')"
}
expect_settings() { # <description> <settings>
	[ "$(settings)" = "$2" ] || fail "$1: the settings are '$(settings)', not '$2'"
}

# tw-power as tw or the person at the computer runs it; what it prints is kept.
power() {
	PATH="$bin:$PATH" TW_SYS="$sys" TW_RUN="$tmp" "$data/tw-power" "$@" >"$tmp/out" 2>&1
}

# --- The three profiles -------------------------------------------------------
machine
power || fail "status failed"
said '^Power profile: balanced (platform: balanced, CPUs: balance_performance)$' || fail "status on the balanced profile"
power saver || fail "saver failed"
expect_settings "saver" "quiet power, 15,"
[ ! -s "$tmp/out" ] || fail "setting a profile printed something"
power
said '^Power profile: saver (platform: quiet, CPUs: power)$' || fail "status on the saver profile"
power performance || fail "performance failed"
expect_settings "performance" "performance performance, 0,"
power
said '^Power profile: performance (platform: performance, CPUs: performance)$' || fail "status on the performance profile"
power balanced || fail "balanced failed"
expect_settings "balanced" "balanced balance_performance, 6,"
[ "$(grep -c 'start tw-power@' "$log")" = 3 ] || fail "each profile did not start its unit once"

# --- Machines that differ -----------------------------------------------------
# One left on the performance governor, which refuses any other preference:
# the profile moves it to powersave first.
machine
for policy in "$cpus"/cpufreq/policy*; do
	echo performance >"$policy/scaling_governor"
	echo 'performance powersave' >"$policy/scaling_available_governors"
done
power saver || fail "saver failed on the performance governor"
expect_settings "saver on the performance governor" "quiet power, 15,"
[ "$(cat "$cpus"/cpufreq/policy*/scaling_governor | sort -u)" = powersave ] ||
	fail "the performance governor was left in place"
# One with no preference to weigh (acpi-cpufreq, or a driver in passive
# mode) keeps the performance governor: powersave would hold every CPU at
# its slowest.
machine
no_preference
for policy in "$cpus"/cpufreq/policy*; do
	echo performance >"$policy/scaling_governor"
	echo 'performance powersave' >"$policy/scaling_available_governors"
done
power balanced || fail "balanced failed without preferences"
[ "$(cat "$cpus"/cpufreq/policy*/scaling_governor | sort -u)" = performance ] ||
	fail "the governor went to powersave on CPUs without a preference"

# One whose most frugal profile is called low-power gets that one.
machine
echo "low-power quiet balanced performance" >"$platform/platform_profile_choices"
power saver || fail "saver failed on a machine with low-power"
expect_settings "saver with low-power on offer" "low-power power, 15,"
# One whose platform has nothing frugal keeps its profile; the CPUs are set.
machine
echo "balanced performance" >"$platform/platform_profile_choices"
power saver || fail "saver failed on a platform without a frugal profile"
expect_settings "saver without a frugal platform profile" "balanced power, 15,"
# CPUs that do not offer a preference keep theirs.
machine
for n in 0 1 2 3; do
	echo "default performance balance_performance" \
		>"$cpus/cpufreq/policy$n/energy_performance_available_preferences"
done
power saver || fail "saver failed with CPUs that do not offer 'power'"
expect_settings "saver with CPUs that do not offer it" "quiet balance_performance, 15,"
# No platform profile: the CPUs alone.
machine
no_platform
power saver || fail "saver failed without a platform profile"
expect_settings "saver without a platform profile" " power, 15,"
power
said '^Power profile: saver (platform: none, CPUs: power)$' || fail "status without a platform profile"
# No preference for the CPUs: the platform and the hint, and status goes by
# the platform.
machine
no_preference
power saver || fail "saver failed without a preference for the CPUs"
expect_settings "saver without a preference for the CPUs" "quiet  15,"
power
said '^Power profile: saver (platform: quiet, CPUs: none)$' || fail "status without a preference for the CPUs"
# Nothing to set at all: nothing happens, and status says so.
machine
no_platform
no_preference
no_bias
power saver || fail "saver failed on a machine with nothing to set"
power
said '^Power profile: this machine has none to set$' || fail "status on a machine with nothing to set"
# CPUs that do not agree, as after somebody set one by hand.
machine
echo power >"$cpus/cpufreq/policy2/energy_performance_preference"
power
said "^Power profile: none of tw's (platform: balanced, CPUs: balance_performance power)$" ||
	fail "status with CPUs that do not agree"

# --- Nothing is written that is there already ---------------------------------
# With every file read-only, the profile that is set goes through, and
# another one fails and says where. (Root may write anyway, so not as root.)
if [ "$(id -u)" != 0 ]; then
	machine
	chmod a-w "$platform/platform_profile" "$cpus"/cpufreq/policy*/energy_performance_preference \
		"$cpus"/cpu[0-9]*/power/energy_perf_bias
	power balanced || fail "the profile that is set already was written again"
	power saver && fail "saver reported success although nothing could be written"
	said 'could not set the profile' || fail "a failed profile does not say so"
	logged "could not write 'quiet' to $platform/platform_profile" || fail "the helper did not say what it could not write"
	logged "could not write 'power' to $cpus/cpufreq/policy3/energy_performance_preference" ||
		fail "the helper stopped at the first setting it could not write"
	expect_settings "a profile that could not be written" "balanced balance_performance, 6,"
fi

# --- One at a time -----------------------------------------------------------
# A profile set by hand as tw sets another: one waits until the other is
# done, so neither is left half written.
machine
(flock "$tmp/tw-power.lock" sh -c ": >'$tmp/locked'; sleep 0.6; rm '$tmp/locked'" &)
tries=0
until [ -e "$tmp/locked" ]; do
	tries=$((tries + 1))
	[ "$tries" -le 100 ] || fail "timed out waiting for the lock to be taken"
	sleep 0.05
done
power saver || fail "saver failed after waiting for another profile"
[ ! -e "$tmp/locked" ] || fail "the helper set a profile while another was being set"
expect_settings "saver after waiting" "quiet power, 15,"

# --- Refused, and asked the wrong thing ---------------------------------------
machine
: >"$tmp/denied"
power saver && fail "saver reported success although the unit was refused"
said 'could not set the profile' || fail "a refused profile does not say so"
expect_settings "a refused profile" "balanced balance_performance, 6,"
rm "$tmp/denied"
status=0
power frugal || status=$?
[ "$status" = 2 ] || fail "an unknown profile exits with $status"
said 'tw-power saver ' && said 'tw-power performance ' || fail "an unknown profile does not print the usage"
status=0
TW_SYS="$sys" TW_RUN="$tmp" "$data/tw-power-root" frugal 2>/dev/null || status=$?
[ "$status" = 2 ] || fail "the helper exits with $status on an unknown word"
expect_settings "an unknown profile" "balanced balance_performance, 6,"

# --- Keyboards and mice, while the screen is off -------------------------------
usb=$sys/bus/usb/devices
# device <name> <removable> <bmAttributes> <control> <interfaces...>: a USB
# device as the kernel lists it, each interface as class, subclass and
# protocol in one (030101 is a boot keyboard).
device() {
	name=$1 removable=$2 attributes=$3 control=$4
	shift 4
	mkdir -p "$usb/$name/power"
	echo "$removable" >"$usb/$name/removable"
	echo "$attributes" >"$usb/$name/bmAttributes"
	echo "$control" >"$usb/$name/power/control"
	n=0
	for interface in "$@"; do
		mkdir -p "$usb/$name/$name:1.$n"
		echo "$interface" | cut -c1-2 >"$usb/$name/$name:1.$n/bInterfaceClass"
		echo "$interface" | cut -c3-4 >"$usb/$name/$name:1.$n/bInterfaceSubClass"
		echo "$interface" | cut -c5-6 >"$usb/$name/$name:1.$n/bInterfaceProtocol"
		n=$((n + 1))
	done
}
control() { cat "$usb/$1/power/control"; } # <name>
desk() { # the G615LM's devices on a desk: what may sleep and what may not
	machine
	rm -f "$tmp/tw-power-away"
	device 3-7 removable a0 on 030101 030000 030000 # a K70: keyboard
	device 3-6 removable a0 on 030102 030000        # a G502: mouse
	device 3-4 fixed a0 auto 030101 030102           # the laptop's own keyboard
	device 3-5 fixed 80 on 0e0100 0e0200            # its webcam
	device 2-1 removable 80 on 080662               # the USB system disk
	device 3-9 removable 80 on 030101               # a keyboard that cannot wake the machine
	device 3-8 removable a0 on 030000 010100        # a headset's volume buttons
	device 3-10 removable a0 auto 030102            # a mouse somebody let sleep by hand
}
desk
power away || fail "away failed"
[ "$(control 3-7) $(control 3-6)" = "auto auto" ] || fail "away did not let the keyboard and mouse sleep"
[ "$(control 3-4) $(control 3-5) $(control 2-1) $(control 3-9) $(control 3-8)" = "auto on on on on" ] ||
	fail "away touched a device it should leave alone"
[ ! -s "$tmp/out" ] || fail "away printed something"
power back || fail "back failed"
[ "$(control 3-7) $(control 3-6)" = "on on" ] || fail "back did not wake the keyboard and mouse"
[ "$(control 3-10)" = auto ] || fail "back woke a mouse that away had not let sleep"
[ ! -e "$tmp/tw-power-away" ] || fail "back left the list of what away let sleep"
# With nothing asleep, back starts no unit, as at each screen-on after it.
: >"$log"
power back || fail "back failed with nothing asleep"
! grep -q 'start tw-power@' "$log" || fail "back started its unit with nothing asleep"
# With nothing to let sleep, away leaves no list, so back starts nothing.
machine
rm -f "$tmp/tw-power-away"
device 3-4 fixed a0 auto 030101
power away || fail "away failed with nothing plugged in"
[ ! -e "$tmp/tw-power-away" ] || fail "away left a list with nothing let sleep"
# A device plugged out while asleep is gone at back; the rest still wake.
desk
power away
rm -rf "${usb:?}/3-6"
power back || fail "back failed with a device gone"
[ "$(control 3-7)" = on ] || fail "back stopped at a device that is gone"
# Refused: says so.
desk
: >"$tmp/denied"
power away && fail "away reported success although the unit was refused"
said 'could not let the keyboards and mice sleep' || fail "a refused away does not say so"
rm "$tmp/denied"

# --- The files that go with them ----------------------------------------------
# These name each other, and nothing else checks that they agree.
for word in saver balanced performance away back; do
	grep -q "\"tw-power@$word.service\"" "$data/tw-power.rules" ||
		fail "the polkit rule does not allow the unit for '$word'"
	grep -q "^$word)" "$data/tw-power-root" || fail "the helper does not know '$word'"
done
grep -q 'systemctl start "tw-power@$1.service"' "$data/tw-power" ||
	fail "tw-power no longer starts the units the polkit rule allows"
grep -q '^static const char battery_command\[\] = "tw-power saver";$' "$data/../src/config.h" &&
	grep -q '^static const char mains_command\[\] = "tw-power balanced";$' "$data/../src/config.h" ||
	fail "config.h no longer has tw set its profiles with tw-power"
grep -q '^static const char screen_off_command\[\] = "tw-power away";$' "$data/../src/config.h" &&
	grep -q '^static const char screen_on_command\[\] = "tw-power back";$' "$data/../src/config.h" ||
	fail "config.h no longer has tw run tw-power away and back with the screen"

echo "ok: the three profiles, machines that lack a setting, nothing written twice, one at a time, refusals, keyboards and mice asleep and awake"
