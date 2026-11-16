# How tw works

For whoever changes tw. What each part does, and the reasons that are not
obvious from its code. The code's own comments have the detail; the rules a
change has to keep are in [CLAUDE.md](../CLAUDE.md).

- [The strip](#the-strip): workspaces, columns, outputs, scrolling, focus
- [Screen off and lock](#screen-off-and-lock)
- [X11 programs](#x11-programs)
- [The clipboard](#the-clipboard)
- [On battery](#on-battery)
- [Notifications: tw-notify](#notifications-tw-notify)
- [The session script](#the-session-script)
- [The NVIDIA GPU: tw-gpu](#the-nvidia-gpu-tw-gpu)
- [Bluetooth: tw-bluetooth](#bluetooth-tw-bluetooth)
- [Power profiles: tw-power](#power-profiles-tw-power)
- [Thunderbolt](#thunderbolt)

What tw's C code does is in `src/`. `tw-notify` is a second program in
`notify/` that shares no code with it. Everything else is shell scripts and
unit files in `data/`, installed with tw but not part of the compositor:
they would work under another one.

## The strip

**Workspaces and columns.** There are `TW_WORKSPACES` (9) workspaces, each a
strip: a list of columns, left to right. A column (`struct tw_column`) is a
stack of windows, top to bottom, sharing the column's width and splitting
its height. It is never empty and is freed when its last window leaves
(`column_take()`). A fullscreen window is always alone in its column.
Dialogs (windows with a parent or a fixed size) are not in the strip: they
sit in the workspace's `floating` list, centred, at the size their client
chose. A window of a fixed size without a parent is often a game rather than
a dialog, and a game goes fullscreen: it then gets a column of its own like
any fullscreen window, and floats again when it leaves fullscreen
(`float_after_fullscreen`). A real dialog asking for fullscreen stays put.

tw has no maximized or minimized state and no window menu, and tells each
window so as it appears (`wm_capabilities`: fullscreen only). Left to
wlroots' default, a program that draws its own title bar (GTK 4) shows
buttons for them that do nothing.

**Outputs.** Each output shows one workspace (`output->workspace`), and no
two outputs show the same one; the rest are hidden. `server->active` is the
workspace with the focus, and the output showing it is the focused output:
new windows open there. Going to a workspace that another output shows moves
the focus to that output; the workspace stays where it is. Unplugging an
output hides its workspace, unless it was the one in use: the oldest
remaining output then shows it instead (`handle_destroy()` in `output.c`).
No window is ever moved because a monitor went away. With more than one
output, a window sticking out of its output is trimmed at the edge
(`window_trim()`), or it would show up on the neighbour. A monitor plugged
in while the screens are off stays off with them. An output that is not for
a desktop (a VR headset, `non_desktop`) is left alone.

**A GPU reset** (a driver hang, a bad wake-up) takes the renderer and all
it made with it. `handle_renderer_lost()` in `output.c` makes a new
renderer and allocator and hands them to the compositor and every output;
without that the screens would stay black until tw was started again. If
an output cannot take the new ones, the old ones are not freed: drawing
with them fails, but freed they would crash tw.

**Arranging.** `strip_arrange()` decides where the view of each workspace on
screen should be (`view_x`) and places everything. Call it after anything
that changes a strip, the focus, a column width, which workspace an output
shows, or an output. Nothing else positions windows. A hidden workspace is
not laid out at all until it is shown again, because sizes depend on the
output. The maths (column widths, stacking, how far to scroll) is in
`layout.c`: pure functions, unit tested.

**Scrolling.** The view glides to `view_x` on a critically damped spring.
`view_pos` is where it is drawn now; `strip_animate()` advances it once per
frame of the output showing it and stops asking for frames when it arrives.
A three-finger swipe moves `view_pos` directly and `strip_swipe_end()` picks
the column edge to settle on. A window is drawn where `view_pos` puts it,
but told to draw (not `suspended`) as soon as it is visible at either
`view_pos` or `view_x`, so the windows the view is heading for are awake
when it gets there.

**What is on screen.** A window is on screen only if its column is in view,
an output shows its workspace, the outputs are on (`!server->blanked`) and
the session is unlocked. In every other case `strip_arrange()` disables its
scene node (no frame callbacks) and sends it the xdg `suspended` state.
That is the battery win of a scrolling layout: most windows are not being
drawn.

**Focus.** Each workspace has `focused` (tiled or floating; gets the
keyboard) and `tiled` (the strip's focus, which the view scrolls to). They
differ only while a dialog is focused. Focus has two parts, both derived by
`focus_update()`:

- the active window: the focused window of the active workspace. Its client
  is told it is activated, and tw draws nothing to mark it;
- the keyboard, which goes to the lock screen if locked, else to a layer
  surface that asked for exclusive focus (a launcher), else to the active
  window.

Change `workspace->focused`, `server->active`, `server->keyboard_layer` or
the lock state, then call `focus_update()`. Nothing else sends focus events.
When the keyboard moves it ends any grab first: an open menu holds the
keyboard and the pointer, and wlroots would otherwise keep them on the menu
through the change, a lock included. A drag is the exception: it is ended
only by a lock, so that a window opening during one does not make the user
drop what they carry. While a drag lasts no surface has the keyboard and
wlroots ignores a change, so when it ends (`handle_drag_destroy()`) the
keyboard goes to whatever has the focus by then, and the pointer to what is
under it. The surface that gets the keyboard is told no key is held, since
the key that changed the focus belonged to a binding and its release goes
to nobody.

The pointer's focus follows the windows, not only the mouse:
`strip_arrange()` ends with `pointer_refocus()`, which gives the pointer to
whatever is under it now (unless a button is held, but even then when the
session locks), and so does each step of a glide or a swipe. Without that, a click or a scroll after a lock, a
workspace change, a window closing or the strip gliding went to the window
that had been under the pointer before.

A window can ask for the focus (xdg-activation). It gets it, and its
workspace is shown, only if its token was made from an input event and
names the surface that event went to; wlroots checks that the surface had
the keyboard or the pointer. A token without an event can be made by any
client at any time, and one without a surface takes any serial the client
was ever sent, the pointer merely passing over it.

**The pointer.** A client may lock the pointer in place and read raw mouse
movement instead (games, 3D editors, and X11 programs through Xwayland), or
confine it. A constraint is in force only while its window has both the
keyboard and the pointer; `pointer_constraint_update()` re-derives that
after either focus changes, so every focus binding releases a lock.

**Drag and drop.** A client starts a drag from a button press; tw lets it
if the press was real (its serial) and the session is not locked, and
wlroots' drag grab then tells each window the pointer passes over. The
picture a client drags, if any, is in `drag_icon_tree`, the top of the
scene, moved with the cursor. wlroots takes the picture's input region
away, so the hit test sees through it. Unlike a click, a drag follows the
windows as the pointer focus does (`pointer_refocus()` runs for it with
the button held): carried to another workspace, it is over what is there,
and let go over nothing it lands nowhere.

**Menus** (xdg popups) are kept on the output: tw hands wlroots the
output's box when a menu first appears and again whenever its client moves
it (`reposition`, which GTK 4 and Firefox use for menus that follow what
they belong to). A layer surface's menu is kept on its layer surface's
output. A window's menus are drawn in `popup_tree`, above every window,
dialog and top-layer surface: under its window's own tree, the menu of a
tiled window would open behind an open dialog, unseen but holding the
keyboard and the pointer. Each window's `popup_tree` follows its window's
position and visibility (`window_move()`, `window_draw()`).

**Keys.** Direction keys follow one scheme: I, J, K, L are up, left, down,
right, and the arrow keys do the same. Alone they move the focus; with Shift
they go to the monitor on that side or, with none there, to the previous or
next workspace; with Ctrl the focused window or column is taken along. The
`DIRECTION` macro in `config.h` binds all four meanings of a key at once.
The other bindings follow niri's defaults. Bindings match on the unshifted
keysym, so Shift combinations do not depend on the layout.

Mod is Super, or Alt when tw runs in a window inside another session,
which owns Super; that is told from the backend, not from the environment.
A binding of Mod with Alt (Mod+Alt+L, lock) does not exist there: it would
be Alt+L, which is Mod+L's.

Every keyboard but a virtual one gets the same keymap, compiled once from
the `XKB_DEFAULT_*` variables. The seat sends the keymap to every client
whenever the keyboard in use has a different one, and a laptop has several
keyboards: the hotkeys and the power button are keyboards too.

**Layer surfaces.** Launchers, bars and notifications are layer-shell
clients (`layer.c`). A new layer surface is told its screen's scale as it is
created: wlroots' scene tells a surface its scale only once it is on screen,
by which time a client has drawn it for scale 1 and has to draw again.
Windows still learn their scale the late way.

wlroots sends a layer surface a configure whenever asked, changed or not,
and every layer surface is arranged whenever one of them changes. So each
remembers what it was last arranged for (the output's box and the room
left by the surfaces before it) and is not configured again until that or
its own state changes: a notification coming and going does not wake the
wallpaper or a bar.

## Screen off and lock

**Idle** (`idle.c`). One timer, not re-armed per input event: input records
a timestamp, and when the timer fires it does what is due (blank the
outputs, suspend the machine) and sleeps until the next thing is. A visible
surface with an idle inhibitor holds both off; one on a hidden workspace,
or a layer surface behind the lock curtain or on a screen that is off, does
not. While the outputs are off every window is suspended.

Nothing tells a compositor that the machine was suspended and has woken, so
`idle.c` keeps a timer that never expires and that the kernel cancels when
the wall clock jumps: free until it happens, and it is what turns the screen
back on after a wake-up. The clock also jumps when the time service sets
it, so a jump counts as a wake-up only if the time spent asleep
(`asleep_ms()`, the difference of CLOCK_BOOTTIME and CLOCK_MONOTONIC) grew.

**Lock** (`lock.c`). tw draws no lock screen. A lock client (swaylock) asks
for the lock through ext-session-lock, or tw takes it by itself with
`lock_now()`: on Mod+Alt+L, when the lid closes, when the screen blanks from
inactivity, and before a suspend. `lock_now()` sets `server->locked` at once,
which hides every window behind an opaque curtain, and then starts the
locker, which takes the lock over. So `locked` with `server->lock == NULL`
is a normal state: the locker has not arrived yet, or it died. A key press
in that state starts the locker again, at most once a second.

The lock client is told `locked` only once the curtain is on every screen,
as ext-session-lock requires: each output remembers its `commit_seq` as the
curtain comes down, and its `present` event for a later frame marks it shown
(`lock_confirm()`). An output that is off counts as shown. `swaylock -f` in
a sleep hook waits for `locked`, so the machine never wakes to the last
unlocked picture. Locking on lid close does not wait for that: logind
suspends when it suspends, and tw holds no sleep delay of its own.

What a lock guarantees (no bindings but VT switching, input only to lock
surfaces, locked until a new client unlocks) is a rule in
[CLAUDE.md](../CLAUDE.md#rules).

## X11 programs

X11 applications are not tw's business beyond `x11.c`. It creates the X11
display sockets, sets `DISPLAY` for the programs tw starts, and on the first
connection hands the sockets to xwayland-satellite, whose windows then
arrive as ordinary xdg-shell windows. While the helper runs tw does not
watch the sockets; when it exits (a pidfd tells tw: one wakeup, no SIGCHLD
handler) tw watches them again. With no X11 program running the cost is two
listening sockets.

**Stopping it.** The helper and Xwayland are stopped again when no X11
program is connected, because many programs only ask X11 a question as they
start and would leave 25 MB of X server behind for the session. tw cannot
ask the server who is connected, and neither Xwayland's own `-terminate`
nor anything else can be passed through the helper, so
`clients_connected()` reads it off the kernel: it lists the sockets the
helper and its child have open (`/proc/<pid>/fd`) and looks each one up in
`/proc/net/unix`.

- A connection that came in through the display's sockets bears their name:
  a client.
- One that is not in that list at all came from another network namespace
  (a sandbox): counted as a client too.
- The rest are the helper's own.
- If anything cannot be read, the helper stays.

There is no event for an X11 client leaving, and looking now and then would
be polling. So tw looks at two moments only, `x11_idle_seconds` after each:
the helper's start, and any window closing (`x11_window_closed()`). A client
that leaves with no window closing is noticed at the next window that does.
A client queued behind a helper tw stopped is served by the next one, not
turned away like one queued behind a helper that died.

The helper is looked up on `PATH` before each start, not once: an update
may have moved it. It must be installed when tw starts, though, or tw opens
no X11 sockets at all.

The helper has no chance to tidy up when stopped and leaves its own socket
and lock file (`xwls-1`) in the runtime directory; the next helper takes the
same name, so they do not pile up.

## The clipboard

On Wayland a copy stays inside its program, which hands it over when
somebody pastes, so it is gone when the program exits. Keeping it is the one
job tw took in from a helper (`clipboard.c`): a helper for it could never be
started on demand, because it has to have read a copy before its program
exits, so it ran all session.

**Reading.** When the seat's selection changes (`set_selection`) tw asks the
new source for each format it offers, one after the other, each through a
pipe of its own, and keeps the bytes. The program stays the clipboard's
owner for as long as it lives. When the source goes away tw offers what it
read as a data source of its own (`handle_takeover()`; from an idle
callback, so that every listener has first heard that the clipboard is
empty), until the next copy replaces it. tw knows its own source by its
`impl` and does not read that one. A program that asks for an empty
clipboard is obeyed: `handle_request_set_selection()` calls
`clipboard_cleared()` first, which is how a copy taken back on purpose is
told from one whose program exited.

A copy offering `x-kde-passwordManagerHint` (KeePassXC and other password
managers mark secrets with it) is not read at all: its program empties the
clipboard after a while, and a copy kept by tw would outlive that if the
program exited first.

**What keeps it small.**

- Formats with the same bytes are kept once (a text comes under five names).
- One copy may hold `clipboard_keep_bytes` (64 MB) at most, the format being
  read included; a larger one is left with its program, so no program can
  make tw grow without limit.
- Formats are read one at a time, so the most held in passing is one format
  more than what is kept.
- Nothing blocks: tw's end of every pipe is non-blocking, when it reads and
  when it serves a paste from a copy it keeps, and a paste larger than the
  reader's pipe goes on as the reader takes it (`tw_paste`).
- No timer and no polling: with a copy held and nothing happening tw does
  not wake.

What that comes to, measured: 28 kB for a line of text, and 20.4 MB for a
text of 20 MB offered in five formats (`wl-clip-persist` held 98 MB for
it), back down after the next small copy.

Writing to a reader that has left would kill tw, so tw ignores SIGPIPE, and
`spawn()` and the start of the X11 helper set it back for the programs tw
starts.

**A program that dies while being read.** A pipe ends the same way when its
program has written everything and when the program died over it. The
kernel closes a dying program's files in the order of their numbers, and in
which order tw then hears of the pipe and of the connection is not certain.
When the pipe comes first, tw can have taken a format that was cut short as
read, and asked for the next one, before it learns that the program is
gone. When the connection comes first, the format on its way is dropped,
whole or not. What tells the two apart is that a dead program
never reacts to that next request. So when a source goes away while it is
being read, the newest format read counts only if something came of the one
after it (`answered`), the format on its way is dropped, and a format that
ended with no bytes is never kept.

Known, and left: the last format a program offers has none after it and is
taken as it is, so a program that dies while writing its last format in
more than one piece (over 4 kB) leaves that one cut short on the clipboard.

## On battery

`power.c` does three things, all off one event source. What that costs on a
machine with a battery: 20 kB, and a socket that wakes tw each time a
battery or charger announces itself, which they do with nothing changed
(every 35 to 110 s on the G533ZW), to read a handful of small files. These
are the one wakeup an idle tw has, and they come from the kernel, not from
tw. On a machine without a battery, nothing.

**Knowing.** `server->on_battery` follows the kernel's own rule
(`power_supply_is_system_supplied()`): the machine has a battery, and no
supply that is not a battery is online. Supplies of scope `Device` belong to
a mouse or a phone and are skipped. Changes arrive through a libudev monitor
on the `power_supply` subsystem (wlroots links libudev anyway). tw does not
read what an announcement says, beyond its scope: one from a `Device` (a
wireless mouse reporting its own battery) is dropped unread. For any other
it reads `/sys/class/power_supply` again and acts only if the answer
changed, since most announcements change nothing.

A machine without a battery opens no monitor and runs no command. Neither
does a tw that is not what runs the machine, one inside another session or
on the headless backend (`server->session` is NULL): it would set the power
profile a second time, next to the session that does. The tests switch it
on all the same by pointing `TW_SYS` at a made-up machine; tw then does not
listen to the kernel, whose announcements are about the real machine (a
test that heard the real battery once warned of the made-up one).

**Slower outputs.** On battery every output runs in the slowest mode of its
preferred size that is not below `battery_refresh_hz` (60), because a
program that animates draws as often as the output refreshes.
`output_mode()` is the one place that says which mode an output should be
in; a new output, an output coming back on and a change of power supply all
ask it. wlroots refuses a mode for an output that is off, so
`outputs_update_mode()` skips those and `output_set_power()` sets the mode
as it turns one on, and turns it on in its old mode if the new one is
refused: a dark screen is the one failure this must not have.

**The power profile.** `battery_command` runs when the cable is pulled,
`mains_command` when it is back, and one of the two at startup. They are
`tw-power saver` and `tw-power balanced`; see
[Power profiles](#power-profiles-tw-power).

**A low battery.** There is no bar, so nothing shows the charge. With each
announcement on battery `handle_change()` reads it (the fullest battery's
`capacity`) and asks `tw_battery_look()` what is due:
`battery_warn_command` once at `battery_warn_percent`, and a suspend
(`suspend_now()`: lock, then `suspend_command`) when the charge has fallen
to `battery_suspend_percent` and again for each percent below that, so that
a machine woken and left unplugged goes back to sleep before it runs out.

A reading is `fresh` when somebody has just arrived (the cable was pulled,
tw started, the machine woke up), and then it only sets the mark the charge
has to fall below: nobody is put to sleep the moment they open the lid.
Waking is told from the clocks, not from an event that could come after the
battery's: CLOCK_BOOTTIME runs during sleep and CLOCK_MONOTONIC does not, so
their difference grows only then (`asleep_ms()`). tw does not look at the
charge as it starts, because a notification sent before `tw-session` has
told D-Bus where the session is would start the notification service
without a display; the battery's next announcement comes soon enough.

## Notifications: tw-notify

`tw-notify` shows notifications. It is a program of its own in `notify/`
that shares no code with tw and talks to it as any layer-shell client
would. It is kept small by running only while there is something to show
and by speaking D-Bus itself.

**It runs only while there is something to show.** Its D-Bus service file
makes the session bus start it when a program sends a notification or asks
for its capabilities, and it exits `linger_ms` (one second) after the last
notification has closed or the last question was answered. Leaving must not
lose a notification that is on its way, so it first gives up its name on
the bus (`ReleaseName`) and exits when the answer to that arrives: whatever
was sent before the bus took the name away has been delivered by then, and
whatever is sent after starts a new `tw-notify`. One that arrived in
between is shown, and the name asked for again; if a new `tw-notify` has
taken the name by then, both show a list until the old one's notifications
have closed. That is left as it is: it needs a notification to arrive in
the few milliseconds of the hand-over.

**Any program may replace or close a notification by its id**, as the
specification allows: `notify-send -r <id>` and scripts that close one with
`gdbus` are new programs each time. The program that last sent or replaced
a notification is the one told of its click and its closing. Ids go on from
the time of day in milliseconds, so a new `tw-notify` does not hand out an
id an earlier one gave, and a program holding an old id does not reach
somebody else's notification by accident.

**It speaks D-Bus by itself.** The messages are read and written by
`bus.c`, with no D-Bus library: the login line, `Hello`,
`RequestName`, the four methods of `org.freedesktop.Notifications`, `Ping`
(with or without its interface named), an empty `GetAll`, `Introspect`, and
the signals. Everything read off the socket is checked against the bytes
that are there, and a `Notify` body must be exactly what its signature
says, to the byte; `bus.c` reads it (`bus_notify_read()`), so the unit test
reads it with the code that runs. `tests/bus_test.c` is mostly about
messages that are cut short or damaged. A message that cannot be read is
passed over: a call among them gets `InvalidArgs` back if its header got as
far as its sender, and a big-endian one, which is not read, is passed over
by its length. Only a message whose length cannot be told (another version
of the protocol) ends `tw-notify`, which closes its notifications first.
Signals go to the program that sent the notification and to nobody else.

**Text.** The font is `font` in `notify/config.h`, found through
fontconfig and drawn with FreeType, so any character some installed font
has is shown: fontconfig's fallback list is walked for each character a
font lacks. Colour (emoji) and bitmap fonts are passed over, since their
pictures come in fixed sizes. Both libraries are opened with the first
picture, not at startup, so a program that only asks for the capabilities
costs no font. With fonts loaded `tw-notify` holds about 3 MB of its own,
most of it fontconfig's list of fonts; it is gone a second after the last
notification. There is no bold, no markup and no right-to-left text.

**The window.** The list is one layer surface on the overlay layer, top
right, drawn into a shared-memory picture that `tw-notify` unmaps as soon as
the compositor has it. On a screen with a fractional scale the picture is in
the screen's own pixels (fractional-scale and viewporter), its size rounded
up so that it is a whole number of logical pixels. The overlay layer is
above fullscreen windows, so a game does not hide the battery warning, and
below the lock screen, so a locked session shows none. When its monitor is
unplugged the compositor closes the surface, and the list opens again at
once on a monitor that is left; with none left the compositor closes it
before its first configure, and it comes back as soon as a monitor is
plugged in. So a warning that does not expire is not left out of sight.
When the compositor itself goes, `tw-notify` tells the programs their
notifications are gone (reason 1, expired) and exits.

At most `max_shown` notifications are listed. The oldest goes to make room
for a new one, but not a critical one while another could go: the battery
warning is sent once and does not expire.

A click closes the notification under it. A click while the list is
changing (a new picture not yet on screen) is let go, since it would be
matched against the new layout while the old one is shown. A left click on
one whose program
listed the action `default` first gets an activation token from the
compositor for that click and sends it on (`ActivationToken`, then
`ActionInvoked`), which is what lets the program bring its window forward
under tw's rule that only a token made from input may take the focus.
`GetCapabilities` says `actions` for that reason and because Chromium falls
back to drawing its own notifications when a service does not list it.

The service file is installed under `/usr/local/share`, which D-Bus reads
before `/usr/share` with the first to claim a name winning, so tw's entry
beats another notification service's without that one being removed.

## The session script

`data/tw-session` starts tw as a login session, from a login screen's entry
or typed on a text console. It publishes the session to the user's D-Bus
and systemd, so it is never run inside another desktop to try it out;
`TW_SESSION_PRINT=1 sh data/tw-session` prints what it would do instead.

What it does besides starting tw:

- **Picks the GPUs**; see [the NVIDIA GPU](#the-nvidia-gpu-tw-gpu).
- **Points programs at the keyring.** It sets `GNOME_DESKTOP_SESSION_ID`,
  without which Chromium and Electron programs use no keyring at all (their
  `safeStorage` reports `basic_text`) and cannot keep a login. The keyring
  itself is not tw's business: the user's systemd starts it and its PAM
  module unlocks it at login.
- **Keeps the accessibility bus off** unless `TW_A11Y=1` is set. It exports
  `NO_AT_BRIDGE` for GTK 3 and `GTK_A11Y` for GTK 4. The programs tw starts
  inherit that, but the one that started the bus at every login is the
  file-dialog portal, a GTK 3 program that systemd starts, and such
  programs only see what `dbus-update-activation-environment` hands on. So
  the names to hand on are kept in one list (`service_env`), which the
  startup command passes along with where the session is.

  Telling the toolkits is not enough for Chromium and Electron programs,
  which ask the session bus for the accessibility bus's address as they
  start; the session bus then starts the bus's launcher, which stays. So
  `tw-a11y.conf` adds `ConditionEnvironment=!NO_AT_BRIDGE` to the bus's
  user unit: it starts only where systemd has not been handed that
  variable, and the caller is told at once that there is no such service.
  With `TW_A11Y=1`, `tw-session` also takes the variable back from systemd,
  which remembers what an earlier session told it for as long as the user
  is logged in anywhere.
- **Says what to do when tw cannot start** because a library it was built
  against is gone after a system update (the loader's exit status, 127).
- **Runs `~/.config/tw/autostart`**, if there is one, once tw is up.

The desktop portals are not the script's: `tw-portals.conf`, installed for
xdg-desktop-portal, has screen capture and screenshots served by the wlr
backend and everything else (file dialogs) by the GTK one.

`data/post-install.sh` is what `meson install` runs last: it has systemd,
polkit, udev and the installing user's systemd read the new files, and does
nothing for a staged install.

## The NVIDIA GPU: tw-gpu

On a laptop that also has an Intel (or AMD) GPU the NVIDIA GPU is off until
something needs it, and none of this is in tw's C code.

**Off** means its kernel driver is unloaded and the GPU is off the PCI bus
(`remove` on the device; the root helper leaves the port's path in
`/run/tw-gpu-off`, and scans that port to bring the GPU back, removing the
note only once the GPU is back: without it the machine would pass for one
with no NVIDIA GPU). Unloading
alone is not enough: NVIDIA's libraries load the driver through their setuid
`nvidia-modprobe` as soon as a program asks about the GPU, which `btop` and
`fastfetch` do, and modprobe's configuration cannot refuse it (kmod ignores
`install` commands for a module with soft dependencies, which `nvidia`
has). With no NVIDIA GPU on the bus the driver itself refuses to load.

**`tw-session`** does three things for this. A GPU, for it, is any display
controller on the PCI bus (class `0x03`): VGA, but also 3D (`0x0302`),
which is what the NVIDIA GPU of many laptops calls itself and which has no
`boot_vga` file. The first GPU is the one the firmware showed the boot
screen on (`boot_vga`); a machine whose integrated GPU is not VGA-class has
none, and then it is the one with the built-in panel connected (eDP, LVDS,
DSI), or else the first that is not NVIDIA's. It names the GPUs tw may use in
`WLR_DRM_DEVICES` whenever the machine has a second GPU, be it loaded,
without its driver, or switched off, because wlroots opens a GPU that
appears later only when that variable is unset; so the GPU can come and go
under a running tw. It keeps programs on the first GPU
(`__EGL_VENDOR_LIBRARY_FILENAMES`, and `VK_LOADER_DRIVERS_DISABLE='*nvidia*'`,
which goes by the driver manifest's name and so survives a Mesa update
during the session), so that none of them grabs the NVIDIA GPU while it
is on for another. It hands the same two on to D-Bus and systemd for the
programs they start (the portals), and takes them back from systemd on a
login that does not set them. And once tw is up it runs
`tw-gpu login`, unless a monitor is on the NVIDIA GPU.

**`tw-gpu run <program>`** switches the GPU on, starts the program with
NVIDIA's offload variables, and switches it off afterwards. The launcher
starts everything through `tw-gpu launch`, which does the same for desktop
entries with `PrefersNonDefaultGPU=true` or KDE's
`X-KDE-RunOnDiscreteGpu=true`, and otherwise just runs the program. It
reads the entry the launcher names in `DESKTOP_ENTRY_PATH` (fuzzel sets
it), and without that looks the desktop ID up as the specification says,
each dash standing for a dash or a directory (`kde-foo.desktop` may be
`kde/foo.desktop`).

Who still needs the GPU is tracked with one lock file: every program
started by `run` inherits a shared lock on it, and the switch-off, arranged
before the program starts and in a session of its own, waits for the
exclusive lock. So there is no counter to get out of step.

The root helper leaves a GPU alone that any program has open (it reads
`/proc/*/fd`), and then one `tw-gpu retry` waits for that to end: it looks
at the driver's use counts every five seconds and tries again when they
have changed. That is the only polling in all of this. It runs only while
the GPU is on although nothing asked for it, and gives up after ten minutes
in which nothing changed: something that keeps the GPU open all session (a
monitoring program) would otherwise keep it waking every five seconds. The
next `run` or `off` tries again. It is also what waits for a login screen,
which keeps the GPU open for some seconds after tw has the screen.

NVIDIA's own daemons keep the driver open too, and are not counted as
users: `nvidia-powerd` and `nvidia-persistenced` are stopped before the
driver is unloaded and started again with the GPU, each if it is enabled.
Before the GPU leaves the bus, its other NVIDIA functions (its sound, on
some a USB-C controller) are allowed to sleep: the slot loses power only
when every function on it is asleep.

The helper leaves alone a GPU that another driver than NVIDIA's has
(nouveau), and one whose driver is no longer on the disk: after a kernel
update the running kernel's modules are gone until the next boot, and a
driver unloaded then could not be loaded again. `tw-gpu` then keeps it on
and says to reboot. A machine with no NVIDIA driver installed at all is
told apart by the running kernel's own modules still being there, and is
told so instead. `tw-gpu` also keeps the GPU on whenever tw has it open:
with no GPUs named in `WLR_DRM_DEVICES` (one GPU, or `TW_ALL_GPUS=1`)
wlroots opened them all.

Switching on has a trap: the kernel announces the GPU before a driver can
bind to it, udev loads the driver in answer, that load fails ("probe
routine was not called"), and a load made right then fails with it. The
helper runs `udevadm settle` first and tries the load up to five times.

Switching needs root: `tw-gpu` starts `tw-gpu@on.service` or
`tw-gpu@off.service`, which a polkit rule allows, and those run
`tw-gpu-root`. The two units could run side by side, so the helper takes
a lock (`/run/tw-gpu.lock`) and does one thing at a time. When a unit
fails, `tw-gpu` says which journal to read. `tw-power` and `tw-bluetooth`
are built the same way.

A udev rule (`72-tw-gpu.rules`) lets an NVIDIA GPU whose driver is
unloaded power down. CachyOS's `71-nvidia.rules` forbids that; tw's sorts
after it and wins. `tw-gpu` takes the GPU off the bus anyway, so the rule
matters for a driver unloaded by hand or a GPU that could not be removed.

## Bluetooth: tw-bluetooth

Bluetooth is handled like the NVIDIA GPU: off until asked for, with none of
it in tw's C code. `bluetoothd` otherwise runs all day, because systemd
starts it whenever the kernel has an adapter (udev gives every Bluetooth
device a wish for `bluetooth.target`), whether the radio is blocked or not.
So off is not a stopped service but a driver that is not loaded: with no
adapter nothing starts the service, the stock `bluetooth.service` stays
enabled as shipped, and the Bluetooth modules are out of the kernel too.

**The blacklist is also the list.** `tw-bluetooth.conf`, installed into
`modprobe.d`, blacklists the adapter's driver (`btusb`). A blacklist stops
what udev does at boot, loading by the device's alias, and not a load by
name, so the same file is the list of what `tw-bluetooth on` loads: the
root helper reads the names out of it. An adapter on another bus (UART,
SDIO) has a driver the file does not name and is there from boot; once
`off` has unloaded it, `on` has to know it. So the helper keeps every
module it unloads in `/run/tw-bluetooth-unloaded` and loads those too.

`on`, `off` and the timer's `idle` are three units, which systemd would run
side by side; the helper takes a lock first, so the timer running out
during an `on` waits for it. `tw-power`'s helper does the same.

**On** leaves `/run/tw-bluetooth`, loads the drivers, waits for an adapter,
lifts the radio's block and starts `tw-bluetooth-idle.timer`. It lifts the
block a second after udev has settled, not at once, because systemd puts
the block back to what it was when the adapter was last there
(systemd-rfkill) and would undo it; once it has seen the block lifted, "not
blocked" is what it puts back from then on.

`on` also starts the service itself at the end, since an adapter that was
there already (an earlier `off` that could not unload its driver) brings
no event that would start it.

**Off** stops `bluetooth.service` and unloads every module that holds the
Bluetooth core and then the core, in up to five passes half a second apart:
the sound service keeps a Bluetooth socket open until it has seen the
daemon go.

**The service cannot simply be stopped.** When `bluetoothd` leaves the bus,
WirePlumber calls it to take back what it had registered, and the system
bus starts a service that is called while it is not running (the calls that
take leave have auto-start on, unlike the one that looks for it). Without
more, the service was running again 7 ms after it had stopped, held `bnep`
and the core, and `off` failed; or it was started while the helper was
unloading, and its first socket made the kernel load the core again after
`off` had reported success. So `tw-bluetooth-service.conf` adds
`ConditionPathExists=/run/tw-bluetooth` to `bluetooth.service`: the service
starts only while Bluetooth is switched on. That is why `on` leaves the
note before it loads the driver, and `off` removes it before it stops the
service. A start that the condition refuses is one line in the journal.

**Off by itself.** Bluetooth goes off when nothing has been connected for
ten minutes, with no process waiting for that. The kernel keeps each
connection as a device next to its adapter (`hci0:256`); a udev rule starts
the timer over whenever one is removed, and when the timer runs out the
helper (`idle`) switches off unless a connection exists, or the drivers
are no longer on the disk for the running kernel (after a kernel update,
until the next boot). Both only while
`/run/tw-bluetooth` is there, so a machine that has Bluetooth on from boot
(built with `-Dbluetooth=false`, or before its first reboot) is left alone.
A second udev rule lets a USB adapter sleep (`power/control` to `auto`),
which its driver would do as it loads and nobody does with the driver kept
away. It finds the adapter by its Bluetooth interface, which every adapter
has, and sets the whole device: a chip that is Wi-Fi and Bluetooth in one
often does not call itself a Bluetooth device as a whole.

Two other designs were weighed and dropped. Following the radio's block
(`rfkill unblock` starts the daemon, `block` stops it) needs no root
helper, but leaves the modules loaded and has no clean way to start: a new
adapter is unblocked until systemd blocks it again, so the daemon would
start and stop at every boot. Running WirePlumber's Bluetooth half as a
second instance only while Bluetooth is on would take 2.9 MB off its line
in `btop`, of which 0.28 MB is memory in use, for a whole second
WirePlumber while Bluetooth is on.

## Power profiles: tw-power

A profile is three settings in `/sys` that only root may write, the same
three power-profiles-daemon writes before it stays for the session:

| Profile | Platform profile | Each CPU's `energy_performance_preference` | `energy_perf_bias` |
| --- | --- | --- | --- |
| saver | `low-power`, or `quiet` where a machine has no such | `power` | 15 |
| balanced | `balanced` | `balance_performance` | 6 |
| performance | `performance` | `performance` | 0 |

Under the `performance` governor intel_pstate and amd-pstate refuse every
preference but `performance`, so `saver` and `balanced` move such a CPU to
`powersave` first, as the daemon does. Nothing else in cpufreq or
intel_pstate moves with the daemon's profiles, and ASUS's
`throttle_thermal_policy` follows the platform's profile by itself.

`tw-power` has them written by a root helper through a oneshot unit, the
way `tw-gpu` does its switching, and nothing stays running. A
machine that lacks one of the settings is not given it, and a setting that
already reads as wanted is not written.

## Thunderbolt

Thunderbolt devices are let in by a udev rule (`tw-thunderbolt.rules`, the
one in the kernel's own guide) instead of boltd, which runs from boot only
to approve them. Where the kernel's DMA protection is on
(`iommu_dma_protection` reads 1) boltd approves every device without asking,
and the rule does the same as the device appears. Where it is off the rule
does nothing. [setup.md](setup.md#thunderbolt-a-dock-without-a-daemon) says
how to tell and when to keep boltd.
