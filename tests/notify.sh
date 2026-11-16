#!/bin/sh
# tw-notify, the notification service: started by a session bus when a
# program sends a notification, shown by a headless tw, gone when the last
# one has closed. The bus is a private dbus-daemon and the programs are
# busctl calls, so this also shows that tw-notify's own reading and writing
# of D-Bus messages agrees with two real implementations.
#
# Not covered: what the notifications look like. tw-notify says what it
# draws (-d) and the test reads that.
set -eu
. "$(dirname "$0")/lib.sh"
keys_bin=$3
notify_bin=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")

command -v dbus-daemon >/dev/null 2>&1 && command -v busctl >/dev/null 2>&1 || {
	echo "skipped: needs dbus-daemon and busctl"
	exit 77
}

# A time that is not a number is refused: read as 0 it would mean "until
# clicked" for every notification.
# (With no bus to reach, so that one that takes the option ends there.)
for bad in '-t soon' '-t 2s' '-t -5' '-t' '-x'; do
	# shellcheck disable=SC2086 # two words on purpose
	if env -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$run_dir" "$notify_bin" $bad 2>&1 |
		grep -q '^usage:'; then :; else
		fail "tw-notify took '$bad' for an option"
	fi
done

bus=unix:path=$run_dir/bus
bus_pid=

# start_bus <Wayland display>: a session bus that starts tw-notify for the
# first notification, with a timeout of 300 ms for the ones that name none.
# What tw-notify says ends up in bus.log, next to the bus's own lines.
start_bus() {
	mkdir -p "$run_dir/services"
	printf '[D-BUS Service]\nName=org.freedesktop.Notifications\nExec=%s -d -t 300\n' \
		"$notify_bin" >"$run_dir/services/org.freedesktop.Notifications.service"
	cat >"$run_dir/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>$bus</listen>
  <servicedir>$run_dir/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
	env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$run_dir" WAYLAND_DISPLAY="$1" \
		dbus-daemon --config-file="$run_dir/bus.conf" --nofork >>"$run_dir/bus.log" 2>&1 &
	bus_pid=$!
	client_pids="$client_pids $bus_pid"
	wait_until "the bus" test -S "$run_dir/bus"
}

stop_bus() {
	kill "$bus_pid"
	wait "$bus_pid" 2>/dev/null || true
	rm -f "$run_dir/bus"
}

call() {
	busctl --address="$bus" call org.freedesktop.Notifications \
		/org/freedesktop/Notifications org.freedesktop.Notifications "$@"
}

# note <replaces> <title> <text> <actions and hints> <timeout>: sends a
# notification and prints its id. Actions and hints as busctl takes arrays.
note() {
	# shellcheck disable=SC2086
	call Notify 'susssasa{sv}i' test "$1" '' "$2" "$3" $4 -- "$5" | sed 's/^u //'
}
plain='0 0'
clickable='2 default Open 0' # a click is to be reported to the program
urgent='0 1 urgency y 2'

logged() { grep -q "$1" "$run_dir/bus.log"; }
# Whether tw-notify is running, which is whether the name has an owner.
running() {
	[ "$(busctl --address="$bus" call org.freedesktop.DBus /org/freedesktop/DBus \
		org.freedesktop.DBus NameHasOwner s org.freedesktop.Notifications)" = "b true" ]
}
gone() { ! running; }
# Whether the screen shows the latest state: the last thing tw-notify said of
# its notifications is that it drew them.
settled() { grep '^show \|^draw \|^close ' "$run_dir/bus.log" | tail -n 1 | grep -q '^draw '; }

start_tw -d
start_bus wayland-0

# What reaches the programs: every message on the bus, as dbus-monitor sees
# them. Left out where it is not installed.
if command -v dbus-monitor >/dev/null 2>&1; then
	dbus-monitor --address "$bus" "type='signal',interface='org.freedesktop.Notifications'" \
		>"$run_dir/signals.log" 2>&1 &
	client_pids="$client_pids $!"
	wait_until "the monitor" grep -q . "$run_dir/signals.log"
fi
# signalled <member> <id> <second argument as dbus-monitor prints it>
signalled() {
	[ ! -e "$run_dir/signals.log" ] ||
		grep -A2 "member=$1\$" "$run_dir/signals.log" | tr -s ' \n' ' ' | grep -q "uint32 $2 $3"
}

# Nothing runs until a program has something to say.
running && fail "tw-notify runs before any notification"

# The first notification starts it. It is shown, closes when its time is up,
# its program is told, and tw-notify leaves.
first=$(note 0 'First' 'Some text.' "$plain" -1)
wait_until "the first notification" logged "^show $first 'First' for 300 ms\$"
wait_until "a picture of it" logged '^draw 380x'
running || fail "tw-notify does not own its name while it shows a notification"
wait_until "it to expire" logged "^close $first reason 1\$"
wait_until "its program to be told" signalled NotificationClosed "$first" "uint32 1"
wait_until "tw-notify to leave" gone
logged '^leave$' || fail "tw-notify went without giving its name up first"

# The next one starts it again, with an id no earlier one had.
second=$(note 0 'Second' '' "$plain" 200)
[ "$second" != "$first" ] || fail "an id was used twice"
# libnotify keeps an id in a signed int; `notify-send -r` aborts on one
# above 2^31 - 1.
[ "$first" -gt 0 ] && [ "$first" -le 2147483647 ] && [ "$second" -le 2147483647 ] ||
	fail "the ids $first and $second do not fit a signed 32-bit int"
wait_until "the second notification" logged "^show $second 'Second' for 200 ms\$"
wait_until "it to expire" logged "^close $second reason 1\$"

# What it says it can do, to the programs that ask before they send. The
# list has to hold "actions", or Chromium does not use the service at all.
[ "$(call GetCapabilities)" = 'as 2 "body" "actions"' ] ||
	fail "capabilities: $(call GetCapabilities)"
[ "$(call GetServerInformation)" = 'ssss "tw-notify" "tw" "1" "1.2"' ] ||
	fail "server information: $(call GetServerInformation)"
busctl --address="$bus" introspect org.freedesktop.Notifications \
	/org/freedesktop/Notifications | grep -q '^\.Notify .* susssasa{sv}i  *u ' ||
	fail "tw-notify does not describe itself"
call Nonsense 2>&1 | grep -q 'no such method' || fail "an unknown method got no error"

# An urgent one stays until it is closed.
urgent_id=$(note 0 'Urgent' 'Stays.' "$urgent" -1)
wait_until "the urgent notification" logged "^show $urgent_id 'Urgent' for 0 ms\$"
sleep 0.6
logged "^close $urgent_id " && fail "an urgent notification expired"
# So does one whose program asks for that.
stays=$(note 0 'Stays' '' "$plain" 0)
wait_until "the notification that stays" logged "^show $stays 'Stays' for 0 ms\$"

# Any program may replace a notification by its id, as the specification
# allows: each busctl here, like each `notify-send -r`, is a program of its
# own. It keeps its id and its place. Replacing one that is not there makes
# a new one.
same=$(note "$urgent_id" 'Urgent, changed' '' "$urgent" -1)
[ "$same" = "$urgent_id" ] || fail "replacing gave the id $same, not $urgent_id"
wait_until "the replaced text" logged "^show $urgent_id 'Urgent, changed'"
new=$(note 7 'New' '' "$plain" 0)
[ "$new" != 7 ] || fail "replacing a notification that does not exist reused its id"

# A level, as each press of a volume key sends, replaces the last one with
# its tag (the hint most servers take for that); one with another tag stands
# beside it.
tagged() { echo "0 1 x-canonical-private-synchronous s $1"; }
level=$(note 0 'Volume 40%' '' "$(tagged volume)" 0)
again=$(note 0 'Volume 45%' '' "$(tagged volume)" 0)
[ "$again" = "$level" ] || fail "a level with the same tag got the id $again, not $level"
wait_until "the level replaced" logged "^show $level 'Volume 45%'"
other=$(note 0 'Brightness 50%' '' "$(tagged brightness)" 0)
[ "$other" != "$level" ] || fail "a level with another tag replaced the first"
call CloseNotification u "$level" >/dev/null
call CloseNotification u "$other" >/dev/null
wait_until "the two levels to close" logged "^close $other reason 3\$"

# Any program may close one too, and the program that replaced it last is
# told.
call CloseNotification u "$urgent_id" >/dev/null
wait_until "the urgent one to close" logged "^close $urgent_id reason 3\$"
wait_until "its program to be told" signalled NotificationClosed "$urgent_id" "uint32 3"
call CloseNotification u "$new" >/dev/null
call CloseNotification u 12345 >/dev/null || fail "closing an unknown notification failed"
wait_until "the new one to close" logged "^close $new reason 3\$"

# A tag too long to keep whole is no tag: two that differ only past the
# length kept stay apart.
long=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
long1=$(note 0 'Long 1' '' "$(tagged "${long}1")" 0)
long2=$(note 0 'Long 2' '' "$(tagged "${long}2")" 0)
[ "$long1" != "$long2" ] || fail "two long tags that differ at their end replaced each other"
call CloseNotification u "$long1" >/dev/null
call CloseNotification u "$long2" >/dev/null
wait_until "the two long-tagged ones to close" logged "^close $long2 reason 3\$"

# Clicks. One notification is left, 41 pixels high with only a title, in the
# top right corner 12 pixels from both edges; a second goes below it. A
# click on the lower one closes that one and leaves the other.
lower=$(note 0 'Lower' '' "$plain" 0)
wait_until "the lower one" logged "^show $lower 'Lower'"
wait_until "two notifications on screen" settled
logged '^draw 380x81 ' || fail "two notifications with a title each are not 81 pixels high"
pointer_to 1000 70
click
wait_until "the lower one to close" logged "^close $lower reason 2\$"
logged "^close $stays " && fail "a click closed the wrong notification"
wait_until "its program to be told" signalled NotificationClosed "$lower" "uint32 2"
logged "^action $lower " && fail "a program that asked for no click was told of one"

# A left click on one whose program wants to know: it is told, with a token
# from the compositor that lets it bring its window forward.
call CloseNotification u "$stays" >/dev/null
asks=$(note 0 'Asks' 'Click me.' "$clickable" 0)
wait_until "the one with an action" logged "^show $asks 'Asks'"
wait_until "it on screen" settled
pointer_to 1000 30
click
wait_until "the program to be told of the click" logged "^action $asks default with a token\$"
wait_until "the token to be sent" signalled ActivationToken "$asks" 'string "[0-9a-f]'
wait_until "the action to be sent" signalled ActionInvoked "$asks" 'string "default"'
wait_until "it to close" logged "^close $asks reason 2\$"

# More than fit: the oldest go, but not an urgent one while another can.
keep=$(note 0 'Keep' '' "$urgent" -1)
ids=
for i in 1 2 3 4 5 6; do
	ids="$ids $(note 0 "Number $i" '' "$plain" 0)"
done
set -- $ids
wait_until "the oldest to make room" logged "^close $1 reason 1\$"
wait_until "the next to make room" logged "^close $2 reason 1\$"
logged "^close $3 " && fail "a notification went although there was room"
logged "^close $keep " && fail "an urgent notification was pushed out"
held="$keep $6"

# Only one tw-notify at a time: a second, started by hand, gives up.
if DBUS_SESSION_BUS_ADDRESS=$bus "$notify_bin" 2>"$run_dir/second.log"; then
	fail "a second tw-notify stayed"
fi
grep -q 'another notification service is running' "$run_dir/second.log" ||
	fail "a second tw-notify did not say why it left"

# Text that does not fit: longer than is kept, cut between characters and
# not inside one; line breaks and tabs; a title with a line break. And a
# picture of 70 kB in the hints, which is read past. Each of the three
# pushes out one of the numbers above.
long=$(printf 'é%.0s' $(seq 1500))
big=$(note 0 "$(printf 'Two\nlines\tand a tab')" "$long $(printf 'a\n\n\nb\r\n')" "$plain" 0)
wait_until "the long one" logged "^show $big "
picture=$(note 0 'Picture' '' "0 1 image-data (iiibiiay) 150 150 450 false 8 3 70000 $(printf '7 %.0s' $(seq 70000))" 0)
wait_until "the one with a picture" logged "^show $picture 'Picture'"
# A title longer than the 159 bytes that are kept: 79 of these two-byte
# letters stay, and not 79 and a half.
wide=$(note 0 "$(printf 'é%.0s' $(seq 100))" '' "$plain" 0)
wait_until "the wide title, cut between letters" \
	logged "^show $wide '$(printf 'é%.0s' $(seq 79))' for 0 ms\$"
[ "$(call GetServerInformation)" = 'ssss "tw-notify" "tw" "1" "1.2"' ] ||
	fail "tw-notify does not answer after the long notifications"

# tw goes while notifications are showing: tw-notify goes too, and tells
# their programs they are gone. (stop_tw would stop the bus first, as one of
# tw's clients.)
held="$held $big $picture $wide"
others=$client_pids
client_pids=
stop_tw
client_pids=$others
wait_until "tw-notify to leave with the compositor" gone
for id in $held; do
	logged "^close $id reason 1\$" || fail "notification $id was not closed as tw went"
done
wait_until "the urgent one's program to be told" signalled NotificationClosed "$keep" "uint32 1"
stop_bus

# No compositor to show them on: the program gets an id, is told at once
# that the notification is gone, and tw-notify leaves.
start_bus wayland-nowhere
lost=$(note 0 'Lost' '' "$plain" 0)
wait_until "the lost one to be closed" logged "^close $lost reason 1\$"
logged 'no Wayland compositor' || fail "tw-notify did not say there is no compositor"
wait_until "tw-notify to leave" gone
stop_bus

if grep -q 'Sanitizer\|runtime error' "$run_dir/bus.log"; then
	fail "sanitizer report from tw-notify"
fi

echo "ok: on demand, expiry, leaving, capabilities, urgent, replace, tags, close, clicks, token, overflow, single instance, long text, closed with the compositor, no compositor"
