#!/bin/sh
# Running on battery: what tw makes of the power supplies the kernel lists,
# and the command it runs for the state it finds as it starts. The machine is
# made up: tw reads $run_dir/sys in place of /sys (see lib.sh).
#
# Two things cannot be shown here. Only the kernel can announce that a supply
# changed, so a cable going in or out while tw runs is not covered. And the
# headless backend's outputs have no modes, so of the change of refresh rate
# only the choice of mode is tested (tests/layout_test.c).
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

fake_program powerprofilesctl "echo \"\$*\" >>'$profile_log'"

# A machine without a battery is left alone, whatever else is listed.
expect "no power supplies" neither ""
supply AC type=Mains online=1
expect "a desktop" neither ""
supply hidpp_battery_0 type=Battery scope=Device status=Discharging
expect "a desktop with a wireless mouse" neither ""

# A laptop: on mains when its charger is online, on battery when it is not.
supply BAT0 type=Battery status=Charging
supply ADP0 type=Mains online=1
expect "a laptop on its charger" mains "set balanced"
supply BAT0 type=Battery status=Discharging
supply ADP0 type=Mains online=0
expect "a laptop on battery" battery "set power-saver"

# Any supply that is not a battery counts, so a USB-C charger does too. A
# phone charging from the laptop does not: its supply has the scope "Device".
supply BAT0 type=Battery status=Charging
supply ADP0 type=Mains online=0
supply ucsi-source-psy-1 type=USB online=1
expect "a laptop on a USB-C charger" mains "set balanced"
supply BAT0 type=Battery status=Discharging
supply ADP0 type=Mains online=0
supply ucsi-source-psy-1 type=USB online=1 scope=Device
expect "a laptop on battery charging a phone" battery "set power-saver"

# Without the program tw runs nothing and says so.
rm "$bin_dir/powerprofilesctl"
supply BAT0 type=Battery status=Discharging
supply ADP0 type=Mains online=0
start_tw -d
wait_until "tw to say the command is not installed" \
	grep -q 'not installed: powerprofilesctl set power-saver' "$run_dir/tw.log"
stop_tw

echo "ok: no battery, mains, battery, USB-C charger, a device's supply, missing command"
