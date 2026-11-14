#!/bin/sh
# Screen blanking: the outputs power off after a while without input and every
# window is told to stop drawing; a visible idle inhibitor (a video player)
# holds that off. $3 = tw-test-keys.
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

press x
expect_state a "$on" "a key press to turn the screen back on"

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
echo "ok: blanking, inhibitors, manual power-off"
