#!/bin/sh
# Screen blanking: the outputs power off after a while without input and every
# window is told to stop drawing; a visible idle inhibitor (a video player)
# holds that off. With a locker installed the session locks as it blanks.
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

stop_tw

# --- Locking -------------------------------------------------------------------
# With a locker installed, tw locks by itself as the screen blanks and starts
# the locker, which takes the lock over. The stand-in is the test client.
fake_program swaylock "
echo \$\$ >'$run_dir/locker.pid'
exec '$client_bin' --lock >>'$run_dir/locker.log' 2>&1"
locks() { [ "$(grep -c '^locked' "$run_dir/locker.log")" = "$1" ]; }

start_tw -b 2
start_client d
expect_state d "$on" "the window to start visible"
expect_state d "suspended=1" "the screen to turn off after two seconds without input"
expect_line locker locked "the locker to be started as the screen blanks"

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
echo "ok: blanking, inhibitors, manual power-off, lock on idle, locker restart"
