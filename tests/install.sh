#!/bin/sh
# data/post-install.sh, which `meson install` runs once the files are in
# place, against stand-ins for id, systemctl and udevadm: what it reloads
# when the install was a real one made as root, and that it keeps its hands
# off in every other case. What it cannot show: that the real systemd, udev
# and polkit then go by the new files.
# $1 = the data/ directory of the source tree.
set -eu
data=$1
tmp=$(mktemp -d "$PWD/install-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
bin=$tmp/bin
log=$tmp/log
mkdir -p "$bin"

fail() {
	echo "FAIL: $*" >&2
	[ -s "$log" ] && sed 's/^/  log: /' "$log" >&2
	[ -s "$tmp/err" ] && sed 's/^/  said: /' "$tmp/err" >&2
	exit 1
}

# Who runs the install: the number in $tmp/uid.
cat >"$bin/id" <<EOF
#!/bin/sh
cat '$tmp/uid'
EOF
for tool in systemctl udevadm; do
	cat >"$bin/$tool" <<EOF
#!/bin/sh
echo "$tool \$*" >>'$log'
[ ! -e '$tmp/broken' ]
EOF
done
chmod +x "$bin/id" "$bin/systemctl" "$bin/udevadm"

# install [VAR=value...]: runs the step as `meson install` would, as root
# unless $tmp/uid says otherwise. Leaves what it ran in $log.
install() {
	: >"$log"
	env -i PATH="$bin:$PATH" "$@" sh "$data/post-install.sh" >"$tmp/out" 2>"$tmp/err"
}
ran() { grep -qx "$1" "$log"; }

echo 0 >"$tmp/uid"

# --- A real install, made with sudo ---------------------------------------------
install SUDO_USER=somebody || fail "the step failed"
ran 'systemctl daemon-reload' || fail "systemd was not told to read the new units"
ran 'systemctl try-restart polkit.service' || fail "polkit was not made to read the new rules"
ran 'udevadm control --reload' || fail "udev was not told to read the new rules"
ran 'systemctl --machine=somebody@.host --user daemon-reload' ||
	fail "the user's systemd was not told to read the new drop-in"
[ "$(grep -c . "$log")" = 4 ] || fail "it ran something else as well"
[ ! -s "$tmp/err" ] || fail "it complained although everything worked"

# --- Made by root itself: there is no user to reload for ------------------------
install || fail "the step failed without SUDO_USER"
ran 'systemctl daemon-reload' || fail "systemd was not reloaded for root's own install"
! grep -q -- '--user' "$log" || fail "it reloaded a user's systemd with no user named"
install SUDO_USER=root || fail "the step failed with SUDO_USER=root"
! grep -q -- '--user' "$log" || fail "it took root for the user whose session it is"

# --- Every case in which it must keep its hands off -----------------------------
install DESTDIR="$tmp/stage" SUDO_USER=somebody || fail "the step failed for a staged install"
[ ! -s "$log" ] || fail "a staged install reloaded the running system"
install MESON_INSTALL_DRY_RUN=1 SUDO_USER=somebody || fail "the step failed for a dry run"
[ ! -s "$log" ] || fail "a dry run reloaded the running system"
echo 1000 >"$tmp/uid"
install SUDO_USER=somebody || fail "the step failed without root"
[ ! -s "$log" ] || fail "an install without root tried to reload the system"
echo 0 >"$tmp/uid"

# --- A reload that fails does not fail the install ------------------------------
: >"$tmp/broken"
install SUDO_USER=somebody || fail "a failed reload failed the install"
[ "$(grep -c . "$log")" = 4 ] || fail "it gave up after the first reload that failed"
grep -q 'could not reload' "$tmp/err" || fail "a failed reload went unsaid"
rm "$tmp/broken"

echo "ok: the step after meson install reloads systemd, polkit, udev and the user's systemd, and only for a real install"
