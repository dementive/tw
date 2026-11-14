# Sourced by the headless tests; expects $1 = tw binary, $2 = test client.
# Tests that press keys set keys_bin to the tw-test-keys binary.
#
# Starts tw on wlroots' headless backend with the software renderer, inside a
# private XDG_RUNTIME_DIR. The tests therefore need no GPU and no seat, and
# can never touch the session they are launched from. With no host session in
# the environment, tw uses Super as its modifier.

tw_bin=$1
client_bin=$2
keys_bin=
# XDG_RUNTIME_DIR must be absolute; Meson passes the binaries as ./tw.
run_dir=$(mktemp -d "$(cd "$(dirname "$tw_bin")" && pwd)/test.XXXXXX")
tw_pid=
client_pids=

cleanup() {
	for pid in $client_pids $tw_pid; do
		kill "$pid" 2>/dev/null || true
	done
	wait 2>/dev/null || true
	rm -rf "$run_dir"
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	for log in "$run_dir"/*.log; do
		[ -s "$log" ] && sed "s/^/  $(basename "$log" .log): /" "$log" >&2
	done
	exit 1
}

# wait_until <description> <command...>: poll for up to 5 seconds.
wait_until() {
	description=$1
	shift
	tries=0
	while ! "$@" >/dev/null 2>&1; do
		tries=$((tries + 1))
		[ "$tries" -le 100 ] || fail "timed out waiting for $description"
		sleep 0.05
	done
}

# start_tw [tw arguments]
start_tw() {
	env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DISPLAY \
		XDG_RUNTIME_DIR="$run_dir" \
		WLR_BACKENDS=headless \
		WLR_RENDERER=pixman \
		"$tw_bin" "$@" >"$run_dir/tw.log" 2>&1 &
	tw_pid=$!
	wait_until "the Wayland socket" test -S "$run_dir/wayland-0"
}

# spawn_client <name> [client mode]: output goes to $run_dir/<name>.log.
# Sets last_client_pid. See tests/client.c for the modes.
spawn_client() {
	name=$1
	shift
	XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 \
		"$client_bin" "$@" >"$run_dir/$name.log" 2>&1 &
	last_client_pid=$!
	client_pids="$client_pids $last_client_pid"
}

# start_client <name> [client mode]: spawn_client, then wait for its surface
# to be configured.
start_client() {
	spawn_client "$@"
	wait_until "client $1 to be configured" grep -q 'configure' "$run_dir/$1.log"
}

# expect_line <name> <exact line> <description>: the client printed this line.
expect_line() {
	wait_until "$3" grep -qx "$2" "$run_dir/$1.log"
}

# press <combo...>: e.g. `press super+ctrl+l super+2`. See tests/keys.c.
press() {
	XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 "$keys_bin" "$@" ||
		fail "could not press: $*"
}

# The checks below look at a client's most recent line of one kind, because
# what matters is the state it is in now, not one it passed through.
last_line_matches() { # <name> <line prefix> <regex>
	grep "^$2" "$run_dir/$1.log" | tail -n 1 | grep -Eq "$3"
}

# expect_state <name> <regex> <description>: the latest window state.
expect_state() {
	wait_until "$3" last_line_matches "$1" configure "$2"
}

# expect_keyboard <name> <enter|leave> <description>: the latest focus event.
expect_keyboard() {
	wait_until "$3" last_line_matches "$1" keyboard "keyboard $2"
}

# Size from a client's latest window state, as <width>x<height>.
current_size() {
	grep '^configure' "$run_dir/$1.log" | tail -n 1 | sed 's/^configure \([0-9x]*\) .*/\1/'
}

# Disconnect every client, then ask tw to exit and require a clean exit.
stop_tw() {
	for pid in $client_pids; do
		kill "$pid" 2>/dev/null || true
	done
	client_pids=
	kill -TERM "$tw_pid"
	wait "$tw_pid" || fail "tw exited with status $?"
	tw_pid=
	if grep -q 'Sanitizer\|runtime error' "$run_dir/tw.log"; then
		fail "sanitizer report"
	fi
}
