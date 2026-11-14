#!/bin/sh
# Performance budgets, checked on an optimized build:
#   $3  max context switches over 3 s while idle with clients mapped
#   $4  max own memory in kB
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
window=3

# Summed over all threads, so a driver or library thread that polls counts too.
switches() {
	cat /proc/"$tw_pid"/task/*/status |
		awk '/^voluntary_ctxt_switches/ { n += $2 } END { print n }'
}

start_tw
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

echo "idle wakeups in ${window}s: $wakeups (budget $max_wakeups)"
echo "own memory: $own_kb kB (budget $max_own_kb kB)"
echo "total PSS including pixel buffers: $pss_kb kB"

[ "$wakeups" -le "$max_wakeups" ] || fail "idle wakeups over budget"
[ "$own_kb" -le "$max_own_kb" ] || fail "own memory over budget"
stop_tw
