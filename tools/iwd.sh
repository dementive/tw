#!/bin/sh
# Moves Wi-Fi from NetworkManager, and the wpa_supplicant it starts, to iwd:
# one small program in place of two large ones. Made so that trying it cannot
# leave the machine without a network.
#
#   sudo sh tools/iwd.sh try    switch over for a trial
#   sudo sh tools/iwd.sh keep   make the trial what starts at boot
#   sudo sh tools/iwd.sh back   return to NetworkManager, now and at boot
#   sh tools/iwd.sh             say which of the two is running
#
# `try` first copies the Wi-Fi networks NetworkManager knows, passwords
# included, to where iwd keeps them, so iwd joins the same network unasked.
# Then it stops NetworkManager, starts iwd, and waits up to a minute for the
# internet to be reachable. If it is not, or the script is interrupted, it
# stops iwd and starts NetworkManager again. Nothing it does lasts past a
# reboot: until `keep`, NetworkManager is still what starts at boot, so
# turning the machine off and on is always a way back.
#
# What iwd does not do: the wired port, VPNs, mobile broadband. Passwords are
# copied between two directories only root can read and are never printed.
#
# For the tests: IWD_ROOT is put in front of /etc, /var and /sys and lifts
# the need to be root, IWD_WAIT is how many seconds `try` waits.
set -eu
LC_ALL=C
export LC_ALL

root=${IWD_ROOT:-}
wait_seconds=${IWD_WAIT:-60}
# The page NetworkManager itself fetches to tell whether the internet is
# there.
check_url=http://ping.archlinux.org/nm-check.txt
nm_dir=$root/etc/NetworkManager/system-connections
phys=$root/sys/class/ieee80211
iwd_dir=$root/var/lib/iwd
iwd_conf=$root/etc/iwd/main.conf

say() { printf '%s\n' "$*"; }
fail() {
	say "$*" >&2
	exit 1
}

need_root() {
	[ -n "$root" ] || [ "$(id -u)" = 0 ] || fail "This changes system services: run it with sudo."
}

active() { systemctl is-active --quiet "$1"; }
enabled() { systemctl is-enabled --quiet "$1" 2>/dev/null; }

# Whether the internet is reachable: a default route, and a page fetched by
# name, which needs an address, a route and DNS to all be working.
online() {
	ip -4 route show default 2>/dev/null | grep -q . &&
		curl -fsS -m 5 -o /dev/null "$check_url" 2>/dev/null
}

# wait_online: 0 as soon as the internet is reachable, 1 if it is not after
# the time allowed.
wait_online() {
	waited=0
	while ! online; do
		[ "$waited" -lt "$wait_seconds" ] || return 1
		sleep 1
		waited=$((waited + 1))
	done
}

# value <group> <key> <file>: one setting of a NetworkManager connection
# file, as written there.
value() {
	awk -v group="[$1]" -v key="$2" '
		/^\[/ { inside = ($0 == group); next }
		inside {
			i = index($0, "=")
			if (i > 0 && substr($0, 1, i - 1) == key) {
				print substr($0, i + 1)
				exit
			}
		}' "$3"
}

# A network's name as NetworkManager writes it, to the name itself: either
# text with a few characters escaped, or a list of byte values.
ssid_decode() {
	if printf '%s' "$1" | grep -Eq '^([0-9]+;)+$'; then
		# shellcheck disable=SC2059
		printf "$(printf '%s' "$1" | awk -F';' '{ for (i = 1; i < NF; i++) printf "\\%03o", $i }')"
	else
		printf '%s' "$1" | awk '{
			out = ""
			n = length($0)
			for (i = 1; i <= n; i++) {
				c = substr($0, i, 1)
				if (c == "\\" && i < n) {
					c = substr($0, ++i, 1)
					if (c == "s") c = " "
					else if (c == "t") c = "\t"
				}
				out = out c
			}
			printf "%s", out
		}'
	fi
}

# The file iwd keeps a network in: its name as it is if that is letters,
# digits, spaces, underscores and minus signs, else "=" and the name in hex.
iwd_file() {
	if printf '%s' "$1" | grep -q '[^A-Za-z0-9 _-]'; then
		printf '=%s.%s' "$(printf '%s' "$1" | od -An -tx1 | tr -d ' \n')" "$2"
	else
		printf '%s.%s' "$1" "$2"
	fi
}

# Copies the Wi-Fi networks NetworkManager knows to iwd. One that iwd
# already knows is left as it is.
import_networks() {
	copied=0
	umask 077
	mkdir -p "$iwd_dir"
	for file in "$nm_dir"/*.nmconnection; do
		[ -f "$file" ] || continue
		case $(value connection type "$file") in
		wifi | 802-11-wireless) ;;
		*) continue ;;
		esac
		name=$(ssid_decode "$(value wifi ssid "$file")")
		[ -n "$name" ] || continue
		key_mgmt=$(value wifi-security key-mgmt "$file")
		password=$(value wifi-security psk "$file")
		hidden=$(value wifi hidden "$file")
		case $key_mgmt in
		'') kind=open ;;
		wpa-psk | sae) kind=psk ;;
		*)
			say "  not copied: '$name' signs in with a user name or certificate. Set it up by hand (man iwd.network)."
			continue
			;;
		esac
		target=$iwd_dir/$(iwd_file "$name" "$kind")
		[ ! -e "$target" ] || continue
		if [ "$kind" = psk ] && [ -z "$password" ]; then
			say "  not copied: '$name' has no password saved for all users. Join it once with: iwctl station <device> connect '$name'"
			continue
		fi
		if [ "$(value ipv4 method "$file")" = manual ]; then
			say "  note: '$name' has a fixed address in NetworkManager, which is not carried over."
		fi
		{
			if [ "$hidden" = true ]; then
				printf '[Settings]\nHidden=true\n'
			fi
			if [ "$kind" = psk ]; then
				# Both files escape a value the same way, so it is copied as
				# written. A key of 64 hex digits is the processed form.
				if printf '%s' "$password" | grep -Eq '^[0-9A-Fa-f]{64}$'; then
					printf '[Security]\nPreSharedKey=%s\n' "$password"
				else
					printf '[Security]\nPassphrase=%s\n' "$password"
				fi
			fi
		} >"$target"
		copied=$((copied + 1))
	done
	say "Saved Wi-Fi networks copied to iwd: $copied."
}

# iwd is to get addresses and hand DNS to systemd-resolved by itself; without
# NetworkManager nothing else would.
configure_iwd() {
	if [ -e "$iwd_conf" ]; then
		grep -Eq '^[[:space:]]*EnableNetworkConfiguration[[:space:]]*=[[:space:]]*(true|1)' "$iwd_conf" ||
			fail "$iwd_conf exists and does not set EnableNetworkConfiguration=true under [General]. Add that line, then run this again."
	else
		umask 022
		mkdir -p "$(dirname "$iwd_conf")"
		printf '[General]\nEnableNetworkConfiguration=true\n' >"$iwd_conf"
	fi
}

# iwd does not use the Wi-Fi interface the kernel made: it deletes it and
# makes its own, which the kernel takes away again the moment iwd stops. A
# Wi-Fi card is then left with no interface at all, and NetworkManager with
# nothing to connect. So give each such card one back, named as the kernel
# names it.
restore_interfaces() {
	for phy in "$phys"/*; do
		[ -d "$phy" ] || continue
		has_interface=
		for net in "$phy"/device/net/*; do
			[ -e "$net" ] && has_interface=yes
		done
		if [ -z "$has_interface" ]; then
			phy=$(basename "$phy")
			iw phy "$phy" interface add "wlan${phy#phy}" type managed ||
				say "Could not give $phy its interface back. A reboot does."
		fi
	done
}

to_networkmanager() {
	systemctl stop iwd.service 2>/dev/null || true
	restore_interfaces
	systemctl start NetworkManager.service
}

status() {
	for service in NetworkManager iwd; do
		say "$service: $(active "$service.service" && echo running || echo "not running"), $(enabled "$service.service" && echo "starts at boot" || echo "does not start at boot")"
	done
	if online; then
		say "The internet is reachable."
	else
		say "The internet is NOT reachable."
	fi
	for device in "$root"/sys/class/net/*/wireless; do
		[ -d "$device" ] || continue
		device=$(basename "$(dirname "$device")")
		say "$device: $(iw dev "$device" get power_save 2>/dev/null || echo "power save unknown")"
	done
}

try() {
	need_root
	command -v iwctl >/dev/null 2>&1 || fail "iwd is not installed: sudo pacman -S iwd"
	command -v iw >/dev/null 2>&1 || fail "iw is not installed, and the way back needs it: sudo pacman -S iw"
	if active iwd.service && ! active NetworkManager.service; then
		fail "iwd is already what is running. 'keep' makes it permanent, 'back' returns to NetworkManager."
	fi
	online || fail "The internet is not reachable now, so a trial would prove nothing. Nothing was changed."
	import_networks
	configure_iwd

	# From here on, anything but success ends with NetworkManager running.
	trial=running
	trap 'if [ "$trial" = running ]; then say "Interrupted: returning to NetworkManager."; to_networkmanager; fi' EXIT
	trap 'exit 1' INT TERM HUP
	say "Stopping NetworkManager and starting iwd. The network is away for some seconds."
	systemctl stop NetworkManager.service
	systemctl stop wpa_supplicant.service 2>/dev/null || true
	# Forget the names looked up so far, so that the check below shows that
	# iwd has told the resolver which DNS servers to ask.
	resolvectl flush-caches 2>/dev/null || true
	if systemctl start iwd.service && wait_online; then
		trial=done
		say ""
		say "The internet is reachable through iwd."
		status
		say ""
		say "This is a trial: NetworkManager is stopped, but still what starts at boot."
		say "  sudo sh tools/iwd.sh keep   to stay on iwd"
		say "  sudo sh tools/iwd.sh back   to return to NetworkManager (a reboot does the same)"
		return 0
	fi

	trial=done
	say ""
	say "No internet through iwd after $wait_seconds seconds. What iwd said:"
	journalctl -b -u iwd.service -n 25 --no-pager 2>/dev/null | sed 's/^/  /' || true
	say ""
	say "Returning to NetworkManager."
	to_networkmanager
	if wait_online; then
		say "Back on NetworkManager, and the internet is reachable. Nothing was changed for good."
	else
		say "NetworkManager is running again but the internet is not back yet."
		say "Give it a moment; if it stays away, a reboot puts everything as it was."
	fi
	exit 1
}

keep() {
	need_root
	active iwd.service || fail "iwd is not running. Run 'try' first."
	online || fail "The internet is not reachable through iwd right now, so nothing was changed. 'back' returns to NetworkManager."
	# NetworkManager's unit takes its two helpers along.
	systemctl disable NetworkManager.service
	systemctl enable iwd.service
	say "iwd now starts at boot, and NetworkManager does not."
	say "'sudo sh tools/iwd.sh back' undoes this. Wi-Fi from here on: iwctl (try 'iwctl station list')."
}

back() {
	need_root
	systemctl disable iwd.service 2>/dev/null || true
	systemctl enable NetworkManager.service
	to_networkmanager
	if wait_online; then
		say "Back on NetworkManager, now and at boot, and the internet is reachable."
	else
		say "NetworkManager is running and starts at boot, but the internet is not reachable yet."
	fi
}

case ${1:-status} in
try) try ;;
keep) keep ;;
back) back ;;
status) status ;;
*) fail "usage: $0 [try|keep|back]" ;;
esac
