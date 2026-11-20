#!/bin/sh
# tw-control against stand-ins: wpctl, brightnessctl, tw-power, tw-bluetooth,
# systemctl, swaylock and tw-monitors keep their state in files and write
# down how they were run, and the battery is a made-up one. What each row
# shows, Left, Right and Return on each, the session actions and the second
# Return they ask for, one box at a time, and logging out.
#
# No system bus is given, so Wi-Fi is "not available" and BlueZ is never
# asked; iwd and BlueZ themselves are not covered (docs/testing.md).
#
# $3 = tw-test-keys, $4 = tw-control.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3
control_bin=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")

state=$run_dir/state
ran=$run_dir/ran
sys=$run_dir/control-sys
mkdir -p "$state" "$sys/class/bluetooth" "$sys/class/power_supply/BAT0"
for pair in type=Battery capacity=74 status=Discharging energy_now=40000000 power_now=10000000; do
	echo "${pair#*=}" >"$sys/class/power_supply/BAT0/${pair%%=*}"
done
echo 40 >"$state/volume"
echo 0 >"$state/muted"
echo 0 >"$state/mic"
echo 48 >"$state/output"
echo 500 >"$state/light"
echo 1 >"$state/keys"
echo balanced >"$state/profile"

# stand_in <name> <script>: a program for tw-control's PATH, with $state,
# $ran and $sys set and every run written down.
stand_in() {
	{
		echo '#!/bin/sh'
		echo "state='$state' ran='$ran' sys='$sys'"
		echo 'echo "${0##*/}${*:+ $*}" >>"$ran"'
		cat
	} >"$bin_dir/$1"
	chmod +x "$bin_dir/$1"
}
stand_in wpctl <<'EOF'
read volume <"$state/volume"; read muted <"$state/muted"; read mic <"$state/mic"
read output <"$state/output"
case $1 in
get-volume)
	if [ "$2" = @DEFAULT_AUDIO_SOURCE@ ]; then
		[ "$mic" = 1 ] && echo 'Volume: 1.00 [MUTED]' || echo 'Volume: 1.00'
	else
		printf 'Volume: %d.%02d%s\n' $((volume / 100)) $((volume % 100)) \
			"$([ "$muted" = 1 ] && echo ' [MUTED]')"
	fi ;;
set-volume)
	step=${5%%%*}
	case $5 in *+) volume=$((volume + step)) ;; *) volume=$((volume - step)) ;; esac
	[ $volume -gt 100 ] && volume=100; [ $volume -lt 0 ] && volume=0
	echo $volume >"$state/volume" ;;
set-mute)
	if [ "$2" = @DEFAULT_AUDIO_SOURCE@ ]; then echo $((1 - mic)) >"$state/mic"
	else echo $((1 - muted)) >"$state/muted"; fi ;;
set-default) echo "$2" >"$state/output" ;;
status)
	echo 'PipeWire'
	echo ''
	echo 'Audio'
	echo ' ├─ Devices:'
	echo ' │      42. Built-in Audio                      [alsa]'
	echo ' │  '
	echo ' ├─ Sinks:'
	echo " │  $([ "$output" = 48 ] && echo '*' || echo ' ')   48. Speakers                    [vol: 0.40]"
	echo " │  $([ "$output" = 67 ] && echo '*' || echo ' ')   67. Headphones                  [vol: 1.00]"
	echo ' │  '
	echo ' ├─ Sources:'
	echo ' │  *   50. Microphone                  [vol: 1.00]'
	echo ''
	echo 'Video' ;;
esac
EOF
stand_in brightnessctl <<'EOF'
read light <"$state/light"; read keys <"$state/keys"
case "$*" in
*kbd_backlight*info) echo "asus::kbd_backlight,leds,$keys,$((keys * 33))%,3" ;;
*kbd_backlight*set\ 1+) [ $keys -lt 3 ] && echo $((keys + 1)) >"$state/keys" ;;
*kbd_backlight*set\ 1-) [ $keys -gt 0 ] && echo $((keys - 1)) >"$state/keys" ;;
*info) echo "intel_backlight,backlight,$light,$((light / 10))%,1000" ;;
*set\ 5%+) echo $((light + 50)) >"$state/light" ;;
*set\ 5%-) echo $((light - 50)) >"$state/light" ;;
*set\ *%) value=${4%\%}; echo $((value * 10)) >"$state/light" ;;
esac
EOF
stand_in tw-power <<'EOF'
read profile <"$state/profile"
[ $# = 0 ] && echo "Power profile: $profile (platform: quiet, CPUs: power)" || echo "$1" >"$state/profile"
EOF
stand_in tw-bluetooth <<'EOF'
[ "$1" = on ] && : >"$sys/class/bluetooth/hci0"
EOF
for program in systemctl swaylock tw-monitors; do
	stand_in "$program" </dev/null
done

# control <name>: the box, saying what it does in $run_dir/<name>.log.
control() {
	env XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 PATH="$bin_dir" TW_SYS="$sys" \
		DBUS_SYSTEM_BUS_ADDRESS="unix:path=$run_dir/no-bus" \
		"$control_bin" -d 2>"$run_dir/$1.log" &
	box=$!
	client_pids="$client_pids $box"
	wait_until "the box to have the keyboard" grep -qx keyboard "$run_dir/$1.log"
}
# row <name> <line>: the box shows that row now, as its last change said.
row() { grep '^row ' "$run_dir/$1.log" | grep "^row ${2%%:*}:" | tail -n 1 | grep -qxF "row $2"; }
expect_row() { wait_until "the row '$2'" row "$1" "$2"; }
was_run() { grep -qxF "$1" "$ran"; }
gone() { ! kill -0 "$1" 2>/dev/null; }

start_tw

# --- What it shows ---------------------------------------------------------------
control a
for line in "Wi-Fi: not available" "Bluetooth: off" "Volume: 40%" "Output: Speakers" \
		"Microphone: on" "Brightness: 50%" "Keyboard light: 1 of 3" \
		"Battery: 74%, 4 h 0 min left" "Power mode: balanced" "Monitors: open" "Session: Lock"; do
	row a "$line" || fail "no row '$line': $(grep '^row' "$run_dir/a.log")"
done

# --- Changing it -------------------------------------------------------------------
# Down past Bluetooth to Volume; Right raises it, Return mutes.
press Down Down Right
expect_row a "Volume: 45%"
press Return
expect_row a "Volume: 45%, muted"
press Down Right
expect_row a "Output: Headphones"
press Down Return
expect_row a "Microphone: muted"
press Down Left
expect_row a "Brightness: 45%"
press Down Right
expect_row a "Keyboard light: 2 of 3"
# Past the battery to the power mode, which tw-power changes.
press Down Down Right
expect_row a "Power mode: performance"
wait_until "tw-power to be run" was_run "tw-power performance"
# Bluetooth: Right switches it on through tw-bluetooth.
press Up Up Up Up Up Up Up Right
wait_until "tw-bluetooth to be run" was_run "tw-bluetooth on"

# --- The session -------------------------------------------------------------------
# Restart asks for a second Return; Escape keeps it from happening.
press Down Down Down Down Down Down Down Down Down Down Right Right Right
expect_row a "Session: Restart"
press Return
wait_until "a second Return to be asked for" grep -q '^status: Return again to restart' "$run_dir/a.log"
press Escape
gone "$box" && fail "Escape after the question closed the box"
was_run "systemctl reboot" && fail "restarted without a second Return"
press Return Return
wait_until "systemctl reboot to be run" was_run "systemctl reboot"
wait_until "the box to go" gone "$box"

# Lock needs one Return and locks through the locker.
control b
press Up Up Up Up Up Up Up Up Up Up Up Up
for n in 1 2 3 4 5 6 7 8 9 10 11; do press Down; done
expect_row b "Session: Lock"
press Return
wait_until "the locker to be run" was_run "swaylock -f"
wait_until "the box to go" gone "$box"

# Monitors opens tw-monitors.
control c
for n in 1 2 3 4 5 6 7 8 9; do press Down; done
press Return
wait_until "tw-monitors to be run" was_run "tw-monitors"
# Gone before the next starts, or the next would take itself for the key
# pressed again and close it.
wait_until "the box to go" gone "$box"

# One box at a time: a second one closes the first.
control d
first=$box
env XDG_RUNTIME_DIR="$run_dir" "$control_bin" -d 2>"$run_dir/e.log" </dev/null ||
	fail "the second box failed"
grep -qx "closed the one that was open" "$run_dir/e.log" || fail "the second box did not close the first"
wait_until "the first box to go" gone "$first"

# Logging out stops tw, as Mod+Shift+E does.
control f
for n in 1 2 3 4 5 6 7 8 9 10; do press Down; done
press Right Return Return
wait_until "tw to stop" gone "$tw_pid"
code=0
wait "$tw_pid" || code=$?
[ "$code" = 0 ] || fail "tw exited with status $code after logging out"
tw_pid=
for log in "$run_dir"/*.log; do
	! grep -q 'Sanitizer\|runtime error' "$log" || fail "sanitizer report in $(basename "$log")"
done
echo "ok: rows, sound, light, power mode, Bluetooth, session actions and their question, one at a time, logging out"
