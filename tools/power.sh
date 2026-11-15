#!/bin/sh
# Battery draw next to what the screen is doing, every two seconds: watts,
# frames shown per second, and how many CPU cores' worth of work is going on.
# For telling what an animation or a refresh rate costs: run it on battery,
# leave the screen still for half a minute, then let something move.
#
#   sh tools/power.sh [seconds]        (default 60)
#
# Only lines that say "Discharging" mean anything: on mains the figure is the
# charging rate. The battery's figure lags what happens by about eight
# seconds. Frames are counted from the Intel GPU's interrupts, two per frame
# shown, so the column stays at 0 with another GPU. Needs no root and changes
# nothing.
set -eu

battery=
for dir in /sys/class/power_supply/*; do
	if [ "$(cat "$dir/type" 2>/dev/null)" = Battery ] && [ "$(cat "$dir/scope" 2>/dev/null)" != Device ]; then
		battery=$dir
		break
	fi
done
[ -n "$battery" ] || { echo "this machine has no battery" >&2; exit 1; }

total=${1:-60}
step=2
ticks=$(getconf CLK_TCK)

interrupts() {
	awk '/ i915$/ { for (i = 2; i <= NF; i++) if ($i ~ /^[0-9]+$/) n += $i } END { print n + 0 }' /proc/interrupts
}
# Everything but idle and waiting for a disk, summed over all CPUs.
busy() { awk 'NR == 1 { print $2 + $3 + $4 + $7 + $8 + $9 }' /proc/stat; }
microwatts() {
	if [ -r "$battery/power_now" ]; then
		cat "$battery/power_now"
	else
		echo $(($(cat "$battery/current_now") / 1000 * ($(cat "$battery/voltage_now") / 1000)))
	fi
}

printf '%4s  %-12s %6s  %8s  %9s\n' s status watts frames/s cpus_busy
last_interrupts=$(interrupts)
last_busy=$(busy)
elapsed=0
while [ "$elapsed" -lt "$total" ]; do
	sleep "$step"
	elapsed=$((elapsed + step))
	now_interrupts=$(interrupts)
	now_busy=$(busy)
	awk -v t="$elapsed" -v status="$(cat "$battery/status")" -v uw="$(microwatts)" \
		-v frames=$(((now_interrupts - last_interrupts) / 2)) \
		-v busy=$((now_busy - last_busy)) -v step="$step" -v ticks="$ticks" \
		'BEGIN { printf "%4d  %-12s %6.2f  %8.0f  %9.2f\n", t, status, uw / 1e6, frames / step, busy / ticks / step }'
	last_interrupts=$now_interrupts
	last_busy=$now_busy
done
