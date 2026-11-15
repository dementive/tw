#!/bin/sh
# Where the machine's memory is: splits the "used" figure of btop and free
# into its parts, which add up to it. Reads /proc, changes nothing, needs no
# root.
#
#   sh tools/mem.sh
#
# The parts:
#   held back   free memory and cache that the kernel does not count as
#               available: its reserve for itself (large while transparent
#               huge pages are on) and a slice of the file cache. Nothing
#               uses it.
#   kernel      what no program holds: drivers, kernel modules, the table of
#               the compressed swap device, pages parked per CPU, and the
#               kernel's own bookkeeping (slab, stacks, page tables).
#   shared      memory shared between programs, mostly the buffers of the
#               screen and of windows.
#   programs    what each program holds for itself.
set -u

awk '
{ v[$1] = $2 }
END {
	mb = 1024
	used = v["MemTotal:"] - v["MemAvailable:"]
	file = v["Active(file):"] + v["Inactive(file):"]
	held = v["MemFree:"] + file + v["SReclaimable:"] - v["MemAvailable:"]
	counted = v["SUnreclaim:"] + v["KernelStack:"] + v["PageTables:"] + v["SecPageTables:"] + v["Percpu:"]
	shared = v["Shmem:"]
	programs = v["AnonPages:"]
	other = used - held - counted - shared - programs
	printf "%6.0f MB  used, as btop and free count it (of %.0f MB)\n", used / mb, v["MemTotal:"] / mb
	printf "%6.0f     held back by the kernel: free memory and cache, in use by nothing\n", held / mb
	printf "%6.0f     kernel: drivers, modules, swap table, pages parked per CPU\n", other / mb
	printf "%6.0f     kernel: its own bookkeeping (slab %.0f, stacks %.0f, page tables %.0f, per CPU %.0f)\n",
		counted / mb, v["SUnreclaim:"] / mb, v["KernelStack:"] / mb,
		(v["PageTables:"] + v["SecPageTables:"]) / mb, v["Percpu:"] / mb
	printf "%6.0f     shared: screen and window buffers\n", shared / mb
	printf "%6.0f     programs, each for itself:\n", programs / mb
}' /proc/meminfo

# Programs by what they hold for themselves; the same program name is summed.
for status in /proc/[0-9]*/status; do
	awk '/^Name:/ { name = $2 } /^RssAnon:/ { if ($2 > 0) print $2, name }' "$status" 2>/dev/null
done | awk '
{ kb[$2] += $1; count[$2]++ }
END { for (name in kb) printf "%d %s %d\n", kb[name], name, count[name] }' | sort -rn | awk '
NR <= 14 { printf "%14.1f  %s%s\n", $1 / 1024, $2, ($3 > 1 ? " (" $3 " processes)" : ""); next }
{ rest += $1; n += $3 }
END { if (n) printf "%14.1f  %d smaller ones\n", rest / 1024, n }'

parked=$(awk '/count:/ { pages += $2 } END { printf "%.0f", pages * 4 / 1024 }' /proc/zoneinfo)
echo "Of the kernel part, $parked MB are free pages parked per CPU."
