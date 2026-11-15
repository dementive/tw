#!/bin/sh
# Run by `meson install` once the files are in place: has systemd, udev and
# polkit read what was installed, so that nothing has to be reloaded by hand
# before the new units, rules and drop-ins count.
#
# Does nothing for a staged install (DESTDIR), a dry run, or an install made
# without root, and never fails the install: what it could not do is said,
# and a reboot does the same.
set -u

[ -z "${DESTDIR:-}" ] || exit 0
[ -z "${MESON_INSTALL_DRY_RUN:-}" ] || exit 0
[ "$(id -u)" = 0 ] || exit 0
command -v systemctl >/dev/null 2>&1 || exit 0

attempt() { # <what it is for> <command...>
	what=$1
	shift
	"$@" 2>/dev/null || echo "tw: could not reload $what ($*); a reboot does it too" >&2
}

# The units tw-gpu, tw-power and tw-bluetooth start, and the line added to
# the Bluetooth service.
attempt "systemd's units" systemctl daemon-reload
# The rules that let those units be started without a password. polkit reads
# new rules by itself, but only in directories that were there as it started,
# which /usr/local's is not before the first install.
attempt "polkit's rules" systemctl try-restart polkit.service
if command -v udevadm >/dev/null 2>&1; then
	attempt "udev's rules" udevadm control --reload
fi
# The line added to the accessibility bus's unit is read by the systemd of
# whoever is logged in: the user who ran sudo.
user=${SUDO_USER:-}
if [ -n "$user" ] && [ "$user" != root ]; then
	attempt "$user's systemd units" systemctl --machine="$user@.host" --user daemon-reload
fi
exit 0
