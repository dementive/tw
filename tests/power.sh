#!/bin/sh
# Running on battery: what tw makes of the power supplies the kernel lists,
# and the command it runs for the state it finds as it starts. The machine is
# made up: tw reads $run_dir/sys in place of /sys (see lib.sh).
#
# Two things cannot be shown here. Only the kernel can announce that a supply
# changed, so nothing that happens while tw runs is covered: a cable going in
# or out, and the battery running low. What a falling charge calls for is
# tested as a function instead (tests/layout_test.c). And the headless
# backend's outputs have no modes, so of the change of refresh rate only the
# choice of mode is tested, in the same place.
set -eu
. "$(dirname "$0")/lib.sh"

supplies=$run_dir/sys/class/power_supply

# supply <name> <attribute>=<value>...: as the kernel would list it.
supply() {
	name=$1
	shift
	mkdir -p "$supplies/$name"
	for pair in "$@"; do
		printf '%s\n' "${pair#*=}" >"$supplies/$name/${pair%%=*}"
	done
}

profile_log=$run_dir/profile.log
ran() { [ "$(cat "$profile_log" 2>/dev/null)" = "$1" ]; }

# expect <what the machine is> <"battery", "mains" or "neither"> <command>:
# starts tw on the supplies set up since the last call, then clears them.
expect() {
	start_tw -d
	if [ -n "$3" ]; then
		wait_until "$1: the command '$3'" ran "$3"
		grep -q "running on $2\$" "$run_dir/tw.log" || fail "$1: tw did not say it runs on $2"
	fi
	# Once, and nothing at all where nothing is expected.
	sleep 0.3
	ran "$3" || fail "$1: expected '$3', ran: $(cat "$profile_log" 2>/dev/null)"
	if [ "$2" = neither ] && grep -q 'running on \(battery\|mains\)$' "$run_dir/tw.log"; then
		fail "$1: tw acted on a power supply"
	fi
	stop_tw
	rm -rf "$run_dir/sys" "$profile_log"
}

fake_program tw-power "echo \"\$*\" >>'$profile_log'"

# A machine without a battery is left alone, whatever else is listed.
expect "no power supplies" neither ""
supply AC type=Mains online=1
expect "a desktop" neither ""
supply hidpp_battery_0 type=Battery scope=Device status=Discharging
expect "a desktop with a wireless mouse" neither ""

# A laptop: on mains when its charger is online, on battery when it is not.
supply BAT0 type=Battery status=Charging
supply ADP0 type=Mains online=1
expect "a laptop on its charger" mains "balanced"
supply BAT0 type=Battery status=Discharging
supply ADP0 type=Mains online=0
expect "a laptop on battery" battery "saver"

# Any supply that is not a battery counts, so a USB-C charger does too. A
# phone charging from the laptop does not: its supply has the scope "Device".
supply BAT0 type=Battery status=Charging
supply ADP0 type=Mains online=0
supply ucsi-source-psy-1 type=USB online=1
expect "a laptop on a USB-C charger" mains "balanced"
supply BAT0 type=Battery status=Discharging
supply ADP0 type=Mains online=0
supply ucsi-source-psy-1 type=USB online=1 scope=Device
expect "a laptop on battery charging a phone" battery "saver"
# A USB-C port that charges at a programmable voltage (PPS) reads 2, and the
# kernel counts any value but 0 as online.
supply BAT0 type=Battery status=Charging
supply ADP0 type=Mains online=0
supply tcpm-source-psy-1 type=USB online=2
expect "a laptop on a USB-C PPS charger" mains "balanced"

# What the kernel lists varies by machine. As in the kernel's own rule, a
# supply without a type is passed over, and so is a charger without "online".
supply BAT0 type=Battery status=Discharging
supply ADP0 type=Mains
supply odd online=1
expect "a laptop with odd supplies" battery "saver"

# A battery that is already low as tw starts is left for its next
# announcement. So tw does not warn before the session is up to show it, and
# does not suspend a machine that somebody has just logged in to.
fake_program systemctl "echo \"systemctl \$*\" >>'$run_dir/low.log'"
fake_program notify-send "echo \"notify-send \$*\" >>'$run_dir/low.log'"
supply BAT0 type=Battery status=Discharging capacity=3
supply ADP0 type=Mains online=0
expect "a laptop on battery at 3%" battery "saver"
[ ! -e "$run_dir/low.log" ] ||
	fail "tw acted on a low battery as it started: $(cat "$run_dir/low.log")"

# A tw that does not run the machine leaves its power alone: one inside
# another session or, as here, one without a screen that no test has pointed
# at a made-up machine. On a laptop this tw could see the real battery.
env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DISPLAY -u TW_SYS \
	PATH="$bin_dir" XDG_RUNTIME_DIR="$run_dir" WLR_BACKENDS=headless WLR_RENDERER=pixman \
	"$tw_bin" -d >"$run_dir/tw.log" 2>&1 &
tw_pid=$!
wait_until "the Wayland socket" test -S "$run_dir/wayland-0"
sleep 0.3
if [ -e "$profile_log" ] || grep -q 'running on \(battery\|mains\)$' "$run_dir/tw.log"; then
	fail "a tw that does not run the machine acted on its power supplies"
fi
stop_tw

supply BAT0 type=Battery status=Discharging
supply ADP0 type=Mains online=0

# A command that fails is reported.
fake_program tw-power "exit 1"
start_tw -d
wait_until "tw to say the command failed" \
	grep -q 'the power profile command for battery failed' "$run_dir/tw.log"
stop_tw

# One still running as tw stops is left to finish, and tw stops cleanly.
fake_program tw-power "echo \$\$ >'$run_dir/profile.pid'; exec /bin/sleep 10"
start_tw -d
wait_until "the command to start" test -s "$run_dir/profile.pid"
stop_tw
kill "$(cat "$run_dir/profile.pid")" 2>/dev/null || true

# Without the program tw runs nothing and says so.
rm "$bin_dir/tw-power"
start_tw -d
wait_until "tw to say the command is not installed" \
	grep -q 'not installed: tw-power saver' "$run_dir/tw.log"
stop_tw

echo "ok: no battery, mains, battery, USB-C charger, a device's supply, odd supplies, low at startup, not the session, failing, running and missing command"
