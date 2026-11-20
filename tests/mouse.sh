#!/bin/sh
# The mouse's bindings: the wheel and its tilt with Mod, a mouse button held
# as Mod (the G502's G6), buttons bound to actions, Mod and the middle button
# holding the strip, and none of it while locked. $3 = tw-test-keys.
#
# The screen is 1280x720 with no gaps: three columns of 640 make a strip
# 1920 wide.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3

on='activated=1 suspended=0'
hidden='suspended=1'
# Evdev codes, as in src/config.h.
middle=274 right=273 back=275 g6=277 g9=278
# none <pattern> <description>: no window was sent such a line.
none() {
	if grep -l "$1" "$run_dir"/[a-d].log >/dev/null 2>&1; then
		fail "$2: $(grep -H "$1" "$run_dir"/[a-d].log)"
	fi
}

fake_program tw-launch "echo launched >>'$run_dir/launched.log'"
# The pointer rests where c, the third column, comes to rest: the windows
# gliding past under it take the pointer but not the focus.
start_tw
pointer_to 1000 300
start_client a
start_client b
start_client c
expect_state c "$on" "the newest window to be focused"
wait_until "c to come to rest under the pointer" \
	last_line_matches c pointer "pointer (enter|motion) 360 300"

# --- The wheel ---------------------------------------------------------------
# Without Mod it scrolls the window under the pointer.
press wheel:1
expect_line c "axis 0 15" "the wheel without Mod to scroll the window under the pointer"

# With Mod it goes along the strip, a column a notch, and the windows hear
# nothing of it. Two notches in one event are two steps.
press hold:super wheel:-1
expect_state b "$on" "Mod and the wheel up to focus the column on the left"
press hold:super wheel:-1
expect_state a "$on" "Mod and the wheel up to go on along the strip"
press hold:super wheel:2
expect_state c "$on" "two notches down to go two columns right"
grep -q "^configure .* $on" "$run_dir/b.log" || fail "the two notches skipped the column between"

# With Mod and Shift it goes through the workspaces, but no faster than one
# step in 150 ms: two notches at once are one step. The window opened after
# them is on the next workspace, not the one after.
press hold:super+shift wheel:2
expect_state c "$hidden" "Mod+Shift and the wheel down to show the next workspace"
start_client d
press super+1
expect_state c "$on" "Mod+1 to go back"
press hold:super+shift pause:200 wheel:1
expect_state d "$on" "the wheel spun two notches to have gone one workspace"
press hold:super+shift wheel:-1
expect_state c "$on" "Mod+Shift and the wheel up to show the previous workspace"

# Tilted, with Mod it goes along the strip, and with Mod+Shift, with one
# monitor, to the next or previous workspace.
press hold:super tilt:-1
expect_state b "$on" "Mod and the wheel tilted left to focus the column on the left"
press hold:super tilt:1
expect_state c "$on" "Mod and the wheel tilted right to focus the column on the right"
press hold:super+shift tilt:1
expect_state d "$on" "Mod+Shift and the wheel tilted right to show the next workspace"
press hold:super+shift tilt:-1
expect_state c "$on" "Mod+Shift and the wheel tilted left to show the previous workspace"
[ "$(cat "$run_dir"/[a-d].log | grep -c '^axis')" = 1 ] ||
	fail "a window heard the wheel turned with Mod: $(grep -H '^axis' "$run_dir"/[a-d].log)"

# --- A mouse button held as Mod ------------------------------------------------
# G6 held stands for Mod, with no key held; the windows never hear it.
press down:$g6 wheel:-1 up:$g6
expect_state b "$on" "G6 held and the wheel up to focus the column on the left"
none "^button $g6" "a window heard the button that is Mod"

# --- Mod and the middle button hold the strip -----------------------------------
# The view shows b and c, and the cursor is in the middle of b. Held there
# and moved right, the strip follows the mouse; let go, it settles at its
# start, c out of sight. Moved back left, it settles with a out of sight.
# The focus stays on b, in view throughout. The hand moves at a hand's pace
# and rests before letting go, so the strip has no speed left to coast on.
# drag <dx>: seven steps of dx.
drag() {
	set -- "$1" pause:20 "$1" pause:20 "$1" pause:20 "$1" pause:20 "$1" pause:20 "$1" pause:20 "$1"
	press hold:super down:$middle "$@" pause:150 up:$middle
}
drag rel:100,0
expect_state c "$hidden" "the strip dragged right to settle at its start"
drag rel:-100,0
expect_state a "$hidden" "the strip dragged left to settle with the first column out of sight"
last_line_matches b configure "$on" || fail "b, in view throughout, lost the focus"
none "^button $middle" "a window heard the middle button that held the strip"

# --- Buttons --------------------------------------------------------------------
# Mod and the right button cycle the width of the window under the pointer.
press hold:super click:$right
wait_until "Mod and the right button to widen b to two thirds" \
	sh -c "grep '^configure' '$run_dir/b.log' | tail -n 1 | grep -q '^configure 853x720 '"
none "^button $right" "a window heard Mod and the right button"

# The thumb's back button is the window's, and with Mod goes left.
press click:$back
expect_line b "button $back pressed" "the back button without Mod to reach the window"
press hold:super click:$back
expect_state a "$on" "Mod and the back button to focus the column on the left"

# G9 opens the launcher, focusing the window under the pointer first as a
# click does.
press click:$g9
wait_until "G9 to start the launcher" grep -qx launched "$run_dir/launched.log"
none "^button $g9" "a window heard G9"

# --- Locked ----------------------------------------------------------------------
# No mouse binding runs: the wheel with Mod and G9 go to the lock screen.
start_client locker --lock
expect_line locker locked "the session to lock"
press hold:super wheel:1
expect_line locker "axis 0 15" "Mod and the wheel to reach the lock screen"
press click:$g9
expect_line locker "button $g9 pressed" "G9 to reach the lock screen"
[ "$(grep -c . "$run_dir/launched.log")" = 1 ] || fail "G9 started the launcher while locked"
press Return
expect_line locker unlocked "the lock client to unlock"
expect_state b "$on" "the focus to be where it was before the lock, Mod and the wheel having done nothing"

stop_tw
echo "ok: the wheel and its tilt with Mod, a button held as Mod, the strip held with the mouse, button bindings, nothing while locked"
