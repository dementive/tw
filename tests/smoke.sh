#!/bin/sh
# End-to-end check of everything a client can observe: column sizing, focus,
# key bindings, workspaces, layer surfaces, fullscreen, the screen lock, and a
# clean shutdown. In a
# sanitizer build a use-after-free, undefined behaviour or a leak fails it.
#
# $3 = tw-test-keys. Windows are named a..f; the comments track the strip.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3

on='activated=1 suspended=0'      # focused and visible
off_screen='activated=0 suspended=1'

start_tw

# --- Columns -------------------------------------------------------------------
start_client a
expect_state a "configure [1-9][0-9]*x[1-9][0-9]* $on" "the first window to be sized and focused"
expect_keyboard a enter "the first window to get the keyboard"

# Each new window opens to the right and takes focus. Five columns never fit
# on one output with the stock column widths, so the first one scrolls off.
for name in b c d e; do
	start_client "$name"
done
expect_state e "$on" "the last window to be focused"
expect_state a "$off_screen" "the first window to be suspended off-screen"
expect_keyboard a leave "the first window to lose the keyboard"

# New columns must not resize existing ones: every window got the same size.
size=$(current_size a)
for name in b c d e; do
	[ "$(current_size "$name")" = "$size" ] || fail "window $name is not $size"
done

# --- Key bindings: strip is a b c d [e] ----------------------------------------
press super+Home
expect_state a "$on" "Mod+Home to focus the first column"
expect_state e "activated=0" "the last column to lose focus"
press super+l
expect_state b "$on" "Mod+L to focus the next column"
press super+End super+h
expect_state d "$on" "Mod+End then Mod+H to focus the fourth column"

# Moving a column keeps it focused: d goes first, so Mod+L then reaches a.
press super+ctrl+Home super+l
expect_state a "$on" "Mod+Ctrl+Home to move the column to the front"
# strip is d [a] b c e

press super+r
wait_until "Mod+R to change the column width" test "$(current_size a)" != "$size"
press super+f
maximized=$(current_size a)
[ "${maximized%x*}" -gt "${size%x*}" ] || fail "Mod+F did not widen the column: $maximized"
press super+f

# A key that is bound never reaches the client; an unbound one does. Exactly
# two key lines (x pressed, x released) means no binding leaked a key event.
key_lines() { [ "$(grep -c '^key ' "$run_dir/$1.log")" = "$2" ]; }
press x
wait_until "only the unbound key to reach the focused window" key_lines a 2

press super+q
wait_until "Mod+Q to close the focused window" grep -q "^closed" "$run_dir/a.log"
expect_state d "$on" "focus to move to the left neighbour of a closed window"
# strip is [d] b c e

# --- Workspaces ----------------------------------------------------------------
press super+2
expect_state d "$off_screen" "windows to be suspended when their workspace is hidden"
start_client f
expect_state f "$on" "a window opened on workspace 2 to be focused"

press super+1
expect_state d "$on" "Mod+1 to bring workspace 1 back with its focus"
expect_state f "$off_screen" "workspace 2 to be hidden"

# Sending a column to another workspace takes the view along.
press super+ctrl+2
expect_state d "$on" "the moved column to stay focused"
expect_state f "activated=0 suspended=0" "workspace 2 to be shown again"
expect_state b "suspended=1" "workspace 1 to be hidden"

press super+Page_Up
expect_state b "$on" "Mod+Page_Up to go to the workspace above"
press super+Page_Down
expect_state d "$on" "Mod+Page_Down to go back down"

# --- Layer shell: a launcher borrows the keyboard and gives it back ------------
start_client launcher --layer
grep -q '^layer configure 400x200$' "$run_dir/launcher.log" ||
	fail "layer surface was not given the size it asked for"
launcher_pid=$last_client_pid
expect_keyboard launcher enter "the launcher to take the keyboard"
expect_keyboard d leave "the window to lose the keyboard"
grep '^configure' "$run_dir/d.log" | tail -n 1 | grep -q "$on" ||
	fail "the window should stay drawn as active under a launcher"

press x
wait_until "keys to go to the launcher" grep -q '^key ' "$run_dir/launcher.log"
if grep -q '^key ' "$run_dir/d.log"; then
	fail "a key leaked to the window behind the launcher"
fi

kill "$launcher_pid"
expect_keyboard d enter "the keyboard to return to the window"

# --- Fullscreen: workspace 2 is f [d] ------------------------------------------
press super+shift+f
expect_state d "$on fullscreen=1" "Mod+Shift+F to make the window fullscreen"
full=$(current_size d)
[ "${full%x*}" -gt "${maximized%x*}" ] ||
	fail "fullscreen ($full) is not wider than a maximized column ($maximized)"
expect_state f "suspended=1" "the neighbour of a fullscreen window to be hidden"
press super+shift+f
expect_state d "$on fullscreen=0" "Mod+Shift+F again to leave fullscreen"
[ "$(current_size d)" = "$size" ] || fail "leaving fullscreen did not restore the column size"

# A window that asks to start fullscreen is never given a column size first.
start_client g --fullscreen
expect_state g "$on fullscreen=1" "a window that asks for fullscreen to get it"
sizes=$(sed -n 's/^configure \([0-9x]*\) .*/\1/p' "$run_dir/g.log" | sort -u)
[ "$sizes" = "$full" ] || fail "fullscreen-from-start window was sized '$sizes', not $full"
# workspace 2 is f d [g]

# --- Screen lock ---------------------------------------------------------------
start_client locker --lock
locker_pid=$last_client_pid
expect_line locker locked "the session to lock"
expect_keyboard locker enter "the lock screen to take the keyboard"
expect_keyboard g leave "the window to lose the keyboard"
expect_state g "suspended=1" "windows to be suspended behind the lock screen"

# No bindings while locked: Mod+Q arrives at the lock screen as a plain key.
press super+q
wait_until "keys to go to the lock screen" key_lines locker 2
if grep -q '^closed' "$run_dir/g.log"; then
	fail "a key binding ran while the session was locked"
fi

spawn_client intruder --lock
expect_line intruder "lock denied" "a second lock client to be refused"

# If the locker dies the session stays locked: a new window stays hidden and
# gets no keyboard.
kill "$locker_pid"
start_client h
expect_state h "suspended=1" "a window opened while locked to stay hidden"
if grep -q '^keyboard enter' "$run_dir/h.log"; then
	fail "a window got the keyboard while the session was locked"
fi

# A new locker can take over the abandoned lock, and unlock.
start_client locker2 --lock
expect_line locker2 locked "a new lock client to take over"
press Return
expect_line locker2 unlocked "the lock client to unlock"
expect_keyboard h enter "the keyboard to return to the focused window"
expect_state h "$on" "windows to come back after unlocking"

stop_tw
echo "ok: columns $size, bindings, workspaces, layer shell, fullscreen $full, lock, clean exit"
