#!/bin/sh
# Setting the screens from a program (wlr-output-management) and tw-monitors:
# what it lists, what tw takes and refuses, screens turned off and on, what
# was kept put back as tw starts, and the box changing, keeping and going
# back. Two headless screens, as in outputs.sh: HEADLESS-2 on the left with
# the focus, HEADLESS-1 on the right. Headless screens have one mode and no
# make or model, so modes and names are not covered here.
#
# $3 = tw-test-keys, $4 = tw-monitors.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3
monitors_bin=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
export WLR_HEADLESS_OUTPUTS=2
screen_size=2560,720

config=$run_dir/config
kept=$config/tw-monitors
mkdir -p "$config"
# monitors <arguments>: tw-monitors with the test's settings file.
monitors() {
	env XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME="$config" \
		"$monitors_bin" "$@"
}
listed() { monitors --list | grep -qx "$(printf '%s' "$1" | sed 's/ /\t/g')"; } # <line, tabs as spaces>
keep() { printf '%s\n' "$@" | sed 's/ /\t/g' >"$kept"; } # <line, tabs as spaces>...

# tw puts back what was kept for these screens as it starts, through
# tw-monitors --apply on its PATH.
fake_program tw-monitors "exec /usr/bin/env XDG_CONFIG_HOME='$config' '$monitors_bin' \"\$@\""
keep "HEADLESS-1 off" "HEADLESS-2 on 1280x720@0 2.00 0,0"
start_tw
wait_until "the kept settings to be put back as tw starts" listed "HEADLESS-1 HEADLESS-1 off"
listed "HEADLESS-2 HEADLESS-2 on 1280x720@0 2.00 0,0" || fail "listed: $(monitors --list)"
# A window there is laid out at that scale: half of 640x360.
start_client a
expect_state a "^configure 320x360 " "a window laid out at scale 2"

# Turned on again, beside the other, from what is kept.
keep "HEADLESS-1 on 1280x720@0 1.00 1280,0" "HEADLESS-2 on 1280x720@0 1.00 0,0"
monitors --apply || fail "tw refused to turn a screen back on"
listed "HEADLESS-1 HEADLESS-1 on 1280x720@0 1.00 1280,0" || fail "listed: $(monitors --list)"
expect_state a "^configure 640x720 " "the window laid out at scale 1 again"

# Nothing kept for another set of screens, and every screen off, change nothing.
keep "HEADLESS-1 off" "HEADLESS-3 off"
monitors --apply || fail "tw-monitors failed with nothing kept for these screens"
keep "HEADLESS-1 off" "HEADLESS-2 off"
! monitors --apply 2>/dev/null || fail "tw took turning every screen off"
listed "HEADLESS-2 HEADLESS-2 on 1280x720@0 1.00 0,0" || fail "listed: $(monitors --list)"
rm "$kept"

# The box: Down to Scale, Right to the next scale, Return applies, Return
# keeps. Then a change undone with Escape, and one undone by itself.
box() { # <name>: the box, saying what it does in $run_dir/<name>.log
	monitors -d 2>"$run_dir/$1.log" &
	box_pid=$!
	client_pids="$client_pids $box_pid"
	wait_until "the box to have the keyboard" grep -qx keyboard "$run_dir/$1.log"
}
said() { grep -qx "$2" "$run_dir/$1.log"; }
box one
press Down Down Down Right Return
wait_until "the change to be applied" said one applied
listed "HEADLESS-1 HEADLESS-1 on 1280x720@0 1.25 1280,0" || fail "listed: $(monitors --list)"
press Return
wait_until "the change to be kept" said one kept
grep -qx "$(printf 'HEADLESS-1\ton\t1280x720@0\t1.25\t1280,0')" "$kept" || fail "kept: $(cat "$kept")"
press Right Return Escape
wait_until "Escape to go back" said one back
wait_until "the screen back at 1.25" listed "HEADLESS-1 HEADLESS-1 on 1280x720@0 1.25 1280,0"
grep -q '1\.50' "$kept" && fail "a change undone was kept"
press Right Return
sleep 10
twice_back() { [ "$(grep -c '^back$' "$run_dir/one.log")" = 2 ]; }
wait_until "an unanswered change to go back by itself" twice_back
wait_until "the screen back at 1.25" listed "HEADLESS-1 HEADLESS-1 on 1280x720@0 1.25 1280,0"

# With the other screen off, the one left cannot be turned off.
press Tab Up Up Right Return Return
wait_until "HEADLESS-2 to be off" listed "HEADLESS-2 HEADLESS-2 off"
press Tab Right
wait_until "the last screen kept on" said one "status: One screen stays on."
press Escape
wait_until "the box to leave" sh -c "! kill -0 $box_pid 2>/dev/null"

stop_tw
for log in "$run_dir"/*.log; do
	! grep -q 'Sanitizer\|runtime error' "$log" || fail "sanitizer report in $(basename "$log")"
done
echo "ok: listed, put back as tw starts, off and on, refused, the box applying, keeping, going back"
