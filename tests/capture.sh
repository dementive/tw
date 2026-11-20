#!/bin/sh
# The window list and window capture, as a screen recorder uses them (OBS
# through xdg-desktop-portal-wlr): tests/capture.c lists the windows and
# records one. Recording whole outputs is wlroots' own and not covered here.
#
# $3 = tw-test-capture.
set -eu
. "$(dirname "$0")/lib.sh"
capture_bin=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")

# capture <name> <list|record>: says what it sees in $run_dir/<name>.log; its
# pid is in $capture_pid.
capture() {
	XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 \
		"$capture_bin" "$2" >"$run_dir/$1.log" 2>&1 &
	capture_pid=$!
	client_pids="$client_pids $capture_pid"
}
quit() {
	kill "$1" 2>/dev/null || true
	wait "$1" 2>/dev/null || true
}
count() { grep -c "$2" "$run_dir/$1.log" || true; } # <name> <regex>

start_tw

# The list has every window, from when it maps to when it goes.
start_client one
one=$last_client_pid
capture list list
expect_line list "window - tw-test-client" "the window to be listed"
start_client two
two=$last_client_pid
two_listed() { [ "$(count list '^window ')" -eq 2 ]; }
wait_until "the second window to be listed" two_listed
quit "$two"
expect_line list "closed tw-test-client" "the closed window to leave the list"

# A recording of the window shows it, at its size.
capture rec record
rec=$capture_pid
size=$(current_size one)
expect_line rec "frame $size 336699" "a frame of the window, $size"

# Scrolled off screen by two more windows, a window that is being recorded
# is not suspended: the recording would stop moving. Once nobody records it,
# it is.
start_client two
start_client three
expect_state three "activated=1" "the third window to have the focus"
sleep 0.5
last_line_matches one configure "suspended=0" ||
	fail "a window being recorded was suspended off screen"
quit "$rec"
expect_state one "suspended=1" "the window to be suspended once nobody records it"

# A recording ends when its window goes. The first window listed, which is
# the one recorded, is the oldest.
capture rec2 record
wait_until "a recording to start" grep -q '^frame ' "$run_dir/rec2.log"
quit "$one"
expect_line rec2 "stopped" "the recording to stop as its window goes"
stop_tw

echo "ok: the window list, recording a window, off screen and as it goes"
