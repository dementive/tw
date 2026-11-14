#!/bin/sh
# Two monitors. Each shows a workspace of its own out of the same nine; the
# focus and columns move between them, and a window is only ever drawn on the
# monitor its workspace is on. $3 = tw-test-keys.
#
# The headless backend provides two 1280x720 outputs and announces the second
# one first, so they end up side by side with HEADLESS-2 on the left showing
# workspace 1 and HEADLESS-1 on the right showing workspace 2.
# Unplugging a monitor cannot be simulated and is not covered.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3
export WLR_HEADLESS_OUTPUTS=2
screen_size=2560,720

on='activated=1 suspended=0'       # focused
visible='activated=0 suspended=0'  # on a monitor, without the focus
hidden='suspended=1'
left=HEADLESS-2
right=HEADLESS-1
# never_on <name> <monitor>: the window was never drawn on that monitor.
never_on() {
	if grep -q "^output enter $2\$" "$run_dir/$1.log"; then
		fail "window $1 was drawn on $2"
	fi
}

start_tw

# --- A workspace per monitor ----------------------------------------------------
start_client a
expect_line a "output enter $left" "the first window to open on the first monitor"
expect_state a "$on" "the first window to be focused"

press super+shift+l
expect_state a "$visible" "a window to stay on its monitor when the focus leaves for the other"
start_client b
expect_line b "output enter $right" "a new window to open on the focused monitor"
expect_state b "$on" "the window on the second monitor to be focused"
never_on b $left
[ "$(current_size b)" = "$(current_size a)" ] || fail "windows on equal monitors differ in size"

press super+shift+j
expect_state a "$on" "Mod+Shift+J to move the focus to the monitor on the left"
expect_state b "$visible" "the other monitor to keep showing its window"
# Nothing further left: no monitor, and no workspace before the first.
press super+shift+j
sleep 0.3
last_line_matches a configure "$on" || fail "Mod+Shift+J did something with nothing further left"
press super+shift+Right
expect_state b "$on" "Mod+Shift+Right to move the focus to the monitor on the right"
press super+shift+Left
expect_state a "$on" "Mod+Shift+Left to move the focus back"

# --- Moving a column to the other monitor ---------------------------------------
press super+shift+ctrl+l
expect_line a "output enter $right" "Mod+Shift+Ctrl+L to move the column to the monitor on the right"
expect_line a "output leave $left" "the moved column to leave its old monitor"
expect_state a "$on" "the focus to follow the moved column"
# left: workspace 1, empty.   right: workspace 2 with b [a]

# --- Workspaces across monitors -------------------------------------------------
# A workspace that the other monitor shows stays there; the focus goes to it.
press super+1
expect_state a "$visible" "Mod+1 to move the focus to the monitor showing workspace 1"
start_client c
expect_line c "output enter $left" "a window opened then to appear on that monitor"
press super+2
expect_state a "$on" "Mod+2 to return the focus to the monitor showing workspace 2"
expect_state c "$visible" "workspace 1 to stay on its monitor"

# A hidden workspace replaces the one on the focused monitor only.
press super+3
expect_state a "$hidden" "Mod+3 to replace workspace 2 on the focused monitor"
expect_state b "$hidden" "all of workspace 2 to be hidden"
last_line_matches c configure "$visible" || fail "switching workspace on one monitor disturbed the other"
start_client d
expect_line d "output enter $right" "a window on workspace 3 to appear on the second monitor"

# With no monitor on that side, Shift and a direction step through the
# workspaces of this monitor instead.
press super+shift+l
expect_state d "$hidden" "Mod+Shift+L, with no monitor further right, to go to the next workspace"
last_line_matches c configure "$visible" || fail "Mod+Shift+L disturbed the other monitor"
press super+shift+i
expect_state d "$on" "Mod+Shift+I, with no monitor above, to go to the previous workspace"

# Stepping through workspaces skips the one the other monitor shows.
press super+Page_Up
expect_state a "$on" "Mod+Page_Up to go from workspace 3 to workspace 2"
expect_state d "$hidden" "workspace 3 to be hidden again"
press super+Page_Up
sleep 0.3
last_line_matches a configure "$on" ||
	fail "Mod+Page_Up took the workspace away from the other monitor"
last_line_matches c configure "$visible" || fail "Mod+Page_Up disturbed the other monitor"

# --- The mouse across monitors --------------------------------------------------
pointer_to 100 100
click
expect_state c "$on" "a click on a window on the other monitor to focus it"
expect_state a "$visible" "the window that lost the focus to stay visible"

# A click on an empty monitor moves the focus there.
press super+shift+l super+4
expect_state a "$hidden" "Mod+4 to show an empty workspace on the second monitor"
press super+shift+j
expect_state c "$on" "the focus to be back on the first monitor"
pointer_to 2000 300
click
expect_state c "$visible" "a click on the empty monitor to take the focus from the first"
start_client e
expect_line e "output enter $right" "a window opened after that click to appear on the clicked monitor"

# A launcher that names no monitor appears on the focused one.
start_client launcher --layer
expect_line launcher "output enter $right" "a launcher to open on the focused monitor"
never_on launcher $left
kill "$last_client_pid"
expect_keyboard e enter "the keyboard to return to the window"

# --- Fullscreen covers its own monitor ------------------------------------------
press super+shift+f
expect_state e "$on fullscreen=1" "Mod+Shift+F to make the window fullscreen"
[ "$(current_size e)" = 1280x720 ] || fail "fullscreen is $(current_size e), not one monitor"
never_on e $left
last_line_matches c configure "$visible" || fail "fullscreen on one monitor hid the other"
press super+shift+f

# --- A column sticking out of its monitor is not drawn on the next --------------
# On the left monitor c is widened to two thirds, so the column after it hangs
# over the right edge, where the other monitor begins.
press super+shift+j super+r
start_client f
expect_state f "$on" "the new column to be focused"
press super+j
expect_state c "$on" "Mod+J to scroll back to the first column"
expect_state f "$visible" "the second column to stay partly in view"
sleep 0.6 # let the scroll animation finish
never_on f $right
never_on c $right

stop_tw
echo "ok: two monitors, focus and columns between them, workspaces, mouse, fullscreen, trimming"
