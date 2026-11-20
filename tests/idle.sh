#!/bin/sh
# Screen blanking: the outputs power off after a while without input and every
# window is told to stop drawing; a visible idle inhibitor (a video player)
# holds that off, and a notification turns the screen back on. Blanking
# locks nothing. With a locker installed the session
# locks after a longer time without input, and the machine is suspended after
# a longer one still.
# $3 = tw-test-keys.
#
# "Blanked" is observed through the window state: a visible window is
# suspended only when the screen is off.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3

on='activated=1 suspended=0'

start_tw -b 2 # blank after two seconds, so the test does not take minutes

start_client a
expect_state a "$on" "the window to start visible"
expect_state a "suspended=1" "the screen to turn off after two seconds without input"

# No locker is installed here, so the session did not lock: locking without
# one would leave no way back in.
press x
expect_state a "$on" "a key press to turn the screen back on"
press super+alt+l
sleep 0.3
last_line_matches a configure "$on" || fail "Mod+Alt+L locked the session with no locker installed"

# A visible window that inhibits idling keeps the screen on.
start_client video --inhibit
video_pid=$last_client_pid
expect_state video "$on" "the inhibiting window to be focused"
sleep 3.5
last_line_matches video configure "suspended=0" ||
	fail "the screen turned off under a visible idle inhibitor"

# An inhibitor nobody can see does not: on workspace 2 the screen goes off.
press super+2
start_client b
expect_state b "$on" "a window on workspace 2 to be visible"
expect_state b "suspended=1" "an inhibitor on a hidden workspace not to keep the screen on"

# Back on workspace 1 the inhibitor counts again, until its window closes.
press super+1
expect_state video "$on" "a key press to wake the screen on workspace 1"
sleep 3.5
last_line_matches video configure "suspended=0" ||
	fail "the screen turned off although the inhibitor became visible again"
kill "$video_pid"
expect_state a "suspended=1" "the screen to turn off once the inhibitor is gone"

stop_tw

# A notification turns the screen back on, for another blank timeout; a
# launcher, which takes the keyboard and comes from input anyway, does not.
start_tw -b 2
start_client n
expect_state n "$on" "the window to start visible"
expect_state n "suspended=1" "the screen to turn off after two seconds without input"
start_client launcher --layer
launcher_pid=$last_client_pid
sleep 0.5
last_line_matches n configure "suspended=1" || fail "a launcher turned the screen on"
kill "$launcher_pid"
start_client note --notification
expect_state n "suspended=0" "a notification to turn the screen back on"
expect_state n "suspended=1" "the screen to turn off again two seconds after the notification"
stop_tw

# Mod+Shift+P turns the screen off at once, with the timeout disabled. Letting
# go of those keys must not wake it; the next key press does.
start_tw -b 0
start_client c
expect_state c "$on" "the window to start visible"
sleep 2.5
last_line_matches c configure "suspended=0" || fail "the screen turned off with blanking disabled"
press super+shift+p
expect_state c "suspended=1" "Mod+Shift+P to turn the screen off"
sleep 0.5
last_line_matches c configure "suspended=1" || fail "releasing the keys woke the screen"
press x
expect_state c "$on" "the next key press to turn the screen back on"

# A lock client tw did not start dies: the session stays locked. A key press
# then starts the locker only if it is installed; tw does not run a missing
# program to find out (the shell would say "not found"). Another lock client
# takes over and unlocks.
start_client outside --lock
outside_pid=$last_client_pid
expect_line outside locked "the outside lock client to lock"
kill "$outside_pid"
press x
sleep 0.3
last_line_matches c configure "suspended=1" || fail "the session unlocked when the lock client died"
if grep -q 'not found' "$run_dir/tw.log"; then
	fail "a key press ran the locker, which is not installed"
fi
start_client rescue --lock
expect_line rescue locked "a new lock client to take over the abandoned lock"
press Return
expect_state c "$on" "the new lock client to unlock"

stop_tw

# --- Locking -------------------------------------------------------------------
# With a locker installed, tw locks by itself after the lock timeout, the
# screen long off by then, and starts the locker, which takes the lock over.
# Blanking alone locks nothing. The stand-in is the test client.
fake_program swaylock "
echo \$\$ >'$run_dir/locker.pid'
exec '$client_bin' --lock >>'$run_dir/locker.log' 2>&1"
locks() { [ "$(grep -c '^locked' "$run_dir/locker.log" 2>/dev/null)" = "$1" ]; }

# Input between blanking and locking brings the screen back unlocked, and
# starts the lock clock over.
start_tw -b 1 -l 4
start_client f
expect_state f "$on" "the window to start visible"
expect_state f "suspended=1" "the screen to turn off after a second without input"
press x
expect_state f "$on" "a key press to bring the screen back"
[ ! -e "$run_dir/locker.log" ] || fail "the session locked as the screen blanked"
sleep 3
[ ! -e "$run_dir/locker.log" ] || fail "the lock clock did not start over at the key press"
stop_tw

start_tw -b 2 -l 4
start_client d
expect_state d "$on" "the window to start visible"
expect_state d "suspended=1" "the screen to turn off after two seconds without input"
[ ! -e "$run_dir/locker.log" ] || fail "the session locked as the screen blanked"
expect_line locker locked "the locker to be started after four seconds without input"

# Waking the screen shows the lock screen, not the window.
press x
wait_until "the key to go to the lock screen" grep -q '^key ' "$run_dir/locker.log"
if grep -q '^key ' "$run_dir/d.log" || ! last_line_matches d configure "suspended=1"; then
	fail "the window was reachable after waking a locked session"
fi

# The locker dies: the session stays locked, and a key press gets a new one.
# tw starts at most one a second, hence the wait.
kill "$(cat "$run_dir/locker.pid")"
sleep 1.2
last_line_matches d configure "suspended=1" || fail "the session unlocked when the locker died"
press x
wait_until "a key press to start a new locker" locks 2
press Return
expect_state d "$on" "the new locker to unlock the session"

# Mod+Alt+L locks at once.
press super+alt+l
wait_until "Mod+Alt+L to lock" locks 3
expect_state d "suspended=1" "windows to be hidden while locked"
press Return
expect_state d "$on" "the locker to unlock again"

stop_tw

# A notification turns the screen on but is not input: it does not put the
# lock off. Without input since tw started, the lock is due at four seconds;
# counted from the notification it would come one and a half later.
locked_before=$(grep -c '^locked' "$run_dir/locker.log")
start_tw -b 1 -l 4
start_client g
expect_state g "suspended=1" "the screen to turn off after a second without input"
sleep 0.5
start_client news --notification
expect_state g "suspended=0" "a notification to turn the screen back on"
sleep 3.1
locks $((locked_before + 1)) || fail "a notification put the lock off: it counted as input"
stop_tw

# --- Suspend -------------------------------------------------------------------
# After a longer time without input tw locks the session and runs the suspend
# command. The stand-in suspends nothing, so tw finds the machine still awake
# and tries again after the same time.
fake_program systemctl "echo \"\$*\" >>'$run_dir/suspend.log'"
suspends() {
	n=0
	[ -e "$run_dir/suspend.log" ] && n=$(grep -c . "$run_dir/suspend.log")
	[ "$n" = "$1" ]
}
locked_before=$(grep -c '^locked' "$run_dir/locker.log")

start_tw -b 0 -z 2 # never blank, suspend after two seconds
start_client e
expect_state e "$on" "the window to start visible"
wait_until "the machine to be suspended after two seconds without input" suspends 1
grep -qx suspend "$run_dir/suspend.log" ||
	fail "unexpected suspend command: $(cat "$run_dir/suspend.log")"
wait_until "the session to be locked before suspending" locks $((locked_before + 1))
expect_state e "suspended=1" "windows to be hidden before suspending"
wait_until "another attempt while the machine is still awake" suspends 2

# Input starts the clock over. A visible idle inhibitor then holds suspending
# off for as long as it is on screen.
press Return
expect_state e "$on" "the locker to unlock"
start_client film --inhibit
film_pid=$last_client_pid
expect_state film "$on" "the inhibiting window to be focused"
sleep 3.5
suspends 2 || fail "the machine was suspended under a visible idle inhibitor"
kill "$film_pid"
wait_until "the machine to be suspended once the inhibitor is gone" suspends 3

# A layer surface's inhibitor (a video on the overlay layer) counts while it
# is on screen, but not behind the lock curtain: a locked machine suspends.
press Return
expect_state e "$on" "the locker to unlock"
start_client overlay --layer-inhibit
sleep 3.5
suspends 3 || fail "the machine was suspended under a visible layer surface's inhibitor"
locked_now=$(grep -c '^locked' "$run_dir/locker.log")
press super+alt+l
wait_until "Mod+Alt+L to lock" locks $((locked_now + 1))
wait_until "the machine to be suspended with the inhibitor behind the lock" suspends 4

stop_tw

# An inhibitor going away starts the clock over as input does, attempts
# included: the next one is a period away, not one period per attempt that
# failed before it. Without a locker nothing hides the inhibitor as tw tries.
rm "$bin_dir/swaylock" "$run_dir/suspend.log"
start_tw -b 0 -z 1
start_client g
expect_state g "$on" "the window to start visible"
wait_until "three attempts, a second apart" suspends 3
start_client show --inhibit
show_pid=$last_client_pid
expect_state show "$on" "the inhibiting window to be focused"
sleep 1.5
attempts=$(grep -c . "$run_dir/suspend.log")
kill "$show_pid"
sleep 2.5
[ "$(grep -c . "$run_dir/suspend.log")" -gt "$attempts" ] ||
	fail "after the inhibitor went, the next attempt was not a period away"

stop_tw

# As the screen goes off, tw runs tw-power away, and back as it comes on: the
# keyboards and mice plugged in sleep only meanwhile. One at a time and in
# order: a screen that comes back on while away still runs gets back after
# it, not beside it. Nothing runs as tw starts.
inputs_log=$run_dir/inputs.log
fake_program tw-power "echo \"start \$*\" >>'$inputs_log'
[ \"\$*\" = away ] && /bin/sleep 1
echo \"end \$*\" >>'$inputs_log'"
start_tw -b 2
start_client h
expect_state h "$on" "the window to start visible"
[ ! -e "$inputs_log" ] || fail "tw ran tw-power as it started: $(cat "$inputs_log")"
expect_state h "suspended=1" "the screen to turn off after two seconds without input"
wait_until "tw-power away to run as the screen goes off" grep -q '^start away$' "$inputs_log"
press x
expect_state h "$on" "a key press to turn the screen back on"
wait_until "tw-power back to run as the screen comes on" grep -q '^end back$' "$inputs_log"
[ "$(cat "$inputs_log" | tr '\n' ,)" = "start away,end away,start back,end back," ] ||
	fail "away and back did not run one after the other: $(cat "$inputs_log" | tr '\n' ,)"
stop_tw
echo "ok: blanking, inhibitors, a notification waking the screen, manual power-off, abandoned lock without a locker, lock on idle after blanking and despite a notification, locker restart, suspend, the clock after an inhibitor, keyboards and mice let sleep with the screen"
