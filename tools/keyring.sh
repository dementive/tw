#!/bin/sh
# Changes which keyring the login unlocks: oo7 in place of the KDE wallet
# (ksecretd), which is the largest helper in a tw session. The keyring is
# what programs such as the Claude app keep their sign-in in.
#
#   sudo sh tools/keyring.sh oo7      logging in unlocks oo7 from now on
#   sh tools/keyring.sh start         switch the running session over, and check
#   sudo sh tools/keyring.sh kwallet  put the login back as it was
#   sh tools/keyring.sh               say which keyring is running
#
# `oo7` edits /etc/pam.d/login, the file that decides what happens when you
# log in on a console, and that the lock screen checks your password through.
# It takes out the two lines for the KDE wallet and adds three for oo7, all
# marked optional, and keeps the old file as /etc/pam.d/login.before-oo7.
# Nothing else in the file is touched. Then test it before you rely on it:
# with this session still open, log in on a second console. If that login
# fails, `kwallet` from the open session puts the file back.
#
# `start` needs no root. It stops ksecretd, starts oo7, and checks that oo7
# has your login password (the second console's login handed it over) and
# that a program can store a secret and read it back.
#
# For the tests: KEYRING_ROOT is put in front of /etc and /usr and lifts the
# need to be root.
set -eu
LC_ALL=C
export LC_ALL

root=${KEYRING_ROOT:-}
pam=$root/etc/pam.d/login
backup=$pam.before-oo7
module=$root/usr/lib/security/pam_oo7.so
name=org.freedesktop.secrets

say() { printf '%s\n' "$*"; }
fail() {
	say "$*" >&2
	exit 1
}
need_root() {
	[ -n "$root" ] || [ "$(id -u)" = 0 ] || fail "This changes how logging in works: run it with sudo."
}
not_root() {
	[ -n "$root" ] || [ "$(id -u)" != 0 ] || fail "Run this one as yourself, without sudo: it works on your own session."
}

# Who serves the keyring in this session: the program's name, or nothing.
owner() {
	pid=$(busctl --user status "$name" 2>/dev/null | sed -n 's/^PID=//p')
	[ -z "$pid" ] || cat "/proc/$pid/comm" 2>/dev/null || true
}

# The keyring programs use by default: its path on the bus, or nothing.
default_keyring() {
	busctl --user call "$name" /org/freedesktop/secrets org.freedesktop.Secret.Service \
		ReadAlias s default 2>/dev/null | sed -n 's/^o "\(.*\)"$/\1/p' | grep -v '^/$' || true
}

# Whether that keyring is open: "false" from the bus means not locked.
unlocked() {
	keyring=$(default_keyring)
	[ -n "$keyring" ] && [ "$(busctl --user get-property "$name" "$keyring" \
		org.freedesktop.Secret.Collection Locked 2>/dev/null)" = "b false" ]
}

# A secret stored, read back and removed, the way a program does it.
works() {
	printf 'kept' | secret-tool store --label='tw keyring check' tw-keyring-check 1 2>/dev/null || return 1
	[ "$(secret-tool lookup tw-keyring-check 1 2>/dev/null)" = kept ] || return 1
	secret-tool clear tw-keyring-check 1 2>/dev/null || true
}

status() {
	serving=$(owner)
	if [ -z "$serving" ]; then
		say "No keyring is running in this session."
	elif unlocked; then
		say "The keyring is $serving, and it is open."
	else
		say "The keyring is $serving, and it is LOCKED: it has not been given your login password."
	fi
	if grep -q 'pam_oo7\.so' "$pam" 2>/dev/null; then
		say "Logging in unlocks: oo7."
	elif grep -q 'pam_kwallet5\.so' "$pam" 2>/dev/null; then
		say "Logging in unlocks: the KDE wallet."
	else
		say "Logging in unlocks: no keyring."
	fi
}

to_oo7() {
	need_root
	[ -e "$module" ] || fail "oo7 is not installed: sudo pacman -S oo7"
	[ -f "$pam" ] || fail "$pam is not there."
	if grep -q 'pam_oo7\.so' "$pam"; then
		say "$pam already has the lines for oo7."
	else
		[ -e "$backup" ] || cp -p "$pam" "$backup"
		# The new file next to the old one, with the old one's owner and
		# mode, then moved over it in one step: at no moment is there half a
		# file for a login to read.
		new=$pam.new
		cp -p "$pam" "$new"
		{
			grep -v 'pam_kwallet5\.so' "$pam" || true
			printf '%s\n' \
				'-auth       optional     pam_oo7.so' \
				'-session    optional     pam_oo7.so auto_start' \
				'-password   optional     pam_oo7.so'
		} >"$new"
		# What was there, less the wallet's lines, plus exactly three.
		[ "$(grep -c 'pam_oo7\.so' "$new")" = 3 ] &&
			[ "$(grep -v 'pam_oo7\.so' "$new")" = "$(grep -v 'pam_kwallet5\.so' "$pam")" ] || {
			rm -f "$new"
			fail "The new $pam did not come out as meant. Nothing was changed."
		}
		mv "$new" "$pam"
		say "$pam now unlocks oo7. The old file is $backup."
	fi
	# oo7 starts with the first login of every user from now on.
	systemctl --global enable oo7-daemon.service
	say ""
	say "Now test it, with this session left open:"
	say "  1. Press Ctrl+Alt+F3, log in, type 'exit', and come back with Ctrl+Alt+F1."
	say "     If that login fails: sudo sh tools/keyring.sh kwallet"
	say "  2. sh tools/keyring.sh start"
	say "Do not log out or reboot before step 1 has worked."
}

to_kwallet() {
	need_root
	[ -e "$backup" ] || fail "There is no $backup to put back."
	cp -p "$backup" "$pam.new"
	mv "$pam.new" "$pam"
	systemctl --global disable oo7-daemon.service
	say "$pam is as it was before oo7. Log out and in again: the KDE wallet is then"
	say "the keyring once more. A program that signed in since has to sign in again."
}

start() {
	not_root
	grep -q 'pam_oo7\.so' "$pam" 2>/dev/null || fail "Logging in does not unlock oo7 yet. First: sudo sh tools/keyring.sh oo7"
	command -v secret-tool >/dev/null 2>&1 || fail "secret-tool is not installed: sudo pacman -S libsecret"
	if [ "$(owner)" != oo7-daemon ]; then
		# oo7 may take the wallet's entries over; keep the wallet's files
		# as they are now in case you go back.
		wallet=${XDG_DATA_HOME:-$HOME/.local/share}/kwalletd
		if [ -d "$wallet" ] && [ ! -e "$wallet.before-oo7" ]; then
			cp -a "$wallet" "$wallet.before-oo7"
		fi
		# The KDE wallet holds the keyring's name on the bus. A program that
		# runs now has read what it needs already and goes on working.
		pkill -x -u "$(id -u)" ksecretd 2>/dev/null || true
		tries=0
		while [ -n "$(owner)" ] && [ "$tries" -lt 50 ]; do
			sleep 0.1
			tries=$((tries + 1))
		done
		systemctl --user start oo7-daemon.service ||
			fail "oo7 did not start: journalctl --user -u oo7-daemon -n 20. To go back: sudo sh tools/keyring.sh kwallet, then log out and in."
	fi
	[ "$(owner)" = oo7-daemon ] || fail "oo7 is not what serves the keyring (it is: $(owner)). To go back: sudo sh tools/keyring.sh kwallet, then log out and in."
	if ! unlocked; then
		say "oo7 is running but has not been given your login password yet."
		say "Log in once on a second console (Ctrl+Alt+F3, log in, 'exit', Ctrl+Alt+F1),"
		say "then run this again."
		exit 1
	fi
	works || fail "oo7 is open, but storing a secret and reading it back failed: journalctl --user -u oo7-daemon -n 20"
	say "oo7 is the keyring, it is open, and a secret was stored and read back."
	say ""
	say "Next:"
	say "  1. Lock the screen (Mod+Alt+L) and unlock it. If the password is refused,"
	say "     Ctrl+Alt+F3 still logs in: sudo sh tools/keyring.sh kwallet from there."
	say "  2. Quit the Claude app and open it again. It may ask you to sign in once."
}

case ${1:-status} in
oo7) to_oo7 ;;
kwallet) to_kwallet ;;
start) start ;;
status) status ;;
*) fail "usage: $0 [oo7|start|kwallet]" ;;
esac
