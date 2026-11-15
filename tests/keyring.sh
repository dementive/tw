#!/bin/sh
# tools/keyring.sh, which makes the login unlock oo7 in place of the KDE
# wallet, against a made-up machine: a copy of the login file in a scratch
# directory, and stand-ins for systemctl, busctl, secret-tool and pkill that
# keep track of which keyring would be running. No login, bus or root is
# involved.
#
# What it can show: that the login file comes out exactly as meant and goes
# back exactly as it was, and what `start` does in each state it can find.
# What it cannot: that the login module works inside a real login.
set -eu

tools=$1
dir=$(mktemp -d)
bin=$dir/bin
state=$dir/state
root=$dir/root
log=$dir/calls.log
home=$dir/home
mkdir -p "$bin" "$state" "$root/etc/pam.d" "$root/usr/lib/security" "$home/.local/share/kwalletd"

stop_keyrings() {
	for pid_file in "$state"/pid.*; do
		[ -e "$pid_file" ] && kill "$(cat "$pid_file")" 2>/dev/null
		rm -f "$pid_file"
	done
}
trap 'stop_keyrings; rm -rf "$dir"' EXIT

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

# The made-up session. A keyring that runs is a real process with the
# keyring's name (a copy of sleep), because the script reads the name of
# whoever holds the keyring off /proc. state/pid.<name> holds its pid, and
# state/unlocked exists while the keyring has been given the password.
# state/oo7 says what oo7 does when started: "open" (the password is waiting
# for it), "locked" or "broken".
cp /usr/bin/sleep "$bin/ksecretd"
cp /usr/bin/sleep "$bin/oo7-daemon"
stand_in systemctl '
echo "systemctl $*" >>'"$log"'
case "$*" in
"--user start oo7-daemon.service")
	[ "$(cat '"$state"'/oo7)" != broken ] || exit 1
	'"$bin"'/oo7-daemon 1000 >/dev/null 2>&1 &
	echo $! >'"$state"'/pid.oo7-daemon
	[ "$(cat '"$state"'/oo7)" != open ] || touch '"$state"'/unlocked
	;;
esac'
stand_in pkill '
echo "pkill $*" >>'"$log"'
[ -e '"$state"'/pid.ksecretd ] || exit 1
kill "$(cat '"$state"'/pid.ksecretd)"
rm -f '"$state"'/pid.ksecretd '"$state"'/unlocked'
stand_in busctl '
pid=$(cat '"$state"'/pid.* 2>/dev/null | head -n 1)
case "$*" in
"--user status org.freedesktop.secrets") [ -n "$pid" ] && echo "PID=$pid" ;;
*ReadAlias*) [ -n "$pid" ] && echo "o \"/org/freedesktop/secrets/collection/login\"" ;;
*Locked*) [ -n "$pid" ] && { [ -e '"$state"'/unlocked ] && echo "b false" || echo "b true"; } ;;
esac'
stand_in secret-tool '
[ -e '"$state"'/unlocked ] && [ ! -e '"$state"'/forgetful ] || exit 1
case $1 in
store) cat >'"$state"'/secret ;;
lookup) cat '"$state"'/secret ;;
clear) rm -f '"$state"'/secret ;;
esac'

# The login file as it is on the owner's laptop.
login_file() {
	printf '%s\n' '#%PAM-1.0' '' \
		'auth       requisite    pam_nologin.so' \
		'auth       include      system-local-login' \
		'account    include      system-local-login' \
		'session    include      system-local-login' \
		'password   include      system-local-login' \
		'-auth       optional     pam_kwallet5.so' \
		'-session    optional     pam_kwallet5.so auto_start force_run'
}

# machine <what oo7 does>: logged in, the KDE wallet running and open.
machine() {
	stop_keyrings
	rm -rf "$state" "$log" "$dir/out" "$root/etc/pam.d"/* "$home/.local/share/kwalletd.before-oo7"
	mkdir -p "$state"
	login_file >"$root/etc/pam.d/login"
	chmod 644 "$root/etc/pam.d/login"
	touch "$root/usr/lib/security/pam_oo7.so" "$home/.local/share/kwalletd/kdewallet.kwl"
	"$bin/ksecretd" 1000 >/dev/null 2>&1 &
	echo $! >"$state/pid.ksecretd"
	touch "$state/unlocked"
	echo "$1" >"$state/oo7"
}

run() {
	PATH="$bin:/usr/bin:/bin" KEYRING_ROOT="$root" HOME="$home" XDG_DATA_HOME="$home/.local/share" \
		sh "$tools/keyring.sh" "$@" >"$dir/out" 2>&1
}
said() { grep -q "$1" "$dir/out"; }
called() { grep -qx "$1" "$log" 2>/dev/null; }
pam=$root/etc/pam.d/login

# --- The login file ------------------------------------------------------------

machine open
run || fail "status failed"
said 'The keyring is ksecretd, and it is open' || fail "status does not see the KDE wallet"
said 'Logging in unlocks: the KDE wallet' || fail "status misreads the login file"

run oo7 || fail "oo7 failed"
expected=$(printf '%s\n' '#%PAM-1.0' '' \
	'auth       requisite    pam_nologin.so' \
	'auth       include      system-local-login' \
	'account    include      system-local-login' \
	'session    include      system-local-login' \
	'password   include      system-local-login' \
	'-auth       optional     pam_oo7.so' \
	'-session    optional     pam_oo7.so auto_start' \
	'-password   optional     pam_oo7.so')
[ "$(cat "$pam")" = "$expected" ] || fail "the login file came out as: $(cat "$pam")"
[ "$(stat -c %a "$pam")" = 644 ] || fail "the login file's permissions changed"
[ "$(cat "$pam.before-oo7")" = "$(login_file)" ] || fail "the old login file was not kept"
[ ! -e "$pam.new" ] || fail "a half-written file was left next to the login file"
called 'systemctl --global enable oo7-daemon.service' || fail "oo7 is not set to start at login"
said 'Do not log out or reboot before step 1 has worked' || fail "the warning to test first is missing"
run
said 'Logging in unlocks: oo7' || fail "status misreads the new login file"

# Twice changes nothing more, and the kept file stays the first one.
run oo7 || fail "oo7 a second time failed"
[ "$(cat "$pam")" = "$expected" ] || fail "a second run changed the login file again"
[ "$(cat "$pam.before-oo7")" = "$(login_file)" ] || fail "a second run overwrote the kept file"

# Back: the file is exactly what it was.
run kwallet || fail "kwallet failed"
[ "$(cat "$pam")" = "$(login_file)" ] || fail "going back did not restore the login file"
[ "$(stat -c %a "$pam")" = 644 ] || fail "going back changed the login file's permissions"
called 'systemctl --global disable oo7-daemon.service' || fail "oo7 still starts at login after going back"

# Nothing to go back to: refused, file untouched.
machine open
run kwallet && fail "going back without a kept file was not refused"
[ "$(cat "$pam")" = "$(login_file)" ] || fail "a refused 'kwallet' changed the login file"

# A login file with other lines of somebody's own keeps them, in place.
machine open
printf '%s\n' '# my own' 'session    optional     pam_motd.so' >>"$pam"
run oo7 || fail "oo7 on a file with extra lines failed"
grep -q '^# my own$' "$pam" && grep -q 'pam_motd.so' "$pam" || fail "somebody's own lines were dropped"
grep -q 'pam_kwallet5' "$pam" && fail "the wallet's lines are still there"
[ "$(grep -c 'pam_oo7\.so' "$pam")" = 3 ] || fail "not exactly three lines for oo7"

# oo7 not installed: refused, file untouched.
machine open
rm "$root/usr/lib/security/pam_oo7.so"
run oo7 && fail "oo7 without its login module was not refused"
[ "$(cat "$pam")" = "$(login_file)" ] || fail "a refused 'oo7' changed the login file"
[ ! -e "$log" ] || fail "a refused 'oo7' enabled something"

# --- Switching the running session -------------------------------------------

# Not before the login file is changed.
machine open
run start && fail "start before 'oo7' was not refused"
[ -e "$state/pid.ksecretd" ] || fail "a refused start stopped the KDE wallet"

# The password is waiting (a login on a second console left it): oo7 comes
# up open.
machine open
run oo7
run start || fail "start failed although oo7 came up open"
[ ! -e "$state/pid.ksecretd" ] || fail "the KDE wallet still runs"
[ -e "$state/pid.oo7-daemon" ] || fail "oo7 was not started"
said 'oo7 is the keyring, it is open, and a secret was stored and read back' || fail "start did not report success"
[ ! -e "$state/secret" ] || fail "the test secret was left in the keyring"
[ -e "$home/.local/share/kwalletd.before-oo7/kdewallet.kwl" ] || fail "the wallet's files were not kept"
run
said 'The keyring is oo7-daemon, and it is open' || fail "status does not see oo7"

# oo7 comes up without the password: said so, left running for the login
# that will hand it over, and the next start finds it open.
machine locked
run oo7
run start && fail "start reported success with a locked keyring"
said 'has not been given your login password' || fail "a locked oo7 was not explained"
[ -e "$state/pid.oo7-daemon" ] || fail "a locked oo7 was stopped"
run
said 'it is LOCKED' || fail "status does not say the keyring is locked"
touch "$state/unlocked" # the login on the second console
rm "$log"
run start || fail "start failed after the password arrived"
called 'systemctl --user start oo7-daemon.service' && fail "oo7 was started a second time"
grep -q '^pkill' "$log" 2>/dev/null && fail "start went looking for the KDE wallet again"
said 'a secret was stored and read back' || fail "start did not report success"

# oo7 is open but does not keep what it is given.
machine open
run oo7
touch "$state/forgetful"
run start && fail "start reported success although nothing could be stored"
said 'storing a secret and reading it back failed' || fail "a keyring that stores nothing was not explained"

# oo7 does not start.
machine broken
run oo7
run start && fail "start reported success although oo7 did not start"
said 'oo7 did not start' || fail "a failed start was not explained"
said 'sudo sh tools/keyring.sh kwallet' || fail "a failed start does not say how to go back"

echo "ok: login file changed and restored, refusals, start with the password waiting, locked, forgetful, broken"
