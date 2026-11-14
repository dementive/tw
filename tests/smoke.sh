#!/bin/sh
# End-to-end check: windows are sized into columns and focused, a window that
# scrolls off-screen is suspended, and tw shuts down cleanly. In a sanitizer
# build a use-after-free, undefined behaviour or a leak fails this test.
set -eu
. "$(dirname "$0")/lib.sh"

sized='configure [1-9][0-9]*x[1-9][0-9]*'

start_tw

start_client a
expect a "$sized activated=1 suspended=0" "the first window to be sized and focused"

# Each new window opens to the right and takes focus. Five columns never fit
# on one output with the stock column widths, so the first one scrolls off.
for name in b c d e; do
	start_client "$name"
done
expect e "$sized activated=1 suspended=0" "the last window to be focused"
expect a "$sized activated=0 suspended=1" "the first window to be suspended off-screen"

# New columns must not resize existing ones: every window got the same size.
size_a=$(sed -n 's/^configure \([0-9x]*\) .*/\1/p' "$run_dir/a.log" | sort -u)
size_e=$(sed -n 's/^configure \([0-9x]*\) .*/\1/p' "$run_dir/e.log" | sort -u)
[ "$size_a" = "$size_e" ] || fail "window sizes differ or changed: a='$size_a' e='$size_e'"

stop_tw
echo "ok: columns sized $size_a, off-screen window suspended, clean exit"
