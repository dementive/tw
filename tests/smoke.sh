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

# --- Options -------------------------------------------------------------------
# A timeout that is not a number of seconds is refused. Read as 0 it would
# mean "never", and a typo would switch the screen blanking off unnoticed.
"$tw_bin" -h | grep -q -- '-h  show this help' || fail "tw -h does not list -h"
for bad in '-b five' '-z 1h' '-x -3' '-b 2.5' '-b'; do
	# shellcheck disable=SC2086 # two words on purpose
	if "$tw_bin" $bad >/dev/null 2>"$run_dir/options.log"; then
		fail "tw took '$bad' for an option"
	fi
done
if "$tw_bin" -z soon 2>&1 | grep -q "takes a number of seconds, not 'soon'"; then :; else
	fail "tw did not say what is wrong with '-z soon'"
fi
rm -f "$run_dir/options.log"

# A start that cannot work says why and ends with status 1: not with a
# crash on the way out, where only some of what a running tw has exists.
status=0
env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DISPLAY PATH="$bin_dir" \
	XDG_RUNTIME_DIR="$run_dir" WLR_BACKENDS=no-such-backend \
	"$tw_bin" >"$run_dir/start.log" 2>&1 || status=$?
[ "$status" = 1 ] || fail "a tw without a backend exited with status $status, not 1"
grep -q 'failed to create the backend' "$run_dir/start.log" ||
	fail "tw did not say why it could not start"
if grep -q 'Sanitizer\|runtime error' "$run_dir/start.log"; then
	fail "sanitizer report from a start that failed"
fi
rm -f "$run_dir/start.log"

# --- Autostart -----------------------------------------------------------------
# config.h's autostart list starts foot's terminal server and nothing else.
# These two are installed and must not be started. D-Bus starts a
# notification service when a notification arrives, and tw keeps what was
# copied itself (tests/clipboard.sh).
fake_program fnott "echo fnott >>'$run_dir/autostart.log'"
fake_program wl-clip-persist "echo \"wl-clip-persist \$*\" >>'$run_dir/autostart.log'"

# Stand-ins for the launcher and the terminal, to see their keys start them.
fake_program tw-launch "echo launched >>'$run_dir/launched.log'"
fake_program tw-control "echo control >>'$run_dir/launched.log'"
launched() { [ -e "$run_dir/launched.log" ] && [ "$(grep -c . "$run_dir/launched.log")" = "$1" ]; }
fake_program foot "[ ! -e /proc/\$\$/fd/9 ] || echo 'fd 9' >>'$run_dir/leaked.log'
case \$1 in
--server) echo 'foot --server' >>'$run_dir/autostart.log' ;;
*) echo opened >>'$run_dir/terminal.log' ;;
esac"
# foot's terminal server: answers unless told there is none.
fake_program footclient "[ ! -e /proc/\$\$/fd/9 ] || echo 'fd 9' >>'$run_dir/leaked.log'
[ ! -e '$run_dir/no-server' ] || exit 1
echo \"client \$*\" >>'$run_dir/terminal.log'"
# And for what the laptop keys run: each changes a level and shows it. The
# text tools they read the level with are the real ones.
ln -s "$(command -v awk)" "$(command -v cut)" "$bin_dir/"
fake_program wpctl "case \$1 in get-volume) echo 'Volume: 0.45 [MUTED]';; esac"
fake_program brightnessctl "echo \"brightnessctl \$*\" >>'$run_dir/levels.log'
case \"\$*\" in *info*) echo 'asus::kbd_backlight,leds,1,33%,3';; esac"
fake_program notify-send "echo \"notify-send \$*\" >>'$run_dir/levels.log'"
levels() { grep -qxF -- "$1" "$run_dir/levels.log" 2>/dev/null; }

# A file tw was handed open is not handed on to the programs it starts
# (a GPU device left open in one would keep that GPU on).
exec 9>"$run_dir/inherited"
start_tw
exec 9>&-

press super+space
wait_until "Mod+Space to start the launcher" launched 1
press super+d
wait_until "Mod+D to start the launcher" launched 2
press super+Escape
wait_until "Mod+Escape to start tw-control" grep -qx control "$run_dir/launched.log"
press super+t
wait_until "Mod+T to open a terminal through foot's server" grep -qx 'client -E -N' "$run_dir/terminal.log"
! grep -qx opened "$run_dir/terminal.log" || fail "Mod+T started a foot of its own while the server answered"
touch "$run_dir/no-server"
press super+t
wait_until "Mod+T with no terminal server to start a foot of its own" grep -qx opened "$run_dir/terminal.log"
[ ! -e "$run_dir/leaked.log" ] || fail "a program tw started inherited a file tw was given"
press XF86KbdBrightnessUp
wait_until "the keyboard-light key to raise it" levels 'brightnessctl -q --device=*::kbd_backlight set 1+'
wait_until "the new level to show" levels 'notify-send --app-name=tw --urgency=low --expire-time=1500 --hint=string:x-canonical-private-synchronous:keyboard-light Keyboard light 33%'
press XF86KbdBrightnessDown
wait_until "the keyboard-light key to lower it" levels 'brightnessctl -q --device=*::kbd_backlight set 1-'
press XF86AudioRaiseVolume
wait_until "the volume to show" levels 'notify-send --app-name=tw --urgency=low --expire-time=1500 --hint=string:x-canonical-private-synchronous:volume Volume 45%, muted'
# By now tw has long been through its startup: it started what the keys
# asked for, and by itself only the terminal server.
[ "$(cat "$run_dir/autostart.log" 2>/dev/null)" = "foot --server" ] ||
	fail "tw started with the session: $(cat "$run_dir/autostart.log" 2>/dev/null)"

# --- Columns -------------------------------------------------------------------
start_client a
expect_state a "configure [1-9][0-9]*x[1-9][0-9]* $on" "the first window to be sized and focused"
expect_keyboard a enter "the first window to get the keyboard"
# tw offers fullscreen and nothing else: told of maximize, minimize or a
# window menu, a program that draws its own title bar shows buttons for them
# that do nothing.
expect_line a "capabilities fullscreen" "windows to be offered fullscreen alone"

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
press super+j
expect_state a "$on" "Mod+J to focus the previous column"
press super+Right super+Right
expect_state c "$on" "Mod+Right to work like Mod+L"
press super+Left
expect_state b "$on" "Mod+Left to work like Mod+J"
press super+End super+j
expect_state d "$on" "Mod+End then Mod+J to focus the fourth column"

# Moving a column keeps it focused: d goes first, so Mod+L then reaches a.
press super+ctrl+Home super+l
expect_state a "$on" "Mod+Ctrl+Home to move the column to the front"
# strip is d [a] b c e

press super+r
resized() { [ "$(current_size a)" != "$size" ]; }
wait_until "Mod+R to change the column width" resized
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

# Shift with a direction goes through the workspaces: up and left to the
# previous one, down and right to the next. (With a monitor on that side it
# goes there instead; see outputs.sh.)
for keys in "i k" "j l" "Up Down" "Left Right"; do
	back=${keys% *}
	forth=${keys#* }
	press "super+shift+$back"
	expect_state b "$on" "Mod+Shift+$back to go to the previous workspace"
	expect_state d "$off_screen" "the workspace left behind to be hidden"
	press "super+shift+$forth"
	expect_state d "$on" "Mod+Shift+$forth to go to the next workspace"
done
# With Ctrl as well, the focused column comes along.
press super+shift+ctrl+i
expect_state d "$on" "Mod+Shift+Ctrl+I to take the column to the previous workspace"
expect_state b "activated=0 suspended=0" "that workspace to be the one on screen"
expect_state f "$off_screen" "the workspace the column left to be hidden"
press super+shift+ctrl+Down
expect_state d "$on" "Mod+Shift+Ctrl+Down to take the column to the next workspace"
expect_state f "activated=0 suspended=0" "workspace 2 to be shown again"

# --- Focus requests: workspace 5 -----------------------------------------------
# A window may ask for the focus with a token that another program made from
# an input event (a clicked link, a clicked notification). Here `giver` turns
# its keyboard focus into a token; `asker` uses it when it loses the focus.
press super+5
start_client giver --token "$run_dir/token"
expect_line giver token "a focused window to be given an activation token"
start_client asker --activate "$run_dir/token"
expect_state asker "$on" "the asking window to start focused"
press super+6
expect_line asker activate "the window to ask for the focus when it loses it"
expect_state asker "$on" "a focus request to bring the window and its workspace back"
expect_keyboard asker enter "the window that asked to get the keyboard"

# A token made without any input event proves nothing and moves nothing.
start_client thief --activate-forged
expect_state thief "$on" "the third window to start focused"
press super+6
expect_line thief "activate forged" "the window to ask with a made-up token"
sleep 0.3
last_line_matches thief configure "$off_screen" || fail "a made-up token stole the focus"

# Nor does a token from a real input event that does not say which surface
# it came to: any event the client was ever sent would do, such as the
# pointer merely passing over it.
start_client sly --token-no-surface "$run_dir/token2"
expect_line sly token "a token without a surface to be issued"
start_client asker2 --activate "$run_dir/token2"
expect_state asker2 "$on" "the window with that token to start focused"
press super+7
expect_line asker2 activate "the window to ask for the focus with it"
sleep 0.3
last_line_matches asker2 configure "$off_screen" || fail "a token naming no surface stole the focus"
press super+2
expect_state d "$on" "Mod+2 to return to workspace 2"

# --- Layer shell: a launcher borrows the keyboard and gives it back ------------
start_client launcher --layer
grep -q '^layer configure 400x200$' "$run_dir/launcher.log" ||
	fail "layer surface was not given the size it asked for"
# It hears the screen's scale before it is asked to draw. If it heard only
# once its first picture is on screen, it would have drawn that for scale 1
# and would draw again: twice for every launcher on a screen at 1.25.
[ "$(grep '^scale \|^layer configure' "$run_dir/launcher.log" | head -n 1)" = "scale 120" ] ||
	fail "a layer surface was asked to draw before it was told the scale"
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

# A second layer surface coming and going changes nothing for the first,
# which is told nothing: a configure would wake it for no reason.
start_client launcher2 --layer
launcher2_pid=$last_client_pid
expect_line launcher2 "layer configure 400x200" "the second layer surface to be configured"
expect_keyboard launcher2 enter "the newest launcher to take the keyboard"
kill "$launcher2_pid"
expect_keyboard launcher enter "the keyboard to go back to the first launcher"
[ "$(grep -c '^layer configure' "$run_dir/launcher.log")" = 1 ] ||
	fail "a layer surface was configured again although nothing about it changed"

kill "$launcher_pid"
expect_keyboard d enter "the keyboard to return to the window"

# --- Fullscreen: workspace 2 is f [d] ------------------------------------------
press super+shift+f
expect_state d "$on fullscreen=1" "Mod+Shift+F to make the window fullscreen"
full=$(current_size d)
# The whole 1280x720 output. Without gaps a maximized column is as large, so
# what tells them apart is the fullscreen state, checked above.
[ "$full" = 1280x720 ] || fail "fullscreen is $full, not the whole output"
[ "${full%x*}" -ge "${maximized%x*}" ] ||
	fail "fullscreen ($full) is narrower than a maximized column ($maximized)"
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

# Every binding above that moved the focus did so with its key still down.
# The window that got the focus is not told that key is held: its release
# never reaches it, and it would be stuck down.
if grep -l 'keys held' "$run_dir"/*.log >/dev/null 2>&1; then
	fail "a window was told a binding's key is held as it got the focus"
fi

stop_tw
echo "ok: autostart, columns $size, bindings, workspaces, focus requests, layer shell, fullscreen $full, lock, clean exit"
