#!/bin/sh
# tw-launch: which desktop entries it offers, in what order, what it starts
# and how, how often each was started, one launcher at a time, and --dmenu.
# The entries are made up, under the test's own directory, and what they
# start are stand-ins that write down how they were started.
#
# $3 = tw-test-keys, $4 = tw-launch.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3
launch_bin=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")

data=$run_dir/data/applications
own=$run_dir/home/applications
started=$run_dir/started
mkdir -p "$data/sub" "$own" "$run_dir/cache"

entry() { # <file under $data or $own> <lines...>
	file=$1
	shift
	{
		echo '[Desktop Entry]'
		echo 'Type=Application'
		for line; do echo "$line"; done
		echo '[Desktop Action new-window]'
		echo 'Name=Not this one'
		echo 'Exec=wrong'
	} >"$file"
}
entry "$data/firefox.desktop" 'Name=Firefox' 'GenericName=Web Browser' \
	'Keywords=internet;www;' 'Exec=firefox %u'
entry "$data/godot.desktop" 'Name=Godot Engine' 'Exec=godot --flag "a b" %f' \
	'PrefersNonDefaultGPU=true'
entry "$data/htop.desktop" 'Name=Htop' 'Exec=htop' 'Terminal=true'
entry "$data/sub/tool.desktop" 'Name=Tool' 'Exec=tool %c 100%%'
# Not offered: hidden, for another desktop, not installed, not a program,
# and one the user's own entry of the same name hides.
entry "$data/secret.desktop" 'Name=Secret' 'Exec=secret' 'NoDisplay=true'
entry "$data/kde.desktop" 'Name=KDE Settings' 'Exec=kcm' 'OnlyShowIn=KDE;'
entry "$data/gone.desktop" 'Name=Gone' 'Exec=gone' 'TryExec=gone'
printf '[Desktop Entry]\nType=Link\nName=Website\nURL=https://example.org\n' \
	>"$data/link.desktop"
entry "$data/foot.desktop" 'Name=Foot' 'Exec=foot'
entry "$own/foot.desktop" 'Name=Foot' 'Exec=foot' 'Hidden=true'

# Stand-ins that write down their arguments, one in brackets each.
for program in firefox tool footclient tw-gpu; do
	fake_program "$program" "{ printf '$program'; for a; do printf ' [%s]' \"\$a\"; done; echo; } >>'$started'"
done

# launch <name> [arguments]: tw-launch -d; what it prints goes to
# $run_dir/<name>.out, what it says to $run_dir/<name>.log, its pid is in
# $launcher. Waits until it has the keyboard.
launch() {
	name=$1
	shift
	env XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 PATH="$bin_dir" \
		HOME="$run_dir" XDG_DATA_HOME="$run_dir/home" XDG_DATA_DIRS="$run_dir/data" \
		XDG_CACHE_HOME="$run_dir/cache" XDG_CURRENT_DESKTOP=tw \
		"$launch_bin" -d "$@" >"$run_dir/$name.out" 2>"$run_dir/$name.log" <"${input:-/dev/null}" &
	launcher=$!
	wait_until "$name to have the keyboard" grep -qx keyboard "$run_dir/$name.log"
}
said() { grep -qx "$2" "$run_dir/$1.log"; } # <name> <line>
gone() { ! kill -0 "$1" 2>/dev/null; }
# finished <pid> <status>: it exited with that status.
finished() {
	wait_until "the launcher to leave" gone "$1"
	code=0
	wait "$1" || code=$?
	[ "$code" = "$2" ] || fail "the launcher left with status $code, not $2"
}
last_started() { tail -n 1 "$started"; }

start_tw

# What it offers, by name with nothing typed, and what it starts.
launch one
said one "matches 4: Firefox|Godot Engine|Htop|Tool" || fail "offered: $(grep '^matches' "$run_dir/one.log")"
press f i r Return
finished "$launcher" 0
[ "$(last_started)" = "firefox" ] || fail "started '$(last_started)', not firefox without the %u"
grep -q "^1 firefox.desktop$" "$run_dir/cache/tw-launch" || fail "firefox's start was not counted"

# Field codes: %c is the name, %% a percent sign. Started twice, Tool comes
# first among equal matches; Firefox, started once, next.
for n in 1 2; do
	launch tool$n
	press t o o l Return
	finished "$launcher" 0
	[ "$(last_started)" = "tool [Tool] [100%]" ] || fail "started '$(last_started)'"
done
launch order
said order "matches 4: Tool|Firefox|Godot Engine|Htop" ||
	fail "most started first: $(grep '^matches' "$run_dir/order.log")"

# Its kind and its keywords find a program. With nothing matching, Return
# does nothing; BackSpace takes the last character back, and Escape leaves
# without starting anything.
press b r o w s e r
wait_until "the kind of program to find Firefox" said order "matches 1: Firefox"
press ctrl+u q
wait_until "nothing to match q" said order "matches 0:"
mark=$(lines order)
all_back() { since order "$mark" | grep -q '^matches 4: '; }
press Return BackSpace
wait_until "BackSpace to bring them all back" all_back
count=$(grep -c . "$started")
press Escape
finished "$launcher" 1
[ "$(grep -c . "$started")" = "$count" ] || fail "Return with no match or Escape started something"

# A program that wants the discrete GPU goes through tw-gpu, quoted
# arguments kept whole; one that wants a terminal gets foot's.
launch godot
press g o d o t Return
finished "$launcher" 0
[ "$(last_started)" = "tw-gpu [run] [godot] [--flag] [a b]" ] || fail "started '$(last_started)'"
launch htop
press h t o p Return
finished "$launcher" 0
[ "$(last_started)" = "footclient [-E] [-N] [--] [htop]" ] || fail "started '$(last_started)'"

# The key that opened it closes it: a second launcher ends the first.
launch first
first=$launcher
env XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 "$launch_bin" -d \
	2>"$run_dir/second.log" </dev/null || fail "the second launcher failed"
said second "closed the one that was open" || fail "the second launcher did not close the first"
wait_until "the first launcher to go" gone "$first"

# --dmenu: picks one of the lines on stdin and prints it; Escape prints
# nothing, which the screen-sharing portal takes as no.
printf 'Monitor: HEADLESS-1\nWindow: foot (foot)\n' >"$run_dir/choices"
input=$run_dir/choices launch pick --dmenu -p 'Share: '
said pick "matches 2: Monitor: HEADLESS-1|Window: foot (foot)" || fail "offered: $(grep '^matches' "$run_dir/pick.log")"
press w Return
finished "$launcher" 0
[ "$(cat "$run_dir/pick.out")" = "Window: foot (foot)" ] || fail "printed '$(cat "$run_dir/pick.out")'"
input=$run_dir/choices launch decline --dmenu
press Escape
finished "$launcher" 1
[ ! -s "$run_dir/decline.out" ] || fail "Escape printed '$(cat "$run_dir/decline.out")'"

stop_tw
for log in "$run_dir"/*.log; do
	! grep -q 'Sanitizer\|runtime error' "$log" || fail "sanitizer report in $(basename "$log")"
done

echo "ok: what is offered, what is started and how, the most started first, one at a time, --dmenu"
