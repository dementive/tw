#!/bin/sh
# The clipboard. On Wayland a copy stays in the program it was made in and
# is gone when that program exits; tw reads each copy as it is made and
# offers it in the program's place once the program is gone (clipboard.c).
#
# The programs here copy and paste through the data-control protocol, as
# clipboard tools do (tests/clip.c). Windows copy and paste through another
# one, which meets this one inside wlroots, before tw; that side is not
# covered here.
#
# $3 = tw-test-clip.
set -eu
. "$(dirname "$0")/lib.sh"
clip_bin=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")

clip() {
	XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 "$clip_bin" "$@"
}
# copy <name> <format=content>...: a program copies and keeps running. Its
# pid is in $copier, what it says in $run_dir/<name>.log.
copy() {
	start_copy "$@"
	wait_until "$1 to have copied" grep -qx ready "$run_dir/$1.log"
}
# The same without waiting for the copy to be made, for a program that may
# not live to say so.
start_copy() {
	name=$1
	shift
	# Not through clip(): $! must be the program, to be killed, not a shell.
	XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 \
		"$clip_bin" copy "$@" >"$run_dir/$name.log" 2>&1 &
	copier=$!
}
# The program exits, as when its window is closed.
quit() {
	kill "$1" 2>/dev/null || true
	wait "$1" 2>/dev/null || true
}
gone() { ! kill -0 "$1" 2>/dev/null; }
formats() { clip paste | sed -n 's/^formats: //p'; }
formats_are() { [ "$(formats)" = "$1" ]; }
# pasted <format> <line>: a program pasting that format gets this.
pasted() { [ "$(clip paste "$1" | grep -v '^formats:')" = "$2" ]; }
# A reader that waits a second before it reads; its pid is $!.
slow_paste() {
	XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY=wayland-0 \
		"$clip_bin" paste-slow text/plain >"$run_dir/slow.log" 2>&1 &
}
tw_said() { grep -q "$1" "$run_dir/tw.log"; }
tw_said_times() { [ "$(grep -c "$1" "$run_dir/tw.log")" = "$2" ]; } # <text> <how often>
said() { grep -qx "$2" "$run_dir/$1.log"; } # <name> <line>
count() { grep -c "$2" "$run_dir/$1.log" || true; }
# What a copier said its content was, in the words of a paste.
content() { sed -n "s|^content $2: ||p" "$run_dir/$1.log"; }
# tw does not offer again what a moment ago it did not: checked after it has
# had time to.
still_nothing() {
	sleep 0.3
	[ "$(clip paste)" = nothing ]
}

# Programs tw starts must not inherit that tw ignores a broken pipe: the
# startup command notes which signals it was started ignoring.
startup='while read -r key value; do [ "$key" = SigIgn: ] && echo "$value"; done </proc/self/status >"$XDG_RUNTIME_DIR/ignored"'
start_tw -d -s "$startup"
wait_until "the startup command to run" test -s "$run_dir/ignored"
[ "$((0x$(cat "$run_dir/ignored") & 0x1000))" = 0 ] ||
	fail "a program tw starts ignores SIGPIPE (signals ignored: $(cat "$run_dir/ignored"))"

# --- An empty clipboard -------------------------------------------------------
[ "$(clip paste)" = nothing ] || fail "the clipboard is not empty to begin with"

# --- A copy is read as it is made, and stays with its program -----------------
# Three of the four formats hold the same five bytes, as text does: tw keeps
# them once.
copy a 'text/plain;charset=utf-8=hello' 'text/plain=hello' 'UTF8_STRING=hello' \
	'x-test/other=something else'
wait_until "tw to have read the copy" tw_said 'clipboard: kept 4 formats in 19 bytes'
for format in 'text/plain;charset=utf-8' text/plain UTF8_STRING x-test/other; do
	said a "sent $format" || fail "tw did not read $format"
done
[ "$(count a '^sent')" = 4 ] || fail "tw read a format twice"
pasted text/plain 'paste text/plain: hello' || fail "paste while the program runs"
[ "$(count a '^sent text/plain$')" = 2 ] ||
	fail "a paste while the program runs was not answered by the program"
said a cancelled && fail "tw took the clipboard from a program that is running"

# --- The program exits: the copy is still there -------------------------------
quit "$copier"
wait_until "the copy to be offered when its program is gone" \
	pasted text/plain 'paste text/plain: hello'
[ "$(formats)" = 'text/plain;charset=utf-8 text/plain UTF8_STRING x-test/other' ] ||
	fail "the formats are not the program's, in its order: $(formats)"
pasted UTF8_STRING 'paste UTF8_STRING: hello' || fail "a format that shares its bytes"
pasted x-test/other 'paste x-test/other: something else' || fail "the format with other bytes"
pasted image/png 'no image/png' || fail "a format the program never offered"
# What tw offers itself it does not read a second time.
tw_said_times 'clipboard: kept' 1 || fail "tw read the copy it offers itself"

# --- The next copy takes its place --------------------------------------------
copy b 'text/plain=the second'
pasted text/plain 'paste text/plain: the second' || fail "a new copy while tw offers the last"
wait_until "tw to have read the second copy" tw_said 'clipboard: kept 1 formats in 10 bytes'
quit "$copier"
wait_until "the second copy to be offered" pasted text/plain 'paste text/plain: the second'
[ "$(formats)" = text/plain ] || fail "formats of the first copy came back: $(formats)"

# --- An emptied clipboard stays empty -----------------------------------------
# A program that empties the clipboard means it (a password manager after
# its time is up). First the copy tw offers, then one a program still has.
[ "$(clip clear)" = cleared ] || fail "could not empty the clipboard"
still_nothing || fail "tw offered a copy again that was taken off the clipboard"
copy c 'text/plain=a secret'
wait_until "tw to have read the secret" tw_said 'clipboard: kept 1 formats in 8 bytes'
[ "$(clip clear)" = cleared ] || fail "could not empty the clipboard"
wait_until "the program to hear that its copy is off the clipboard" said c cancelled
quit "$copier"
still_nothing || fail "tw kept a copy that was taken off the clipboard"

# A password manager marks what it copies as secret. tw does not read it at
# all, so it is gone with its program, whether or not that program got to
# empty the clipboard first.
copy pw 'x-kde-passwordManagerHint=secret' 'text/plain=hunter2'
wait_until "tw to leave the secret alone" tw_said 'clipboard: a secret, left with its program'
pasted text/plain 'paste text/plain: hunter2' || fail "the secret could not be pasted from its program"
quit "$copier"
still_nothing || fail "tw kept a copy marked secret after its program was gone"

# --- A long copy --------------------------------------------------------------
# More than a pipe holds, so tw reads it in pieces and, once its program is
# gone, writes it in pieces as the reader takes them.
copy d 'text/plain=@3000000' 'x-test/short=short'
wait_until "tw to have read the long copy" tw_said 'clipboard: kept 2 formats in 3000005 bytes'
long=$(content d text/plain)
quit "$copier"
wait_until "the long copy to be offered" pasted text/plain "paste text/plain: $long"
# A reader that takes its time does not hold tw up: another is served
# while the first has not read a byte.
slow_paste
slow=$!
pasted x-test/short 'paste x-test/short: short' || fail "nothing else is served during a slow paste"
grep -q '^paste' "$run_dir/slow.log" && fail "the slow reader was not slow: nothing was shown"
wait "$slow" || fail "the slow reader failed"
said slow "paste text/plain: $long" || fail "a slow reader got $(grep '^paste' "$run_dir/slow.log")"
# Readers that leave: after the first bytes, and before any. Writing to the
# second one's pipe is what kills a program that does not expect it.
[ "$(clip paste-part text/plain | grep -c '^left after')" = 1 ] || fail "paste-part"
[ "$(clip paste-none text/plain | grep -c '^left$')" = 1 ] || fail "paste-none"
pasted x-test/short 'paste x-test/short: short' || fail "tw did not survive a reader that left"
pasted text/plain "paste text/plain: $long" || fail "the copy is damaged after a reader left"

# --- The most tw keeps --------------------------------------------------------
limit=$(($(sed -n 's/^static const size_t clipboard_keep_bytes = \(.*\);$/\1/p' \
	"$(dirname "$0")/../src/config.h")))
[ "$limit" -gt 0 ] || fail "could not read clipboard_keep_bytes from config.h"
# Exactly that much is kept.
copy e "x-test/big=@$limit"
wait_until "tw to have read a copy of the largest size" \
	tw_said "clipboard: kept 1 formats in $limit bytes"
big=$(content e x-test/big)
quit "$copier"
wait_until "the largest copy to be offered" pasted x-test/big "paste x-test/big: $big"
# One byte more, in a second format, and the copy is left with its program:
# pasted from it while it runs, gone with it.
copy f "x-test/big=@$limit" 'x-test/more=z'
wait_until "tw to give up on a copy that is too large" tw_said 'clipboard: more than'
pasted x-test/more 'paste x-test/more: z' || fail "a copy tw does not keep cannot be pasted"
quit "$copier"
still_nothing || fail "tw offers a copy that is larger than it keeps"

# --- A format with nothing in it ----------------------------------------------
# Its program had nothing to give for it: it is not offered as an empty one.
copy g 'text/plain=some text' 'x-test/empty=' 'x-test/last=the last'
wait_until "tw to have read a copy with an empty format" \
	tw_said 'clipboard: kept 2 formats in 17 bytes'
quit "$copier"
wait_until "the copy to be offered without its empty format" formats_are 'text/plain x-test/last'

# --- A program that dies while a format is on its way -------------------------
# What arrived of that format is not all of it, and the pipe does not say
# so: it ends as it does when the program is done. The program dies in the
# second of three formats, with its pipe closed after its connection to tw
# and then before it; the kernel does the latter, and then tw has taken
# the format as read and asked for the third before it learns of the death.
for death in %die0 %die; do
	start_copy h 'text/plain=whole' "x-test/half=$death:100000" 'x-test/after=never read'
	wait_until "the program to die ($death)" gone "$copier"
	wait_until "what was read in full to be offered ($death)" \
		pasted text/plain 'paste text/plain: whole'
	formats_are text/plain ||
		fail "tw offers a format its program died writing ($death): $(formats)"
	[ "$(clip clear)" = cleared ] || fail "could not empty the clipboard"
done
# In the first format: nothing of the copy is left to offer.
start_copy h 'x-test/half=%die:100000' 'text/plain=never read'
wait_until "the program to die in its first format" gone "$copier"
still_nothing || fail "tw offers a copy whose program died writing its first format: $(formats)"

# --- A program that never answers ---------------------------------------------
# tw waits for the format without being held up, and reads none after it.
# When the program is gone, the formats read before are offered: the pipe
# of the unanswered format ends with its program, which is at least a sign
# that the program had taken the request, and so had outlived the format
# before.
copy i 'text/plain=answered' 'x-test/second=the second' 'x-test/hang=%hang' 'x-test/late=late'
wait_until "tw to ask for the format that is never answered" said i 'asked x-test/hang'
pasted text/plain 'paste text/plain: answered' || fail "tw is held up by a program that does not answer"
quit "$copier"
wait_until "the formats read before to be offered" pasted text/plain 'paste text/plain: answered'
formats_are 'text/plain x-test/second' || fail "not the two formats that were read: $(formats)"

# --- A second program copies while the first still runs -----------------------
# The everyday case: the first program's copy is dropped, also in the
# middle of being read (it does not answer for its second format).
copy l 'text/plain=from the first' 'x-test/hang=%hang'
wait_until "tw to be reading the first program's copy" said l 'asked x-test/hang'
copy m 'text/plain=from the second one'
wait_until "the first program to hear that its copy is off the clipboard" said l cancelled
wait_until "tw to have read the second program's copy" \
	tw_said 'clipboard: kept 1 formats in 19 bytes'
quit "$copier"
wait_until "the second program's copy to be offered" \
	pasted text/plain 'paste text/plain: from the second one'

# --- Leaving ------------------------------------------------------------------
# Whatever tw holds as it exits is freed; a sanitizer build says so if not.
# With a copy on offer and a paste under way:
copy n 'text/plain=@3000000'
wait_until "tw to have read the last copy" tw_said 'clipboard: kept 1 formats in 3000000 bytes'
quit "$copier"
wait_until "the last copy to be offered" formats_are text/plain
slow_paste
slow=$!
wait_until "the paste to be under way" pasted text/plain "paste text/plain: $long"
stop_tw
wait "$slow" 2>/dev/null || true

# With a copy read in full whose program is still there:
start_tw -d
copy o 'text/plain=kept'
wait_until "tw to have read the copy" tw_said 'clipboard: kept 1 formats in 4 bytes'
stop_tw
quit "$copier"

# And in the middle of reading one:
start_tw -d
copy p 'text/plain=kept' 'x-test/hang=%hang'
wait_until "tw to be waiting for a format" said p 'asked x-test/hang'
stop_tw
quit "$copier"

echo "ok: copies outlive their programs, cleared ones do not, long, oversized and broken copies"
