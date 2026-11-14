# Sourced by the headless tests; expects $1 = tw binary, $2 = test client.
#
# Starts tw on wlroots' headless backend with the software renderer, inside a
# private XDG_RUNTIME_DIR. The tests therefore need no GPU and no seat, and
# can never touch the session they are launched from.

tw_bin=$1
client_bin=$2
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
	if [ -s "$run_dir/tw.log" ]; then
		sed 's/^/  tw: /' "$run_dir/tw.log" >&2
	fi
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

start_tw() {
	env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DISPLAY \
		XDG_RUNTIME_DIR="$run_dir" \
		WLR_BACKENDS=headless \
		WLR_RENDERER=pixman \
		"$tw_bin" >"$run_dir/tw.log" 2>&1 &
	tw_pid=$!
	wait_until "the Wayland socket" test -S "$run_dir/wayland-0"
}

# start_client <name>: output goes to $run_dir/<name>.log.
start_client() {
	XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 \
		"$client_bin" >"$run_dir/$1.log" 2>&1 &
	client_pids="$client_pids $!"
	wait_until "client $1 to be configured" grep -q '^configure' "$run_dir/$1.log"
}

# expect <name> <regex> <description>: wait for a matching line from a client.
expect() {
	wait_until "$3" grep -Eq "$2" "$run_dir/$1.log"
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
