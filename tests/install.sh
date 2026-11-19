#!/bin/sh
# data/post-install.sh, which `meson install` runs once the files are in
# place, against stand-ins for id, systemctl and udevadm: what it reloads
# when the install was a real one made as root, and that it keeps its hands
# off in every other case. What it cannot show: that the real systemd,
# logind, udev and polkit then go by the new files.
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

# install [VAR=value...] [-- file...]: runs the step as `meson install`
# would, as root unless $tmp/uid says otherwise, with the files of options
# now off as its arguments. Leaves what it ran in $log.
install() {
	: >"$log"
	vars=
	while [ $# -gt 0 ] && [ "$1" != -- ]; do
		vars="$vars $1"
		shift
	done
	[ $# -gt 0 ] && shift
	# shellcheck disable=SC2086 # the words are VAR=value pairs
	env -i PATH="$bin:$PATH" MESON_INSTALL_PREFIX="$tmp/prefix" $vars \
		sh "$data/post-install.sh" "$@" >"$tmp/out" 2>"$tmp/err"
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
ran 'systemctl kill --kill-whom=main --signal=SIGHUP systemd-logind.service' ||
	fail "logind was not told to read what the power key does"
[ "$(grep -c . "$log")" = 5 ] || fail "it ran something else as well"
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

# --- What an earlier install left for an option now off -------------------------
mkdir -p "$tmp/prefix/lib/modprobe.d" "$tmp/prefix/bin"
: >"$tmp/prefix/lib/modprobe.d/tw-bluetooth.conf"
: >"$tmp/prefix/bin/tw-notify"
: >"$tmp/outside"
install SUDO_USER=somebody -- "$tmp/prefix/lib/modprobe.d/tw-bluetooth.conf" \
	"$tmp/prefix/bin/tw-notify" "$tmp/prefix/bin/never-installed" "$tmp/outside" ||
	fail "the step failed with files to remove"
[ ! -e "$tmp/prefix/lib/modprobe.d/tw-bluetooth.conf" ] && [ ! -e "$tmp/prefix/bin/tw-notify" ] ||
	fail "a file of an option now off was left in place"
[ -e "$tmp/outside" ] || fail "it removed a file outside the install prefix"
grep -q 'removed .*tw-bluetooth.conf' "$tmp/out" || fail "a removal went unsaid"
grep -q 'never-installed' "$tmp/out" "$tmp/err" && fail "it spoke of a file that was not there"
ran 'systemctl daemon-reload' || fail "systemd was not reloaded after the removal"
: >"$tmp/prefix/bin/tw-notify"
install DESTDIR="$tmp/stage" -- "$tmp/prefix/bin/tw-notify" || fail "the step failed for a staged install"
[ -e "$tmp/prefix/bin/tw-notify" ] || fail "a staged install removed a file of the running system"

# --- A reload that fails does not fail the install ------------------------------
: >"$tmp/broken"
install SUDO_USER=somebody || fail "a failed reload failed the install"
[ "$(grep -c . "$log")" = 5 ] || fail "it gave up after the first reload that failed"
grep -q 'could not reload' "$tmp/err" || fail "a failed reload went unsaid"
rm "$tmp/broken"

echo "ok: the step after meson install reloads systemd, polkit, udev, logind and the user's systemd, and only for a real install"
