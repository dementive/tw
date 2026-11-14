#!/bin/sh
# Columns that hold several windows, and floating dialogs. $3 = tw-test-keys.
# Windows are named a..d; the comments track the strip, with [x] focused and
# "b,c" meaning b above c in one column.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3

on='activated=1 suspended=0'
width() { size=$(current_size "$1"); echo "${size%x*}"; }
height() { size=$(current_size "$1"); echo "${size#*x}"; }

start_tw
start_client a
start_client b
start_client c
expect_state c "$on" "the third window to be focused"
expect_state a "suspended=1" "the first of three columns to be off-screen"
column_width=$(width c)
full_height=$(height c)
# Half a column, minus the gap and borders between the two windows.
halved() {
	[ "$(height "$1")" -le $((full_height / 2)) ] &&
		[ "$(height "$1")" -ge $((full_height / 2 - 12)) ]
}
full() { [ "$(height "$1")" = "$full_height" ]; }
# strip: a | b | [c]

# --- Joining a column ----------------------------------------------------------
press super+bracketleft
wait_until "a window that joined a column to get half the height" halved c
wait_until "the window it joined to shrink to half as well" halved b
[ "$(width c)" = "$column_width" ] || fail "joining a column changed the window's width"
expect_state c "$on" "the window to keep focus when it joins a column"
expect_state a "activated=0 suspended=0" "two columns to fit on screen"
# strip: a | b,[c]

# --- Focus and order within a column -------------------------------------------
press super+i
expect_state b "$on" "Mod+I to focus the window above"
press super+k
expect_state c "$on" "Mod+K to focus the window below"
press super+Up
expect_state b "$on" "Mod+Up to focus the window above"
press super+Down
expect_state c "$on" "Mod+Down to focus the window below"
press super+ctrl+i super+k # c moves above b, so the window below it is now b
expect_state b "$on" "Mod+Ctrl+I to move the window up within its column"
# strip: a | c,[b]

# --- Leaving a column ----------------------------------------------------------
press super+bracketright
wait_until "a window that left a column to get its full height back" full b
wait_until "the window left behind to get its full height back" full c
expect_state b "$on" "the window to keep focus when it leaves a column"
expect_state a "suspended=1" "three columns not to fit again"
# strip: a | c | [b]

# Mod+Comma pulls the window on the right in; Mod+Period pushes the focused
# window out. Neither moves the focus.
press super+j super+comma
wait_until "Mod+Comma to pull the neighbour into the focused column" halved b
expect_state c "$on" "focus to stay put when pulling a window in"
# strip: a | [c],b
press super+period
wait_until "Mod+Period to give the focused window its own column" full c
wait_until "the window left behind to get its full height back" full b
expect_state c "$on" "focus to stay on the window pushed out"
# strip: a | b | [c]

# --- Closing a window that shares a column -------------------------------------
press super+bracketleft
wait_until "the window to join the column again" halved c
press super+q
expect_line c closed "Mod+Q to close the focused window"
wait_until "the remaining window to get the whole column" full b
expect_state b "$on" "focus to stay within the column"
# strip: a | [b]

# --- Fullscreen takes a window out of a shared column --------------------------
start_client d
press super+bracketleft
wait_until "the new window to join a column" halved d
press super+shift+f
expect_state d "$on fullscreen=1" "a window in a shared column to go fullscreen"
wait_until "the window it shared with to get the whole column" full b
press super+shift+f
expect_state d "$on fullscreen=0" "the window to leave fullscreen"
wait_until "the window to come back at full column height" full d
# strip: a | b | [d]

# --- Floating dialogs ----------------------------------------------------------
size_d=$(current_size d)
start_client dialog --fixed
expect_state dialog "configure 0x0 $on" "a fixed-size window to float at its own size"
expect_keyboard dialog enter "the dialog to take the keyboard"
expect_state d "activated=0 suspended=0" "the window behind a dialog to stay visible"
[ "$(current_size d)" = "$size_d" ] || fail "a dialog resized a tiled window"

# Focus keys act on the strip and leave the dialog, which stays open.
press super+j
expect_state b "$on" "Mod+J to move along the strip from behind a dialog"
expect_state dialog "activated=0 suspended=0" "the dialog to stay on screen without focus"

# Closing a dialog passes focus to another dialog, then back to the strip.
start_client dialog2 --fixed
expect_state dialog2 "$on" "a second dialog to be focused"
press super+q
expect_line dialog2 closed "Mod+Q to close the focused dialog"
expect_state dialog "$on" "focus to pass to the remaining dialog"
press super+q
expect_line dialog closed "Mod+Q to close the last dialog"
expect_keyboard b enter "the keyboard to return to the strip"
expect_state b "$on" "the strip's window to be active again"

# A dialog can be sent to another workspace like a column.
start_client dialog3 --fixed
expect_state dialog3 "$on" "a third dialog to be focused"
press super+ctrl+2
expect_state b "suspended=1" "workspace 1 to be hidden"
last_line_matches dialog3 configure "$on" || fail "the dialog did not stay focused on workspace 2"

stop_tw
echo "ok: stacked columns, floating dialogs"
