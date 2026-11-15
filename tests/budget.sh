#!/bin/sh
# Performance budgets, checked on an optimized build:
#   $3  max context switches over 3 s while idle with clients mapped
#   $4  max own memory in kB
#   $5  max of that allocated by tw itself, in kB
# An idle compositor must not wake up at all, so the first budget is 0.
#
# "Own memory" is PSS (proportional set size: shared pages counted fairly)
# minus shared-memory pixel buffers. Those are the software framebuffer and
# the clients' window contents; they scale with resolution, not with our code.
# On real hardware the GPU driver adds far more than tw itself uses (measure
# with WLR_RENDERER=gles2), which this budget deliberately leaves out.
set -eu
. "$(dirname "$0")/lib.sh"

max_wakeups=$3
max_own_kb=$4
max_alloc_kb=$5
window=3

# Summed over all threads, so a driver or library thread that polls counts too.
switches() {
	cat /proc/"$tw_pid"/task/*/status |
		awk '/^voluntary_ctxt_switches/ { n += $2 } END { print n }'
}

# X11 support is on and waiting for its first client during the measurement:
# a feature nobody is using has to fit in the same budget of zero wakeups.
# The stand-in helper is never started.
fake_program xwayland-satellite 'exit 1'
x11_waiting() {
	for lock in /tmp/.X*-lock; do
		[ "$(tr -d ' \n' <"$lock" 2>/dev/null)" = "$tw_pid" ] && return 0
	done
	return 1
}

start_tw
x11_waiting || fail "X11 support was not active during the measurement"
for name in a b c; do
	start_client "$name"
done
sleep 1 # let the initial frames and configures settle

before=$(switches)
sleep "$window"
after=$(switches)
wakeups=$((after - before))
rollup=/proc/"$tw_pid"/smaps_rollup
pss_kb=$(awk '/^Pss:/ { print $2 }' "$rollup")
own_kb=$(awk '/^Pss_Anon:|^Pss_File:/ { n += $2 } END { print n }' "$rollup")
# What tw allocated itself. Unlike the figures above this does not move with
# what else is running, so it is the one to compare between two builds, and
# it has the tighter budget.
anon_kb=$(awk '/^RssAnon:/ { print $2 }' /proc/"$tw_pid"/status)

echo "idle wakeups in ${window}s: $wakeups (budget $max_wakeups)"
echo "own memory: $own_kb kB (budget $max_own_kb kB)"
echo "of which allocated by tw: $anon_kb kB (budget $max_alloc_kb kB)"
echo "total PSS including pixel buffers: $pss_kb kB"

[ "$wakeups" -le "$max_wakeups" ] || fail "idle wakeups over budget"
[ "$own_kb" -le "$max_own_kb" ] || fail "own memory over budget"
[ "$anon_kb" -le "$max_alloc_kb" ] || fail "tw's own allocations over budget"
stop_tw
