#!/bin/sh
# The mouse, driven through the virtual-pointer protocol: pointer focus, click
# to focus, pointer lock with raw movement (what games and 3D editors use for
# mouse-look), confinement, and no pointer input to windows while locked.
# $3 = tw-test-keys.
#
# The screen is 1280x720 and there are no gaps, so the first column starts at
# 0,0 and the second column at 640,0.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3

on='activated=1 suspended=0'
# count <name> <pattern>: matching lines in a client's log.
count() { grep -c "$2" "$run_dir/$1.log" || true; }
has() { [ "$(count "$1" "$2")" = "$3" ]; }

start_tw
start_client a
start_client game --lock-pointer
expect_state game "$on" "the second window to be focused"

# --- Pointer focus and click to focus ------------------------------------------
pointer_to 100 100
expect_line a "pointer enter 100 100" "the pointer to enter the window under it"
pointer_by 5 3
expect_line a "pointer motion 105 103" "movement to be reported in window coordinates"
last_line_matches a configure "activated=0" || fail "pointing at a window focused it"
click
expect_state a "$on" "a click to focus the window under the pointer"
expect_line a "button 272 pressed" "the click to reach the window too"
press x
wait_until "keys to follow the click" grep -q '^key ' "$run_dir/a.log"

# --- Pointer lock --------------------------------------------------------------
# The game asks for the lock as soon as the pointer enters it, but gets it only
# once it also has the keyboard.
pointer_to 800 100
expect_line a "pointer leave" "the pointer to leave the first window"
expect_line game "pointer enter 160 100" "the pointer to enter the game"
sleep 0.3
has game '^pointer locked' 0 || fail "a window without the keyboard locked the pointer"
click
expect_line game "pointer locked" "the focused window under the pointer to get its lock"

# Locked: the mouse arrives as raw movement and the cursor stays put, however
# far the mouse goes.
motions=$(count game '^pointer motion')
pointer_by 40 -15
expect_line game "relative 40 -15" "raw movement to reach the game"
pointer_by -3000 0
expect_line game "relative -3000 0" "raw movement to be unbounded"
pointer_to 100 100
sleep 0.3
has game '^pointer motion' "$motions" || fail "the cursor moved while the pointer was locked"
has game '^pointer leave' 0 || fail "the cursor left the game while the pointer was locked"

# A focus binding always gets the user out; the cursor is free again.
press super+j
expect_line game "pointer unlocked" "moving the focus away to release the lock"
expect_state a "$on" "Mod+J to focus the first window"
pointer_by 4 0
expect_line game "pointer motion 164 100" "the cursor to move again once unlocked"

# Back in the game the lock returns. When the game lets go of it itself, the
# cursor reappears where the game said.
click
wait_until "the lock to return with the focus" has game '^pointer locked' 2
press x
expect_line game "pointer released" "the game to let go of the pointer"
pointer_by 1 0
expect_line game "pointer motion 21 30" "the cursor to reappear where the game asked"

# --- Confinement ---------------------------------------------------------------
press super+q
wait_until "the game to close" grep -q '^closed' "$run_dir/game.log"
start_client box --confine-pointer
pointer_to 700 60
expect_line box "pointer enter 60 60" "the pointer to enter the confining window"
expect_line box "pointer confined" "the focused window under the pointer to confine it"
pointer_by 500 0
pointer_by 0 500
sleep 0.3
if grep '^pointer motion' "$run_dir/box.log" | awk '$3 > 99 || $4 > 99 { bad = 1 } END { exit !bad }'; then
	fail "the pointer escaped the region it is confined to"
fi
grep -q '^pointer motion 99 ' "$run_dir/box.log" || fail "the pointer did not reach the region's edge"
has box '^pointer leave' 0 || fail "the pointer left the window it is confined to"
press super+j
expect_line box "pointer unconfined" "moving the focus away to end the confinement"

# --- Locked session: the pointer reaches only the lock screen -------------------
before=$(count a '^pointer\|^button')
start_client locker --lock
expect_line locker locked "the session to lock"
pointer_to 100 100
expect_line locker "pointer enter 100 100" "the pointer to go to the lock screen"
click
expect_line locker "button 272 pressed" "clicks to go to the lock screen"
sleep 0.3
has a '^pointer\|^button' "$before" || fail "a window got pointer input while the session was locked"
press Return
expect_line locker unlocked "the lock client to unlock"

stop_tw
echo "ok: pointer focus, click to focus, lock, raw movement, cursor hint, confinement, locked session"
