#!/bin/sh
# Two monitors. Each has nine workspaces of its own and shows one of them; the
# focus and columns move between them, and a window is only ever drawn on the
# monitor its workspace is on. $3 = tw-test-keys.
#
# The headless backend provides two 1280x720 outputs and announces the second
# one first, so they end up side by side with HEADLESS-2 on the left and
# HEADLESS-1 on the right, each showing its own workspace 1.
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
# only_states_since <name> <point> <regex>: every state told since matches.
only_states_since() { ! since "$1" "$2" | grep '^configure' | grep -Eqv "$3"; }

start_tw

# --- A monitor each ------------------------------------------------------------
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
point=$(lines a)
press super+shift+j
press super+shift+Right
expect_state b "$on" "Mod+Shift+Right to move the focus to the monitor on the right"
expect_state a "$visible" "the focus to leave the first monitor"
only_states_since a "$point" "$visible" ||
	fail "Mod+Shift+J did something with nothing further left"
press super+shift+Left
expect_state a "$on" "Mod+Shift+Left to move the focus back"

# --- Moving a column to the other monitor ---------------------------------------
press super+shift+ctrl+l
expect_line a "output enter $right" "Mod+Shift+Ctrl+L to move the column to the monitor on the right"
expect_line a "output leave $left" "the moved column to leave its old monitor"
expect_state a "$on" "the focus to follow the moved column"
# left: workspace 1, empty.   right: workspace 1 with b [a]

# --- Each monitor has nine workspaces of its own --------------------------------
# Mod and a number go to that workspace of the focused monitor, on that
# monitor; the other monitor keeps showing what it shows.
press super+shift+j
start_client c
expect_line c "output enter $left" "a window opened on the left monitor to appear there"
press super+shift+l
expect_state a "$on" "Mod+Shift+L to move the focus back to the right monitor"
expect_state c "$visible" "the left monitor to keep showing its window"
c_point=$(lines c) # c is not to be touched until the pointer comes to it
press super+2
expect_state a "$hidden" "Mod+2 to show the right monitor's own workspace 2"
expect_state b "$hidden" "all of the right monitor's workspace 1 to be hidden"
start_client d
expect_line d "output enter $right" "a window on workspace 2 to appear on the right monitor"
press super+1
expect_state a "$on" "Mod+1 to bring back the right monitor's workspace 1, with its focus"
expect_state d "$hidden" "workspace 2 of the right monitor to be hidden again"

# With no monitor on that side, Shift and a direction step through the
# workspaces of this monitor instead.
press super+shift+l
expect_state d "$on" "Mod+Shift+L, with no monitor further right, to go to the next workspace"
press super+shift+i
expect_state a "$on" "Mod+Shift+I, with no monitor above, to go to the previous workspace"

# Stepping through workspaces stays on this monitor, and stops at its first.
press super+Page_Down
expect_state d "$on" "Mod+Page_Down to go to workspace 2"
press super+Page_Up
expect_state a "$on" "Mod+Page_Up to go back to workspace 1"
point=$(lines a)
press super+Page_Up

# --- The mouse across monitors --------------------------------------------------
# The focus follows the pointer onto a window on the other monitor.
pointer_to 100 100
expect_state c "$on" "the pointer moved onto a window on the other monitor to focus it"
expect_state a "$visible" "the window that lost the focus to stay visible"
only_states_since a "$point" "$visible" ||
	fail "Mod+Page_Up on the first workspace did something"
only_states_since c "$c_point" "$on" ||
	fail "switching workspaces on one monitor disturbed the other"

# A key that moves the focus to the other monitor takes the cursor along, to
# the middle of the window it focuses there. Moved onto an empty monitor, the
# pointer takes the focus there.
press super+shift+l super+4
expect_state a "$hidden" "Mod+4 to show an empty workspace on the second monitor"
press super+shift+j
expect_state c "$on" "the focus to be back on the first monitor"
c_size=$(current_size c)
expect_line c "pointer enter $((${c_size%x*} / 2)) $((${c_size#*x} / 2))" \
	"Mod+Shift+J to take the cursor to the middle of the window it focused"
pointer_to 2000 300
expect_state c "$visible" "the pointer moved onto the empty monitor to take the focus from the first"
start_client e
expect_line e "output enter $right" "a window opened after that to appear on the monitor the pointer is on"

# A launcher that names no monitor appears on the focused one.
start_client launcher --layer
expect_line launcher "output enter $right" "a launcher to open on the focused monitor"
never_on launcher $left
kill "$last_client_pid"
expect_keyboard e enter "the keyboard to return to the window"

# A bar keeps its room on its own monitor: the windows there are made shorter
# by its height, those on the other are not, and the room comes back when it
# goes. wlroots counts the room only once the bar is mapped.
e_size=$(current_size e)
c_size=$(current_size c)
c_point=$(lines c) # nor until the focus returns to it below
start_client bar --bar
expect_line bar "layer configure 1280x30" "a bar to be configured across its monitor"
wait_until "a bar to take its height off the windows on its monitor" \
	last_line_matches e configure "^configure ${e_size%x*}x$((${e_size#*x} - 30)) "
expect_line bar "output enter $right" "the bar to be drawn on its monitor"
never_on bar $left
kill "$last_client_pid"
wait_until "the room a bar kept to come back when it goes" \
	last_line_matches e configure "^configure $e_size "

# --- Fullscreen covers its own monitor ------------------------------------------
press super+shift+f
expect_state e "$on fullscreen=1" "Mod+Shift+F to make the window fullscreen"
[ "$(current_size e)" = 1280x720 ] || fail "fullscreen is $(current_size e), not one monitor"
never_on e $left
press super+shift+f
expect_state e "^configure $e_size $on fullscreen=0" "Mod+Shift+F again to give the window its column back"

# --- A column sticking out of its monitor is not drawn on the next --------------
# On the left monitor c is widened to two thirds, so the column after it hangs
# over the right edge, where the other monitor begins.
press super+shift+j
expect_state c "$on" "the focus to return to the first monitor"
only_states_since c "$c_point" "^configure $c_size activated=[01] suspended=0 " ||
	fail "a bar or fullscreen on one monitor changed a window on the other"
press super+r
start_client f
expect_state f "$on" "the new column to be focused"
expect_line f "output enter $left" "the new column to be drawn on its monitor"
press super+j
expect_state c "$on" "Mod+J to scroll back to the first column"
expect_state f "$visible" "the second column to stay partly in view"
sleep 0.6 # nothing marks the end of the scroll; let it finish
never_on f $right
never_on c $right
stop_tw

# --- Across the end of the strip, and with the mouse ------------------------------
# A fresh start, so that where each column is is known: no gap, each column
# half a monitor wide (640 pixels), the first at the monitor's left edge.
# entered_since <name> <point> <monitor>: the window came onto that monitor.
entered_since() { since "$1" "$2" | grep -qx "output enter $3"; }
# drag <from x,y> <to x,y>: Mod and the left button, from one place to another.
drag() {
	press "move:$1:$screen_size" hold:super down "move:$2:$screen_size" up hold:
}
start_tw
start_client m1
start_client m2
press super+shift+l
start_client m3
# left: m1 m2 [focus m2]   right: m3
expect_state m3 "$on" "a window to open on the second monitor"

# Mod and a direction go on to the next monitor at either end of the strip.
press super+shift+j
expect_state m2 "$on" "the focus to be on the last column of the first monitor"
press super+l
expect_state m3 "$on" "Mod+L at the end of the strip to go on to the monitor on the right"
press super+j
expect_state m2 "$on" "Mod+J at the start of the strip to go back to the monitor on the left"

# Mod+Ctrl and a direction take the column along, to the near end of the
# strip there; the other way brings it back.
point=$(lines m2)
press super+ctrl+l
wait_until "Mod+Ctrl+L at the end of the strip to move the column to the next monitor" \
	entered_since m2 "$point" $right
expect_state m2 "$on" "the focus to go along with the column"
press super+l
expect_state m3 "$on" "the moved column to come in at the near end, left of the one there"
press super+j super+j
expect_state m1 "$on" "Mod+J from the start of the strip to cross back to the left monitor"
press super+l
expect_state m2 "$on" "Mod+L from the end of the strip to cross to the right monitor again"
point=$(lines m2)
press super+ctrl+j
wait_until "Mod+Ctrl+J at the start of the strip to bring the column back" \
	entered_since m2 "$point" $left
expect_state m2 "$on" "the focus to come back with it"
# left: m1 m2 [focus m2]   right: m3

# Mod and the left button carry a window to another monitor. Let go over an
# empty part of it, the window goes after the column focused there.
point=$(lines m1)
drag 300,300 2200,300
wait_until "a window dragged with Mod onto the other monitor to go there" \
	entered_since m1 "$point" $right
expect_state m1 "$on" "the dragged window to be focused"
since m1 "$point" | grep -q '^button' && fail "the window heard the button that moved it"
press super+j
expect_state m3 "$on" "the dragged window to have gone after the column focused there"
# right: m3 m1 [focus m3], m1 at 1920-2560

# Let go over a column, it goes beside it, on the side of its middle the
# pointer is on.
drag 2200,300 1350,300
expect_state m1 "$on" "the dragged window to be focused again"
press super+l
expect_state m3 "$on" "a window let go over the left half of another to go before it"
press super+j
expect_state m1 "$on" "the focus back on the moved window"
# right: m1 m3 [focus m1]

# Let go where it was, nothing moves.
point=$(lines m3)
drag 1400,300 1500,300
sleep 0.2
since m3 "$point" | grep -q '^configure' &&
	fail "a window dropped on itself disturbed the column next to it"

# A plain drag still goes to the window.
# left: m2   right: m1 m3
point=$(lines m2)
press "move:300,300:$screen_size" down "move:350,320:$screen_size" up
wait_until "a plain drag to reach the window" eval "since m2 $point | grep -q '^button'"
since m2 "$point" | grep -q "^output enter $right" && fail "a plain drag moved the window"

# Locked, Mod and the button move nothing.
start_client locker --lock
expect_line locker locked "the session to lock"
point=$(lines m2)
drag 300,300 2200,300
press Return
expect_line locker unlocked "the session to unlock"
entered_since m2 "$point" $right && fail "a window moved with the mouse while locked"

stop_tw
echo "ok: two monitors, focus and columns between them and across the ends of the strip, workspaces, mouse, moving with the mouse, a bar, fullscreen, trimming"
