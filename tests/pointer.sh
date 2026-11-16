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
# It opens where the game was, under the pointer, which it gets at once
# without the pointer moving.
expect_line box "pointer enter 21 30" "a window opening under the pointer to get it"
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
# The pointer rests on a window as the session locks, as it does when the
# session locks on idle. The lock screen appears under it and gets it
# without the pointer moving: a click goes to the lock screen, not to the
# window.
pointer_to 100 100
sleep 0.2
buttons() { cat "$run_dir/a.log" "$run_dir/game.log" "$run_dir/box.log" | grep -c '^button' || true; }
buttons_before=$(buttons)
start_client locker --lock
expect_line locker locked "the session to lock"
expect_line locker "pointer enter 100 100" "the lock screen to get the pointer resting under it"
before=$(count a '^pointer\|^button')
click
expect_line locker "button 272 pressed" "a click without moving the pointer to go to the lock screen"
[ "$(buttons)" = "$buttons_before" ] || fail "a click without moving the pointer reached a window behind the lock"
pointer_by 5 0
expect_line locker "pointer motion 105 100" "movement to go to the lock screen"
sleep 0.3
has a '^pointer\|^button' "$before" || fail "a window got pointer input while the session was locked"
press Return
expect_line locker unlocked "the lock client to unlock"

# A button held down as the session locks (mouse-look, a selection being
# made) does not keep the pointer on the window behind the curtain: not even
# the button's release reaches it.
pointer_to 100 100
pressed=$(count a '^button 272 pressed')
press down pause:1500 &
held=$!
button_down() { [ "$(count a '^button 272 pressed')" -gt "$pressed" ]; }
wait_until "the button to go down in the window" button_down
released=$(count a '^button 272 released')
start_client locker3 --lock
expect_line locker3 locked "the session to lock with a button held"
wait_until "the window to lose the pointer as the session locks" last_line_matches a pointer "pointer leave"
wait "$held"
sleep 0.2
has a '^button 272 released' "$released" || fail "a button let go while locked reached the window behind the lock"
press Return
expect_line locker3 unlocked "the lock client to unlock"

# --- An open menu does not keep the keyboard through a lock ---------------------
# A menu holds the keyboard and the pointer while it is open. When the session
# locks with one open, the lock screen still gets the keys: the menu closes.
start_client menu --menu
expect_line menu "menu shown" "the menu to open"
# Moved by its client to far off the screen, it is kept on it. The window
# is no wider than the screen, so a menu kept on it starts left of 1280.
menu_placed() { [ "$(grep -c '^menu at' "$run_dir/$1.log")" -ge 2 ]; }
wait_until "the moved menu to be placed again" menu_placed menu
grep '^menu at' "$run_dir/menu.log" | tail -n 1 | awk '{ exit !($3 + 100 <= 1280) }' ||
	fail "a menu its client moved went off the screen: $(grep '^menu at' "$run_dir/menu.log" | tail -n 1)"
start_client locker2 --lock
expect_line locker2 locked "the session to lock with a menu open"
expect_line menu "menu closed" "the lock to close the menu"
press Return
expect_line locker2 unlocked "the lock screen to get the keyboard although a menu was open"

# --- The strip glides under a resting pointer -----------------------------------
# The pointer goes to whatever the glide brings under it, without moving: a
# scroll right after Mod+End reaches the window now under it.
press super+End
start_client p1
start_client p2
expect_state p2 "$on" "the newest window to be focused"
press super+Home
pointer_to 100 100
expect_line a "pointer enter 100 100" "the pointer to enter the first window"
press super+End
wait_until "the window the strip glides under the pointer to get it" \
	grep -q '^pointer enter' "$run_dir/p1.log"
expect_line p1 "pointer motion 100 100" "the pointer to follow the glide to its end"
last_line_matches a pointer leave || fail "the first window kept the pointer after the glide"

# --- Screens off: the pointer is over nothing ----------------------------------
# A cursor a window chose does not stay on a dark screen (an animated "busy"
# one would wake tw at every image): the window loses the pointer as the
# screens go off, and gets it back where it was as they come on.
press super+shift+p
wait_until "the window under the pointer to lose it as the screens go off" \
	last_line_matches p1 pointer "pointer leave"
press x
wait_until "the window under the pointer to get it back as the screens come on" \
	last_line_matches p1 pointer "pointer enter 100 100"

# --- Drag and drop ---------------------------------------------------------------
# Text dragged from one window to the one beside it arrives there. The two
# share the screen: the drop window on the left, the drag window, newest
# and focused, on the right.
start_client drop --drop
start_client drag --drag
expect_state drag "$on" "the drag window to open focused"
pointer_to 900 100
wait_until "the strip to come to rest with the drag window under the pointer" \
	grep -Eq '^pointer (enter|motion) 260 100$' "$run_dir/drag.log"
# A hand's pace: the drag window asks for the drag in answer to the press,
# and the drop window says it takes text on entering, both while the button
# is still down.
press down pause:300 "move:300,120:$screen_size" pause:300 up
expect_line drag "drag started" "a press in the drag window to start a drag"
expect_line drop "drop enter" "the drag to reach the window beside it"
expect_line drop "drop" "the drop to arrive"
expect_line drag "drag dropped" "the drag window to hear that it was dropped"
expect_line drag "drag finished" "the drop window to finish the drag"

# A drag carried to an empty workspace lands on nothing there, not on the
# window it was last over, now out of sight.
pointer_to 900 100
press down pause:300 "move:300,120:$screen_size" pause:300 super+9 pause:300 up
expect_line drag "drag cancelled" "a drag let go over an empty workspace to come to nothing"
# While it lasted no window had the keyboard; afterwards it is where the focus
# went, which is nowhere on the empty workspace.
expect_keyboard drag leave "the keyboard to follow the focus once the drag is over"
[ "$(grep -c '^drop$' "$run_dir/drop.log")" = 1 ] || fail "a drag let go over an empty workspace landed on a hidden window"

stop_tw
echo "ok: pointer focus, click to focus, lock, raw movement, cursor hint, confinement, locked session, a glide under the pointer, screens off"
