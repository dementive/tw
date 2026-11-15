#!/bin/sh
# tools/iwd.sh, which moves Wi-Fi from NetworkManager to iwd, against a
# made-up machine: connection files in a scratch directory and stand-ins for
# systemctl, curl, ip and iw that keep track of what would be running. No
# network, service or root is involved.
#
# What it can show: which networks are copied and how, and that every way
# out of a trial that did not work ends with NetworkManager running and the
# Wi-Fi card having an interface. What it cannot: that iwd connects.
set -eu

tools=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
bin=$dir/bin
state=$dir/state
root=$dir/root
log=$dir/calls.log
mkdir -p "$bin" "$state" "$root/etc/NetworkManager/system-connections"

fail() {
	echo "FAIL: $*" >&2
	[ -s "$dir/out" ] && sed 's/^/  out: /' "$dir/out" >&2
	[ -s "$log" ] && sed 's/^/  call: /' "$log" >&2
	exit 1
}

stand_in() {
	printf '#!/bin/sh\n%s\n' "$2" >"$bin/$1"
	chmod +x "$bin/$1"
}

# The made-up machine. A service is running if state/active.<unit> exists
# and starts at boot if state/enabled.<unit> does. The internet is reachable
# if state/online exists. The Wi-Fi card's interface is a directory, as in
# /sys. What iwd does is set by state/iwd: "works", "offline" (starts, never
# gets online) or "broken" (does not start).
interface=$root/sys/class/ieee80211/phy0/device/net/wlan0
stand_in systemctl '
state='"$state"'; interface='"$interface"'; log='"$log"'
command=$1; shift
quiet=
[ "${1:-}" = --quiet ] && shift
case $command in
is-active) [ -e "$state/active.$1" ]; exit ;;
is-enabled) [ -e "$state/enabled.$1" ]; exit ;;
esac
echo "$command $*" >>"$log"
for unit in "$@"; do
	case $command:$unit in
	start:iwd.service)
		[ "$(cat "$state/iwd")" != broken ] || exit 1
		# iwd swaps the interface for one of its own.
		mkdir -p "$interface"
		touch "$state/active.$unit"
		[ "$(cat "$state/iwd")" != works ] || touch "$state/online"
		;;
	stop:iwd.service)
		# ... which goes away with it.
		[ ! -e "$state/active.$unit" ] || rm -rf "$interface" "$state/online"
		rm -f "$state/active.$unit"
		;;
	start:NetworkManager.service)
		touch "$state/active.$unit"
		[ ! -d "$interface" ] || touch "$state/online"
		;;
	stop:NetworkManager.service) rm -f "$state/active.$unit" "$state/online" ;;
	start:*) touch "$state/active.$unit" ;;
	stop:*) rm -f "$state/active.$unit" ;;
	enable:*) touch "$state/enabled.$unit" ;;
	disable:*) rm -f "$state/enabled.$unit" ;;
	esac
done'
stand_in curl '[ -e '"$state"'/online ]'
stand_in ip '[ -e '"$state"'/online ] && echo "default via 192.0.2.1 dev wlan0"'
stand_in iw '
echo "iw $*" >>'"$log"'
case "$*" in
"phy phy0 interface add wlan0 type managed") mkdir -p '"$interface"' ;;
*get\ power_save) echo "Power save: on" ;;
esac'
stand_in iwctl 'exit 0'
stand_in resolvectl 'exit 0'
stand_in journalctl 'echo "a line from iwd"'
# Waiting takes no time, unless a test wants to catch the script at it.
stand_in sleep '[ ! -e '"$state"'/slow ] || /usr/bin/sleep 0.1'

# machine <iwd behaviour>: NetworkManager running and online, as at boot.
machine() {
	rm -rf "$state" "$root/var" "$root/etc/iwd" "$log" "$dir/out"
	mkdir -p "$state" "$interface"
	touch "$state/active.NetworkManager.service" "$state/enabled.NetworkManager.service" \
		"$state/active.wpa_supplicant.service" "$state/online"
	echo "$1" >"$state/iwd"
}

# run <command>: the script on the made-up machine; output in $dir/out.
run() {
	PATH="$bin:/usr/bin:/bin" IWD_ROOT="$root" IWD_WAIT=3 sh "$tools/iwd.sh" "$@" >"$dir/out" 2>&1
}
is() { [ -e "$state/$1" ]; }
said() { grep -q "$1" "$dir/out"; }
called() { grep -qx "$1" "$log"; }

connection() { # <file> <lines...>
	file=$root/etc/NetworkManager/system-connections/$1.nmconnection
	shift
	printf '%s\n' "$@" >"$file"
}

# --- The networks NetworkManager knows ---------------------------------------

connection home '[connection]' 'id=home' 'type=wifi' '[wifi]' 'mode=infrastructure' \
	'ssid=Home Net_5-G' '[wifi-security]' 'key-mgmt=wpa-psk' 'psk=correct horse' \
	'[ipv4]' 'method=auto'
# A name with a letter and a sign outside what iwd puts in a file name, a
# WPA3 network, and a password with characters the file format escapes.
connection cafe '[connection]' 'type=wifi' '[wifi]' 'ssid=Café\;1' '[wifi-security]' \
	'key-mgmt=sae' 'psk=\sleading space and a \\ backslash'
# A name NetworkManager wrote as bytes ("Hi!"), and a key in its processed form.
connection bytes '[connection]' 'type=802-11-wireless' '[wifi]' 'ssid=72;105;33;' \
	'[wifi-security]' 'key-mgmt=wpa-psk' \
	'psk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF'
connection open '[connection]' 'type=wifi' '[wifi]' 'hidden=true' 'ssid=Open Place'
connection fixed '[connection]' 'type=wifi' '[wifi]' 'ssid=Fixed' '[wifi-security]' \
	'key-mgmt=wpa-psk' 'psk=fixed-password' '[ipv4]' 'address1=192.0.2.5/24' 'method=manual'
# Not copied: one that signs in with a user name, one whose password is in a
# keyring and not in the file, one iwd knows already, and the wired port.
connection work '[connection]' 'type=wifi' '[wifi]' 'ssid=Work' '[wifi-security]' 'key-mgmt=wpa-eap'
connection agent '[connection]' 'type=wifi' '[wifi]' 'ssid=Agent' '[wifi-security]' \
	'key-mgmt=wpa-psk' 'psk-flags=1'
connection known '[connection]' 'type=wifi' '[wifi]' 'ssid=Known' '[wifi-security]' \
	'key-mgmt=wpa-psk' 'psk=the old one'
connection wired '[connection]' 'type=ethernet' '[ethernet]' '[wifi-security]' 'psk=never'

# --- A trial that works --------------------------------------------------------

machine works
# iwd knows one network already, from a password typed by hand.
mkdir -p "$root/var/lib"
mkdir -m 700 "$root/var/lib/iwd"
(umask 077 && printf '[Security]\nPassphrase=typed by hand\n' >"$root/var/lib/iwd/Known.psk")
run try || fail "a trial that works failed"

iwd=$root/var/lib/iwd
expect_file() { # <name> <contents>
	[ "$(cat "$iwd/$1" 2>/dev/null)" = "$2" ] || fail "$1 holds: $(cat "$iwd/$1" 2>&1)"
	[ "$(stat -c %a "$iwd/$1")" = 600 ] || fail "$1 can be read by others"
}
expect_file 'Home Net_5-G.psk' "$(printf '[Security]\nPassphrase=correct horse')"
expect_file '=436166c3a93b31.psk' "$(printf '[Security]\nPassphrase=\\sleading space and a \\\\ backslash')"
expect_file '=486921.psk' "$(printf '[Security]\nPreSharedKey=0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF')"
expect_file 'Open Place.open' "$(printf '[Settings]\nHidden=true')"
expect_file 'Fixed.psk' "$(printf '[Security]\nPassphrase=fixed-password')"
expect_file 'Known.psk' "$(printf '[Security]\nPassphrase=typed by hand')"
[ "$(ls "$iwd" | wc -l)" = 6 ] || fail "iwd got networks it should not have: $(ls "$iwd")"
[ "$(stat -c %a "$iwd")" = 700 ] || fail "iwd's directory can be read by others"
said 'copied to iwd: 5' || fail "the count of copied networks is wrong"
said "not copied: 'Work'" || fail "nothing said about the network with a user name"
said "not copied: 'Agent'" || fail "nothing said about the network without a saved password"
said "'Fixed' has a fixed address" || fail "nothing said about the fixed address"
if grep -q 'correct horse\|fixed-password\|backslash\|0123456789abcdef\|the old one' "$dir/out"; then
	fail "a password was printed"
fi
[ "$(cat "$root/etc/iwd/main.conf")" = "$(printf '[General]\nEnableNetworkConfiguration=true')" ] ||
	fail "iwd is not told to get addresses itself"
[ "$(stat -c %a "$root/etc/iwd/main.conf")" = 644 ] || fail "main.conf has odd permissions"

is active.iwd.service || fail "iwd is not running after a trial that worked"
is active.NetworkManager.service && fail "NetworkManager still runs next to iwd"
is active.wpa_supplicant.service && fail "wpa_supplicant still runs next to iwd"
# Still a trial: a reboot goes back.
is enabled.NetworkManager.service || fail "a trial stopped NetworkManager from starting at boot"
is enabled.iwd.service && fail "a trial made iwd start at boot"
said 'reachable through iwd' || fail "the trial did not say it worked"

# A second trial on top of it is refused.
run try && fail "a trial while iwd runs was not refused"

# --- Keeping it, and going back ------------------------------------------------

run keep || fail "keep failed"
is enabled.iwd.service || fail "keep did not make iwd start at boot"
is enabled.NetworkManager.service && fail "keep left NetworkManager starting at boot"

run back || fail "back failed"
is active.NetworkManager.service || fail "back did not start NetworkManager"
is enabled.NetworkManager.service || fail "back did not make NetworkManager start at boot"
is active.iwd.service && fail "back left iwd running"
is enabled.iwd.service && fail "back left iwd starting at boot"
# iwd took the card's interface with it, and NetworkManager needs one.
called 'iw phy phy0 interface add wlan0 type managed' || fail "back did not give the card its interface back"
is online || fail "no network after back"
said 'the internet is reachable' || fail "back did not say the network is there"

# Not kept unless it is running and online.
machine works
run keep && fail "keep without a trial was not refused"
is enabled.NetworkManager.service || fail "a refused keep changed what starts at boot"

# --- Trials that do not work -----------------------------------------------------

# iwd starts and never gets online.
machine offline
run try && fail "a trial without a network did not fail"
is active.NetworkManager.service || fail "NetworkManager is not back after a failed trial"
is active.iwd.service && fail "iwd still runs after a failed trial"
called 'iw phy phy0 interface add wlan0 type managed' || fail "the card got no interface back after a failed trial"
is online || fail "no network after a failed trial"
is enabled.NetworkManager.service || fail "a failed trial changed what starts at boot"
said 'Back on NetworkManager, and the internet is reachable' || fail "a failed trial did not say where things stand"
said 'a line from iwd' || fail "a failed trial did not show what iwd said"

# iwd does not start at all.
machine broken
run try && fail "a trial with a broken iwd did not fail"
is active.NetworkManager.service || fail "NetworkManager is not back after iwd failed to start"
is online || fail "no network after iwd failed to start"

# The machine is offline to begin with: nothing is touched.
machine works
rm "$state/online"
run try && fail "a trial on a machine that is offline was not refused"
[ ! -e "$log" ] || fail "a refused trial stopped or started something: $(cat "$log")"
[ ! -e "$root/var/lib/iwd" ] || fail "a refused trial copied networks"

# An iwd configuration somebody wrote, which does not do addresses: left
# alone, and no trial.
machine works
mkdir -p "$root/etc/iwd"
printf '[General]\nEnableNetworkConfiguration=false\n' >"$root/etc/iwd/main.conf"
run try && fail "a trial with a main.conf that turns addresses off was not refused"
is active.NetworkManager.service || fail "NetworkManager was stopped although the trial was refused"
grep -q 'EnableNetworkConfiguration=false' "$root/etc/iwd/main.conf" || fail "somebody's main.conf was changed"

# Interrupted while waiting for the network (the terminal closed, Ctrl-C).
machine offline
touch "$state/slow"
PATH="$bin:/usr/bin:/bin" IWD_ROOT="$root" IWD_WAIT=600 sh "$tools/iwd.sh" try >"$dir/out" 2>&1 &
pid=$!
tries=0
until grep -qx 'start iwd.service' "$log" 2>/dev/null; do
	tries=$((tries + 1))
	[ "$tries" -le 100 ] || fail "the trial never started iwd"
	/usr/bin/sleep 0.05
done
kill -TERM "$pid"
wait "$pid" && fail "an interrupted trial reported success"
is active.NetworkManager.service || fail "NetworkManager is not back after an interrupted trial"
is active.iwd.service && fail "iwd still runs after an interrupted trial"
is online || fail "no network after an interrupted trial"
said 'Interrupted' || fail "an interrupted trial did not say so"

# --- Saying where things stand, without root -----------------------------------

machine works
run status || fail "status failed"
said 'NetworkManager: running, starts at boot' || fail "status is wrong about NetworkManager"
said 'iwd: not running, does not start at boot' || fail "status is wrong about iwd"
[ ! -e "$log" ] || fail "status changed something"

echo "ok: networks copied, trial, keep, back, failed and interrupted trials, refusals, status"
