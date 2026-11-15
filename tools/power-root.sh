#!/bin/sh
# Where the watts go when nothing is happening: what only root can read.
#
#   sudo sh tools/power-root.sh > build/power-root.txt 2>&1
#
# Reads only; it changes no setting and loads nothing. It takes half a
# minute, most of it one 20-second measurement, and that measurement is only
# worth something if the machine is idle meanwhile: run it from a terminal on
# an empty workspace and keep your hands off until the prompt is back. On
# battery it can also set the battery's own figure against the CPU's.
#
# What it reports, and why:
#   - watts by part of the machine (the CPU's own energy counters, RAPL)
#   - how deep the whole processor package sleeps, and how deep the firmware
#     lets it; shallow sleep with nothing to do is where a high idle draw
#     usually comes from
#   - whether the machine really was idle meanwhile, and who was not
#   - the display: panel self refresh, the refresh rate in use, frame buffer
#     compression, the display engine's own sleep states
#   - whether each PCIe link saves power while idle (ASPM)
#   - which devices ask the platform to stay ready, and which never sleep
#   - what the kernel said at boot about any of the above
set -u
seconds=20
debug=/sys/kernel/debug
cpu=/sys/devices/system/cpu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

section() { printf '\n== %s\n' "$1"; }
show() { # show <file>...: name and content on one line, if it can be read
	for file in "$@"; do
		[ -r "$file" ] && printf '%s: %s\n' "$file" "$(tr '\n' ' ' <"$file" 2>/dev/null)"
	done
}
[ "$(id -u)" = 0 ] || echo "NOT ROOT: the parts only root can read will be missing"

battery=
for dir in /sys/class/power_supply/*; do
	if [ "$(cat "$dir/type" 2>/dev/null)" = Battery ] && [ "$(cat "$dir/scope" 2>/dev/null)" != Device ]; then
		battery=$dir
	fi
done

section "state"
date
echo "kernel $(uname -r), up $(cut -d' ' -f1 /proc/uptime) s"
cat /proc/cmdline
for dir in /sys/class/power_supply/*; do
	echo "${dir##*/}: type=$(cat "$dir/type" 2>/dev/null) online=$(cat "$dir/online" 2>/dev/null)" \
		"status=$(cat "$dir/status" 2>/dev/null) capacity=$(cat "$dir/capacity" 2>/dev/null)"
done
show $cpu/cpu0/cpufreq/energy_performance_preference /sys/firmware/acpi/platform_profile \
	$cpu/intel_pstate/no_turbo $cpu/intel_pstate/max_perf_pct $cpu/intel_pstate/min_perf_pct \
	/sys/module/pcie_aspm/parameters/policy /sys/power/mem_sleep
for dir in /sys/class/backlight/*; do
	echo "${dir##*/}: $(cat "$dir/brightness") of $(cat "$dir/max_brightness")"
done

# --- One measurement: everything that counts, read twice --------------------
snapshot() { # snapshot <a|b>
	date +%s.%N >"$tmp/time.$1"
	for zone in /sys/class/powercap/*; do
		[ -r "$zone/energy_uj" ] || continue
		echo "${zone##*/} $(cat "$zone/name") $(cat "$zone/energy_uj") $(cat "$zone/max_energy_range_uj")"
	done >"$tmp/rapl.$1" 2>/dev/null
	cat "$debug/pmc_core/package_cstate_show" >"$tmp/package.$1" 2>/dev/null
	cat "$debug/pmc_core/slp_s0_residency_usec" >"$tmp/s0.$1" 2>/dev/null
	for state in $cpu/cpu*/cpuidle/state*; do
		echo "$(cat "$state/name") $(cat "$state/time")"
	done >"$tmp/idle.$1"
	cat /sys/class/drm/card*/gt/gt0/rc6_residency_ms >"$tmp/rc6.$1" 2>/dev/null
	awk 'NR > 1 { n = 0; label = ""
		for (i = 2; i <= NF; i++) { if ($i ~ /^[0-9]+$/ && label == "") n += $i; else label = label " " $i }
		print $1, n, label }' /proc/interrupts >"$tmp/irq.$1"
	awk 'NR == 1 { print $2 + $3 + $4 + $7 + $8 + $9, $2 + $3 + $4 + $5 + $6 + $7 + $8 + $9 }' /proc/stat >"$tmp/stat.$1"
	[ -n "$battery" ] && cat "$battery/energy_now" >"$tmp/energy.$1" 2>/dev/null
	for file in "$debug"/dri/*/i915_dmc_info; do
		[ -r "$file" ] && grep -E 'count' "$file"
	done 2>/dev/null | sort -u >"$tmp/dmc.$1"
	# CPU time of every process: pid, ticks, name.
	awk '{ n = split($0, part, ") "); split(part[n], field, " "); name = $2
		sub(/^\(/, "", name); sub(/\)$/, "", name); print $1, field[12] + field[13], name }' \
		/proc/[0-9]*/stat 2>/dev/null >"$tmp/proc.$1"
	# How long each PCI device has been awake. Read here and not later:
	# listing devices with lspci wakes the ones that sleep.
	for dir in /sys/bus/pci/devices/*; do
		echo "${dir##*/} $(cat "$dir/power/runtime_active_time" 2>/dev/null)" \
			"$(cat "$dir/power/runtime_status" 2>/dev/null) $(cat "$dir/power/control" 2>/dev/null)"
	done >"$tmp/pci.$1"
}
snapshot a
sleep "$seconds"
snapshot b
elapsed=$(awk -v a="$(cat "$tmp/time.a")" -v b="$(cat "$tmp/time.b")" 'BEGIN { printf "%.3f", b - a }')

section "watts over $elapsed s, by part (RAPL: psys is the whole machine where the firmware reports it)"
awk -v s="$elapsed" 'NR == FNR { before[$1] = $3; next }
	{ d = $3 - before[$1]; if (d < 0) d += $4; printf "%-18s %-10s %6.2f W\n", $1, $2, d / 1e6 / s }' \
	"$tmp/rapl.a" "$tmp/rapl.b"
if [ -n "$battery" ]; then
	echo "battery: $(cat "$battery/status"), power_now $(cat "$battery/power_now" 2>/dev/null) uW," \
		"energy $(cat "$tmp/energy.a" 2>/dev/null) -> $(cat "$tmp/energy.b" 2>/dev/null) uWh"
fi

section "was the machine idle? (it has to be, for the rest to mean much)"
ticks=$(getconf CLK_TCK)
awk -v s="$elapsed" -v ticks="$ticks" 'NR == FNR { busy = $1; next }
	{ cores = ($1 - busy) / ticks / s
	  printf "%.2f of one cpu core busy: %s\n", cores, cores < 0.1 ? "idle" : "NOT IDLE, something was working" }' \
	"$tmp/stat.a" "$tmp/stat.b"
awk 'NR == FNR { before += $2; next } { after += $2 } END { printf "%.0f interrupts per second in all\n", (after - before) / s }' \
	s="$elapsed" "$tmp/irq.a" "$tmp/irq.b"
echo "busiest programs, in ms of cpu per second:"
awk -v s="$elapsed" -v ticks="$ticks" 'NR == FNR { before[$1] = $2; next }
	($1 in before) && $2 > before[$1] { printf "%8.1f  %s (%s)\n", 1000 * ($2 - before[$1]) / ticks / s, $3, $1 }' \
	"$tmp/proc.a" "$tmp/proc.b" | sort -rn | head -n 6

section "how deep the processor package slept (share of the measurement)"
# pmc_core gives the time spent in each state in microseconds (it divides
# the CPU's counters by the TSC's rate itself). What the shares leave over
# was spent awake.
paste -d'|' "$tmp/package.a" "$tmp/package.b" 2>/dev/null | awk -F'|' -v s="$elapsed" '{
	split($1, a, ":"); split($2, b, ":")
	printf "%-14s %6.1f%%\n", a[1], 100 * (b[2] - a[2]) / (s * 1e6) }'
if [ -s "$tmp/s0.a" ]; then
	echo "deepest platform state (S0ix): $(awk -v a="$(cat "$tmp/s0.a")" -v b="$(cat "$tmp/s0.b")" -v s="$elapsed" \
		'BEGIN { printf "%.2f%%", 100 * (b - a) / (s * 1e6) }') of the interval"
fi

section "the deepest package sleep the firmware allows (register 0xE2 of the CPU)"
msr() { # msr <number>: one register of the first CPU, as 16 hex digits
	dd if=/dev/cpu/0/msr bs=8 count=1 skip=$(($1)) iflag=skip_bytes 2>/dev/null | od -A n -t x8 | tr -d ' '
}
if [ -r /dev/cpu/0/msr ]; then
	raw=$(msr 0xe2)
	echo "raw 0x$raw"
	if [ -n "$raw" ]; then
		# The table of this CPU family, as turbostat has it.
		set -- C0 C2 C3 C6 C7 C7s C8 C9 "no limit (C10)"
		limit=$((0x$raw & 0xf))
		shift "$((limit < 8 ? limit : 8))"
		echo "limit: $limit, package $1; locked by the firmware: $(((0x$raw >> 15) & 1))"
	fi
	# How soon the package has to be awake again from each level. It only goes
	# to a level if every device tolerates that long a wait (their requests
	# are under "what each part of the chipset asks for" below).
	for level in "C3 0x60a" "C6 0x60b" "C7 0x60c" "C8 0x633" "C9 0x634" "C10 0x635"; do
		raw=$(msr "${level#* }")
		[ -n "$raw" ] || continue
		value=$((0x$raw))
		ns=$((value & 0x3ff))
		unit=$(((value >> 10) & 7)) # the count is in units of 32^unit ns
		while [ "$unit" -gt 0 ]; do
			ns=$((ns * 32))
			unit=$((unit - 1))
		done
		echo "package ${level% *}: awake again within $((ns / 1000)) us (raw 0x$raw, in force: $(((value >> 15) & 1)))"
	done
else
	echo "cannot read /dev/cpu/0/msr"
fi

section "how deep the cores slept (share of all cpu time)"
awk -v s="$elapsed" -v n="$(nproc)" 'NR == FNR { before[$1] += $2; next } { after[$1] += $2 }
	END { for (name in after) printf "%-8s %6.2f%%\n", name, 100 * (after[name] - before[name]) / (s * 1e6 * n) }' \
	"$tmp/idle.a" "$tmp/idle.b" | sort
paste "$tmp/rc6.a" "$tmp/rc6.b" 2>/dev/null |
	awk -v s="$elapsed" '{ printf "gpu asleep (rc6): %.1f%%\n", 100 * ($2 - $1) / (s * 1e3) }'

section "interrupts per second (top 10)"
awk -v s="$elapsed" 'NR == FNR { before[$1] = $2; next }
	{ d = $2 - before[$1]; id = $1; $1 = ""; $2 = ""; if (d > 0) printf "%9.1f  %s %s\n", d / s, id, $0 }' \
	"$tmp/irq.a" "$tmp/irq.b" | sort -rn | head -n 10

section "display engine sleep counters, before and after"
paste -d'|' "$tmp/dmc.a" "$tmp/dmc.b" 2>/dev/null

# --- The display -------------------------------------------------------------
section "display (i915)"
show /sys/module/i915/parameters/enable_psr /sys/module/i915/parameters/enable_fbc \
	/sys/module/i915/parameters/enable_dc /sys/module/i915/parameters/enable_guc \
	/sys/module/i915/parameters/psr_safest_params /sys/module/i915/parameters/enable_psr2_sel_fetch
seen=
for dri in "$debug"/dri/*; do
	[ -e "$dri/i915_display_info" ] || continue
	# The same device can be listed under several names.
	dri=$(readlink -f "$dri")
	case " $seen " in *" $dri "*) continue ;; esac
	seen="$seen $dri"
	echo "-- $dri has: $(ls "$dri" | tr '\n' ' ')"
	find "$dri" -maxdepth 2 \( -name '*psr_status' -o -name '*drrs_status' -o -name '*fbc_status' \
		-o -name 'i915_sr_status' -o -name 'i915_dmc_info' -o -name 'i915_runtime_pm_status' \) 2>/dev/null |
		sort | while read -r file; do
		echo "-- $file"
		head -n 45 "$file" 2>&1
	done
	echo "-- $dri/i915_display_info (start)"
	head -n 45 "$dri/i915_display_info" 2>&1
	echo "-- $dri/i915_power_domain_info: wells in use"
	grep -vE ' 0$' "$dri/i915_power_domain_info" 2>&1 | head -n 60
done

# --- Devices -----------------------------------------------------------------
section "PCIe links: the power saving each can do, and what is switched on"
lspci -vv 2>/dev/null | awk '
	/^[0-9a-f]/ { device = $0; next }
	/LnkCap:/ { can = $0; sub(/.*ASPM /, "ASPM ", can); sub(/,.*/, "", can) }
	/LnkCtl:/ { now = $0; sub(/.*ASPM /, "ASPM ", now); sub(/;.*/, "", now)
		printf "%s\n    can: %s   now: %s\n", device, can, now; can = "" }
	/L1SubCtl1:/ { sub(/^[ \t]+/, ""); printf "    %s\n", $0 }'

section "PCI devices that were awake during the measurement, and for how much of it"
lspci -D >"$tmp/names" 2>/dev/null
awk -v s="$elapsed" 'FILENAME == ARGV[1] { name[$1] = substr($0, length($1) + 2); next }
	FILENAME == ARGV[2] { before[$1] = $2; next }
	{ share = 100 * ($2 - before[$1]) / (s * 1000)
	  if (share >= 1) printf "%5.0f%%  %s %s [control=%s]\n", share, $1, name[$1], $4 }' \
	"$tmp/names" "$tmp/pci.a" "$tmp/pci.b"

section "USB devices"
for dir in /sys/bus/usb/devices/*; do
	[ -r "$dir/product" ] || continue
	echo "${dir##*/} $(cat "$dir/product"): $(cat "$dir/power/runtime_status") control=$(cat "$dir/power/control")" \
		"speed=$(cat "$dir/speed") u1=$(cat "$dir/power/usb3_hardware_lpm_u1" 2>/dev/null)" \
		"u2=$(cat "$dir/power/usb3_hardware_lpm_u2" 2>/dev/null)"
done

section "sound: time each codec has spent powered and powered down, in ms"
cat /proc/asound/cards 2>/dev/null
for codec in /sys/class/sound/card*/hwC*; do
	echo "${codec##*/} $(cat "$codec/vendor_name" 2>/dev/null) $(cat "$codec/chip_name" 2>/dev/null):" \
		"on $(cat "$codec/power_on_acct" 2>/dev/null) off $(cat "$codec/power_off_acct" 2>/dev/null)"
done
show /sys/module/snd_hda_intel/parameters/power_save /sys/module/snd_hda_intel/parameters/power_save_controller
for status in /proc/asound/card*/pcm*/sub*/status; do
	[ "$(head -n 1 "$status")" = closed ] || echo "open: $status"
done

section "disks"
show /sys/module/nvme_core/parameters/default_ps_max_latency_us
for disk in /dev/nvme[0-9]; do
	[ -e "$disk" ] || continue
	echo "-- $disk"
	smartctl -c "$disk" 2>&1 | sed -n '/Supported Power States/,$p' | head -n 12
done

section "what each part of the chipset asks for (latency it tolerates; pmc_core ltr_show)"
grep -vE 'RAW: 0x0[[:space:]]' "$debug/pmc_core/ltr_show" 2>&1 | head -n 40
section "chipset parts not powered down (pmc_core pch_ip_power_gating_status)"
grep -E 'State: On' "$debug/pmc_core/pch_ip_power_gating_status" 2>&1 | head -n 60
echo "pmc_core has: $(ls "$debug/pmc_core" 2>&1 | tr '\n' ' ')"

section "what the kernel said"
dmesg 2>&1 | grep -iE 'aspm|psr|drrs|dmc|vmd|power_save|denylist|s0ix|low.power|pmc_core|LTR|rapl|_OSC|i915.*(fbc|dc[0-9]|guc|huc)|nvme.*(apst|power)|throttl' |
	cut -c1-180 | head -n 80
