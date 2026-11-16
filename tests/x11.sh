#!/bin/sh
# X11 support. tw owns the X11 display sockets and starts xwayland-satellite
# only when an X11 client connects; until then X11 support costs no process.
#
# A stand-in plays xwayland-satellite (tests/client.c --x11-serve), so this
# checks tw's side of the hand-over: the display it announces, the arguments
# and sockets it passes, what it does when the helper dies or never starts,
# and that it stops a helper no X11 client is connected to. The real helper
# and Xwayland are not involved.
set -eu
. "$(dirname "$0")/lib.sh"

# What tw's children see as DISPLAY, written by the startup command.
startup='echo "${DISPLAY-none}" >"$XDG_RUNTIME_DIR/display"'
# count <file> <pattern>: matching lines; 0 if the file does not exist yet.
count() {
	if [ -e "$1" ]; then grep -c "$2" "$1" || true; else echo 0; fi
}
helpers() { [ "$(count "$run_dir/helper.log" '^serve')" = "$1" ]; }  # helpers that ran
attempts() { [ "$(count "$run_dir/attempts" .)" = "$1" ]; }          # times tw started one
deaths() { [ "$(count "$run_dir/tw.log" 'was killed by signal')" = "$1" ]; }
gone() { ! kill -0 "$1" 2>/dev/null; }
hung_up() { [ "$(count "$run_dir/helper.log" '^hung up')" = "$1" ]; }  # clients that left
# A window opens and closes: the moment tw looks whether X11 is still in use.
window_closes() {
	start_client "$1"
	expect_keyboard "$1" enter "window $1 to be shown"
	kill "$last_client_pid"
}
helper_pid() { sed -n 's/^serve pid=\([0-9]*\) .*/\1/p' "$run_dir/helper.log" | tail -n 1; }

# --- Helper not installed: no X11, and no DISPLAY that leads nowhere ----------
start_tw -s "$startup"
wait_until "the startup command to run" test -s "$run_dir/display"
[ "$(cat "$run_dir/display")" = none ] ||
	fail "DISPLAY is set although xwayland-satellite is not installed"
stop_tw
rm "$run_dir/display"

# --- Installed -----------------------------------------------------------------
fake_program xwayland-satellite "
echo attempt >>'$run_dir/attempts'
[ -e '$run_dir/broken' ] && exit 1
[ -e '$run_dir/stranger' ] && set -- \"\$@\" -stranger
exec '$client_bin' --x11-serve \"\$@\" >>'$run_dir/helper.log' 2>&1"

# -x 2: look two seconds after the helper started or a window closed.
start_tw -x 2 -s "$startup"
wait_until "the startup command to run" test -s "$run_dir/display"
display=$(cat "$run_dir/display")
number=${display#:}
case $display in
	:[0-9]*) ;;
	*) fail "children got DISPLAY=$display, not an X11 display" ;;
esac
[ -S "/tmp/.X11-unix/X$number" ] || fail "no socket for display $display"
[ "$(tr -d ' \n' <"/tmp/.X$number-lock")" = "$tw_pid" ] ||
	fail "the lock file of display $display does not name tw"
attempts 0 || fail "the helper was started before any X11 client connected"

# The first client starts the helper, which gets the display and both sockets.
spawn_client x1 --x11 path "$display"
expect_line x1 "x11 served" "the first X11 client to be answered"
grep -Eqx "serve pid=[0-9]+ $display -listenfd [0-9]+ -listenfd [0-9]+" "$run_dir/helper.log" ||
	fail "unexpected helper arguments: $(cat "$run_dir/helper.log")"
# tw ignores SIGPIPE for itself (clipboard.c). The helper must not start
# that way, nor Xwayland and the X11 programs under it.
if grep -qx 'ignores SIGPIPE' "$run_dir/helper.log"; then
	fail "the helper was started ignoring SIGPIPE"
fi

# Later clients, by either kind of socket, reach the same helper.
spawn_client x2 --x11 abstract "$display"
expect_line x2 "x11 served" "a client on the abstract socket to be answered"
spawn_client x3 --x11 path "$display"
expect_line x3 "x11 served" "a third client to be answered"
helpers 1 || fail "more than one helper was started"

# --- The helper dies: its clients go with it, the next one starts another ------
kill "$(helper_pid)"
expect_line x1 "x11 closed" "X11 clients to lose their connection with the helper"
wait_until "tw to notice that the helper is gone" deaths 1
spawn_client x4 --x11 path "$display"
expect_line x4 "x11 served" "a new client to be answered after the helper died"
wait_until "a second helper to be started" helpers 2

# --- A helper that fails to start: the client is turned away, once -------------
touch "$run_dir/broken"
kill "$(helper_pid)"
wait_until "tw to notice that the helper is gone" deaths 2
before=$(count "$run_dir/attempts" .)
spawn_client x5 --x11 path "$display"
expect_line x5 "x11 closed" "a client to be turned away when the helper cannot start"
sleep 0.5
attempts $((before + 1)) || fail "tw kept restarting a helper that fails to start"

# --- The helper uninstalled while tw runs: turned away, with a reason ---------
mv "$bin_dir/xwayland-satellite" "$run_dir/uninstalled"
spawn_client x5b --x11 abstract "$display"
expect_line x5b "x11 closed" "a client to be turned away when the helper is gone"
grep -q 'xwayland-satellite is no longer installed' "$run_dir/tw.log" ||
	fail "tw did not say that the helper is no longer installed"
attempts $((before + 1)) || fail "tw ran a helper that is no longer installed"
mv "$run_dir/uninstalled" "$bin_dir/xwayland-satellite"

rm "$run_dir/broken"
spawn_client x6 --x11 path "$display"
x6_pid=$last_client_pid
expect_line x6 "x11 served" "X11 to work again once the helper does"

# --- No X11 client left: the helper is stopped, the next client starts one -----
# While one is connected the helper stays, whatever windows come and go.
helper=$(helper_pid)
window_closes w1
sleep 2.6
gone "$helper" && fail "the helper was stopped although an X11 client was connected"

kill "$x6_pid"
wait_until "the helper to notice that its client left" hung_up 1
window_closes w2
wait_until "the unused helper to be stopped" gone "$helper"
deaths 2 || fail "stopping an unused helper was logged as the helper dying"

# A program that only asks X11 a question: nothing closes a window, so the
# look after the helper's start is what catches it.
before=$(count "$run_dir/helper.log" '^serve')
spawn_client x7 --x11 abstract "$display"
expect_line x7 "x11 served" "a client to be answered after the helper was stopped"
helpers $((before + 1)) || fail "expected one new helper, got: $(cat "$run_dir/helper.log")"
helper=$(helper_pid)
kill "$last_client_pid"
wait_until "a helper nobody uses any more to be stopped" gone "$helper"

# A connection tw cannot place counts as a client. One from a sandbox with a
# network of its own looks like that: it is in no list tw can read.
touch "$run_dir/stranger"
spawn_client x8 --x11 path "$display"
expect_line x8 "x11 served" "X11 to work after the helper was stopped twice"
last_helper=$(helper_pid)
kill "$last_client_pid"
sleep 2.6
gone "$last_helper" && fail "the helper was stopped although it had a connection tw cannot place"

# --- Exit: nothing is left behind ----------------------------------------------
stop_tw
[ ! -e "/tmp/.X11-unix/X$number" ] || fail "the X11 socket was left behind"
[ ! -e "/tmp/.X$number-lock" ] || fail "the X11 lock file was left behind"
wait_until "the helper to be stopped with tw" gone "$last_helper"

# --- tw killed: the next tw takes over the display it left behind --------------
rm -f "$run_dir/display"
start_tw -s "$startup"
wait_until "the startup command to run" test -s "$run_dir/display"
display=$(cat "$run_dir/display")
number=${display#:}
kill -KILL "$tw_pid"
{ wait "$tw_pid"; } 2>/dev/null || true
tw_pid=
[ -S "/tmp/.X11-unix/X$number" ] && [ -e "/tmp/.X$number-lock" ] ||
	fail "a killed tw left no X11 socket and lock behind to take over"
rm -f "$run_dir/display" "$run_dir/wayland-0" "$run_dir/wayland-0.lock"
start_tw -s "$startup"
wait_until "the startup command to run" test -s "$run_dir/display"
[ "$(cat "$run_dir/display")" = "$display" ] ||
	fail "tw took $(cat "$run_dir/display"), not $display that a killed tw left behind"
[ "$(tr -d ' \n' <"/tmp/.X$number-lock")" = "$tw_pid" ] ||
	fail "the lock file a killed tw left behind was not taken over"
stop_tw
[ ! -e "/tmp/.X11-unix/X$number" ] && [ ! -e "/tmp/.X$number-lock" ] ||
	fail "the X11 socket or lock file was left behind after a takeover"

echo "ok: X11 display $display, helper on demand, restart, failure, uninstalled, stopped when unused, cleanup, takeover after a crash"
