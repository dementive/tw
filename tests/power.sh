#!/bin/sh
# Running on battery: what tw makes of the power supplies the kernel lists,
# and the command it runs for the state it finds as it starts; and which
# machine tw takes itself for. The machine is made up: tw reads $run_dir/sys
# in place of /sys (see lib.sh).
#
# On battery tw also caps the frames of an output nobody is interacting with
# (battery_idle_fps); that is shown with an animating window.
# $3 = tw-test-keys.
#
# Two things cannot be shown here. Only the kernel can announce that a supply
# changed, so nothing that happens while tw runs is covered: a cable going in
# or out, and the battery running low. What a falling charge calls for is
# tested as a function instead (tests/layout_test.c). And the headless
# backend's outputs have no modes, so of the change of refresh rate only the
# choice of mode is tested, in the same place.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3

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

# --- The frame cap on battery -----------------------------------------------------
# Once nobody has touched anything for battery_idle_after_seconds (5), an
# animating window is shown at most battery_idle_fps (2) frames a second, and
# input brings the full rate back at once. Not on mains, nor while a visible
# window keeps the screen on, nor for a fullscreen window. The headless
# output runs at 60 Hz; the software renderer under the sanitizers manages
# fewer, so "full rate" is anything well above the cap.
frames() { grep -c '^frame$' "$run_dir/$1.log" || true; }
shown() { # <name>: the frames it was shown in the next two seconds
	before=$(frames "$1")
	sleep 2
	echo $(($(frames "$1") - before))
}
uncapped() { # <name> <what it is>
	sleep 6
	n=$(shown "$1")
	[ "$n" -ge 15 ] || fail "$2: an animation got only $n frames in 2 s without input"
}
supply BAT0 type=Battery status=Discharging
supply ADP0 type=Mains online=0

start_tw
start_client spin --animate
sleep 6
n=$(shown spin)
[ "$n" -le 6 ] || fail "on battery without input, an animation got $n frames in 2 s (cap: 2 a second)"
[ "$n" -ge 2 ] || fail "on battery without input, an animation got $n frames in 2 s: it stopped"
press x
n=$(shown spin)
[ "$n" -ge 15 ] || fail "after a key press the animation got only $n frames in 2 s"
stop_tw

start_tw
start_client film --inhibit --animate
uncapped film "on battery, a window keeping the screen on"
stop_tw

start_tw
start_client game --fullscreen --animate
expect_state game "fullscreen=1" "the window to be fullscreen"
uncapped game "on battery, a fullscreen window"
stop_tw

supply ADP0 type=Mains online=1
start_tw
start_client mains --animate
uncapped mains "on mains"
stop_tw

# --- Which machine ---------------------------------------------------------------
# tw takes the settings (src/machines.h) of the machine whose board the
# firmware names, and the ones for every other machine otherwise.
rm -rf "$run_dir/sys"
machine() { # <board name, or "" for none> <the entry tw is to take>
	if [ -n "$1" ]; then
		mkdir -p "$run_dir/sys/class/dmi/id"
		printf '%s\n' "$1" >"$run_dir/sys/class/dmi/id/board_name"
	fi
	start_tw -d
	wait_until "tw to take the settings for $2 on a board named '$1'" \
		grep -q "machine $2\$" "$run_dir/tw.log"
	stop_tw
	rm -rf "$run_dir/sys"
}
machine G533ZW G533ZW
machine G615LM G615LM
machine "G615LM-X" other
machine "" other

echo "ok: no battery, mains, battery, USB-C charger, a device's supply, odd supplies, low at startup, not the session, failing, running and missing command, the frame cap, the machine by its board"
