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
- [The terminal](#the-terminal)
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

Two files. `strip.c` decides where windows go: the workspaces, their
columns, the focus, the view that scrolls and the key actions. `window.c`
is one window: its client, its nodes in the scene, its menus and title bar.
It carries out what `strip.c` decides (`window_place()`, `window_show()`)
and sends the client only what changed. Each calls the other through
`tw.h`: a window that maps or unmaps goes to `strip_add()` and
`strip_remove()`.

**Workspaces and columns.** There are `TW_WORKSPACES` (9) workspaces, each a
strip: a list of columns, left to right. A column (`struct tw_column`) is a
stack of windows, top to bottom, sharing the column's width and splitting
its height. It is never empty and is freed when its last window leaves
(`column_take()`). A fullscreen window is always alone in its column.
Dialogs (windows with a parent or a fixed size) are not in the strip: they
sit in the workspace's `floating` list, centred, at the size their client
chose. A tiled window is told it is tiled and is clipped to its box; a
dialog is told it is not, and is not clipped, so the shadow its client
draws around it shows. A window of a fixed size without a parent is often a game rather than
a dialog, and a game goes fullscreen: it then gets a column of its own like
any fullscreen window, and floats again when it leaves fullscreen
(`float_after_fullscreen`). A real dialog asking for fullscreen stays put.
A window is sized at its first commit for what it asked to be, so that its
first frame fits; as it maps, `strip_add()` settles its state again from
what it asks for then, since a client can change its mind in between. That
first configure carries the window's whole state (`initial_commit()` in
`window.c`): a window that unmaps and maps again would otherwise be sent
what it was last told, which wlroots keeps, and one hidden at the time
would come back `suspended` and might never draw.

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
remaining output then shows it instead (`output_hand_over()` in
`output.c`). No window is ever moved because a monitor went away. With more than one
output, a window sticking out of its output is trimmed at the edge
(the `cut_left` and `cut_right` of `window_place()`), or it would show up
on the neighbour. A monitor plugged
in while the screens are off stays off with them, with no mode yet; it
holds its workspace all the same (`output_showing()` asks only whether an
output is in use), so that a second one plugged in with it takes another,
and the workspace waits suspended until the screens come on. An output that
is not for a desktop (a VR headset, `non_desktop`) is left alone.

An output's scale and place come from the entry in `src/machines.h` for the
machine tw runs on, picked once as tw starts by the board name in
`/sys/class/dmi/id/board_name` (`machine_pick()` in `main.c`), as are the
machine's sleep settings. Picked at startup rather than when the build is
set up, so that one installed tw is right on each machine its disk boots.
An output the entry does not name is at scale 1, right of the others.

**Moving between monitors.** Mod with left or right moves the focus along
the strip and, past its end (or on an empty strip), on to the monitor on
that side; Mod+Ctrl does the same with the focused column, which comes in at
the near end of the strip there, so the opposite key brings it back.
Mod+Shift+Ctrl takes it to that monitor's workspace after the column
focused there instead. All of them come down to `window_carry()`.

With the mouse, Mod and the left button pick up the window under the
pointer and the release puts it down (`move_begin()`, `move_end()` in
`input.c`, `strip_drop()`): over another column, its column goes beside
that one, on the side of its middle the pointer is on, on whichever
monitor that is; over an empty part of another monitor, or a dialog there,
onto that monitor's workspace after its focused column. A dialog only
changes workspace. The button is tw's: the window's client hears neither
its press nor its release, and while it is held the pointer is over no
window and shows a grabbing hand. Nothing moves on screen until the
release. That keeps it to a hit test at each end and no work in between,
and draws nothing tw would have to draw itself. A move starts only when no
other button is held (a client waits for its release) and no drag and drop
is under way, never while locked, and a release after the session locked
or the window closed puts nothing down.

**The lid.** A laptop's own screen (a connector named `eDP-`, `LVDS-` or
`DSI-`) behind a closed lid is taken out of use while another output is
there, as niri does: out of the layout, its layer surfaces closed, turned
off (`output_leave()`), and its workspace goes as it does when an output is
unplugged. Left in, it would keep a workspace nobody can see, and the
pointer and new windows could land there. It comes back, placed by its
entry in `src/machines.h`, when the lid opens or the last other output goes away
with the lid closed, so that there is always a screen; it shows the
workspace it showed before if no other output has it by then. A screen
plugged in behind a closed lid takes over from the laptop's own at once.
`outputs_lid()` runs on each move of the lid and each output that comes or
goes, and `tw_lid_look()` in `layout.c` decides. Blanking leaves the screen
behind the lid off, and the lock does not wait for it. A VT switch takes
the lid away and gives it back (libinput drops all devices while the
session is away), and libinput reports a lid it adds only if it is closed:
so a lid tw last saw closed is taken to be open when it comes back, unless
it says it is closed in the same dispatch (an idle callback in
`new_switch()` waits for that). Otherwise a lid opened while away would
leave the laptop's own screen dark until it was closed and opened again.

**Mesa's shader compiler** is freed once the renderer exists
(`renderer_release_compiler()` in `output.c`, called wherever a renderer is
made). wlroots' GLES2 renderer compiles its four shaders while it is created
and never again, yet Mesa keeps its GLSL compiler's library of built-in
functions for the life of the process. `glReleaseShaderCompiler()`, the
GLES2 call made for this, drops it into the heap and `malloc_trim()` hands
the pages back: tw's own memory went from 14.5 to 8.8 MB, headless with the
GPU renderer. Trimming alone frees about 40 kB, and a shader compiled later
would make Mesa build the library again, costing the memory but nothing
else.

**A GPU reset** (a driver hang, a bad wake-up) takes the renderer and all
it made with it. `handle_renderer_lost()` in `output.c` hears it in the
middle of drawing a frame, so it only schedules `renderer_replace()` on an
idle callback: the old renderer cannot be freed under its own signal. That
makes a new renderer and allocator and hands them to the compositor and
every output; without that the screens would stay black until tw was
started again.
Each output that took them is asked for one frame, drawn whole, since
nothing else would repaint an idle screen (the lock screen after a bad
wake-up). If an output cannot take the new ones, the old ones are kept
until tw exits, since that output still points at them; it cannot happen
while the same backend gives the new pair the same buffer capabilities. The
pointer's image goes with the old renderer, and wlr_cursor uploads it again
only when it changes, so the cursor is attached to the output layout anew.
Every client's picture loses its texture too, and wlroots imports a
client's buffer only when it is committed. Worse than blank: the scene
damages a buffer it cannot draw on again every frame, so a screen showing
one would repaint at full rate until that client drew again, which a
wallpaper never does. So `surfaces_redraw()` walks the whole scene, hidden
windows and the lock screen included, and tells every mapped window, layer
surface and lock surface again what it was last told (the last size sent,
acked or not, so as not to undo a newer one). Clients draw on a configure;
one that answers it without a new picture stays blank, and its screen
busy, until it draws.

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
the column edge to settle on, nearest to where the release speed would
carry it. That speed (`tw_swipe_speed()`) is measured from the events'
timestamps; libinput sends none while the fingers rest, so fingers that
held still for 0.1 s before lifting, or between two movements, count as
stopped rather than keeping the speed they last had. A window is drawn where `view_pos` puts it,
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
session locks), and so does each step of a glide or a swipe. Without that,
a click or a scroll after a lock, a workspace change, a window closing or
the strip gliding went to the window that had been under the pointer
before. A held button keeps the pointer on the surface its press went to,
wherever the pointer goes (`pointer_update()`), as long as that surface is
drawn and no grab (a drag, an open menu) is in charge: wlroots has no
implicit grab and forgets the buttons of a surface the pointer leaves, so a
selection or a scrollbar dragged past a window's edge would never hear the
release. When the last button comes up, the pointer goes to what it is
over.

A window can ask for the focus (xdg-activation). It gets it, and its
workspace is shown, only if its token was made from an input event and
names the surface that event went to; wlroots checks that the surface had
the keyboard or the pointer. A token without an event can be made by any
client at any time, and one without a surface takes any serial the client
was ever sent, the pointer merely passing over it.

**The pointer.** A client may lock the pointer in place and read raw mouse
movement instead (games, 3D editors, and X11 programs through Xwayland), or
confine it. A constraint is in force only while its window has both the
keyboard and the pointer, and the pointer is inside the constraint's
region; `pointer_constraint_update()` re-derives that after either focus
changes, after each pointer movement and when the client sets the region
(which it first knows at the commit after the request), so every focus
binding releases a lock. Taking hold only inside the region is what the
protocol asks, and wlroots cannot confine a pointer that starts outside
it: the pointer would roam free while the client thinks it confined.

**Drag and drop.** A client starts a drag from a button press; tw lets it
if the press was real (its serial) and the session is not locked, and
wlroots' drag grab then tells each window the pointer passes over. The
picture a client drags, if any, is in `drag_icon_tree`, the top of the
scene, moved with the cursor. wlroots takes the picture's input region
away, so the hit test sees through it. Unlike a click, a drag follows the
windows as the pointer focus does (`pointer_refocus()` runs for it with
the button held): carried to another workspace, it is over what is there,
and let go over nothing it lands nowhere. A refused drag is cancelled by
destroying its data source; one without a source (a drag within one
client) cannot be, and wlroots 0.20 has no call to free it, so each such
refusal leaks the few hundred bytes of the `wlr_drag`. Only a client
starting source-less drags with stale serials hits that.

**Menus** (xdg popups) are kept on the output: tw hands wlroots the
output's box when a menu first appears and again whenever its client moves
it (`reposition`, which GTK 4 and Firefox use for menus that follow what
they belong to). A layer surface's menu is kept on its layer surface's
output. A window's menus are drawn in `popup_tree`, above every window,
dialog and top-layer surface: under its window's own tree, the menu of a
tiled window would open behind an open dialog, unseen but holding the
keyboard and the pointer. Each window's `popup_tree` follows its window's
position and visibility. A layer surface has a `popup_tree` of its own for
the same reason: in `server->popup_tree`, where a bar on the bottom layer
(waybar's default) opens its menus over the windows rather than under them,
and on the overlay layer just above its surface, since `server->popup_tree`
is below that layer. It follows the surface's position each time the
surface is configured, and its layer when that changes. A menu's own tree goes with the menu, or with what
it hangs from if that goes first (`popup_attach()`), so a menu's surface
outliving either never points at a freed or stale tree.

**Keys.** Direction keys follow one scheme: I, J, K, L are up, left, down,
right, and the arrow keys do the same. Alone they move the focus; with Shift
they go to the monitor on that side or, with none there, to the previous or
next workspace; with Ctrl the focused window or column is taken along. Left
and right, alone or with Ctrl, go on past the end of the strip to the
monitor on that side (**Moving between monitors** above). The
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
keyboards: the hotkeys and the power button are keyboards too. If the
variables name a layout xkeyboard-config does not have (an update renamed a
variant, or they came from another machine), tw logs an error and uses
xkbcommon's built-in default (US): with no keymap at all nobody could type,
not even a password into the lock screen, and Ctrl+Alt+F<n> would be gone.

**Levels.** The volume, brightness and keyboard-light keys run a command
from `config.h` that changes the level and then shows the new one with
`notify-send`. Each sends the same tag (the `x-canonical-private-synchronous`
hint) every time, and `tw-notify` replaces the notification with that tag
rather than add one, so holding a key shows one line counting up. A level
changed any other way (a mixer, the kernel's own keys) does not show:
watching PipeWire or the backlight would take a program running all
session.

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
wallpaper or a bar. wlroots takes a surface's exclusive zone off the room
only once the surface is mapped, after its initial commit was arranged, so
a surface with one is arranged again as it maps: a bar's room is kept from
its first frame on.

## Screen off and lock

**Idle** (`idle.c`). Three steps, each after its own time without input:
the outputs go off (`blank_after_seconds`, 120), the session locks
(`lock_after_seconds`, 900) and the machine is suspended (the machine's
`suspend_after_seconds`: 1800 on the G533ZW, 0 on the G615LM). Blanking is only
that: nothing is locked, programs go on working, and any input brings the
screen back. The screen is 2.4 W of an idle laptop's 6.4
([g533zw.md](g533zw.md#the-screen-and-what-a-frame-costs)), so it goes off
soon, and locking comes much later so that one is not forever unlocking.
One timer serves all three, not re-armed per input event: input records a
timestamp, and when the timer fires it does what is due and sleeps until
the next thing is. A notification turns the screen back on for another
`blank_after_seconds` (`idle_wake()`, from `layer.c`: a surface on the top
or overlay layer that does not ask for the keyboard, as `tw-notify` and
mako draw, at its first commit). It is not input: locking and suspending
count from the last input alone, so notifications never keep an empty desk
unlocked, and the screen's own clock counts from whichever came later. A
launcher asks for the keyboard and comes from input anyway. A locker that
is not installed takes the lock step out as tw starts, as a missing suspend
command does the suspend one: a lock that is due and refused would keep the
timer firing. A visible surface with
an idle inhibitor holds all three off; one on a hidden workspace,
or a layer surface behind the lock curtain or on a screen that is off, does
not. While the outputs are off every window is suspended, and the pointer
is over nothing (`pointer_refocus()`): a cursor a client chose, an animated
one included, which wlroots steps with a timer, would otherwise stay on the
dark screen. The client under the pointer gets it back as the outputs come
on. Input and an
inhibitor letting go restart the clock the same way (`idle_clock_restart()`),
failed suspend attempts included.

While the user is on another VT, tw gets no input and wlroots has taken its
outputs away, so the clock stops (`session->events.active`): nothing blanks,
locks or suspends behind the back of someone working on a text console.
Coming back counts as input, which also turns on the outputs wlroots
brings back, should the screen have been off as they left.

An output that refuses to come on, or to be set up as it is plugged in,
would otherwise stay dark until something else touched it. Input tries such
outputs again, at most once a second while it lasts, so that one that keeps
refusing does not fill the log at the pointer's rate.

Nothing tells a compositor that the machine was suspended and has woken, so
`idle.c` keeps a timer that never expires and that the kernel cancels when
the wall clock jumps: free until it happens, and it is what turns the screen
back on after a wake-up. The clock also jumps when the time service sets
it, so a jump counts as a wake-up only if the time spent asleep
(`asleep_ms()`, the difference of CLOCK_BOOTTIME and CLOCK_MONOTONIC) grew.

**Lock** (`lock.c`). tw draws no lock screen. A lock client (swaylock) asks
for the lock through ext-session-lock, or tw takes it by itself with
`lock_now()`: on Mod+Alt+L, after `lock_after_seconds` without input, before
a suspend, and when the laptop's own screen is left the only one behind a
closed lid: the lid closes with no monitor plugged in, or the last monitor
is unplugged while it is closed. Those are the times logind suspends the
machine. A docked laptop whose lid closes does not lock: logind does not
suspend it (`HandleLidSwitchDocked=ignore`), the user works on at the
monitor, and its own screen goes out of use instead (**The lid** under
[The strip](#the-strip)). `lock_now()` sets `server->locked` at once, which
hides every window behind an opaque curtain, and then starts the locker,
which takes the lock over. So `locked` with `server->lock == NULL`
is a normal state: the locker has not arrived yet, or it died. A key press
in that state starts the locker again, at most once a second. A lock
surface's first picture takes the pointer if it is under it, which rests as
the locker arrives: a click reaches it without the mouse moving first.

A locker that is installed but cannot start (its config rejected after an
update, a library missing) leaves the session locked with nobody to unlock
it. tw does not unlock by itself; any ext-session-lock client may take the
lock over and unlock it, so the way out is from a text console: a locker
that works, started with `WAYLAND_DISPLAY` set to tw's socket (for
swaylock, `swaylock -C /dev/null` skips a broken config).

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
turned away like one queued behind a helper that died. The sockets are
non-blocking: the dead helper's Xwayland may still be accepting on them for
a moment, and turning the queue away must not leave tw waiting in
`accept()` for a connection Xwayland took first.

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
empty), until the next copy replaces it. The offer carries the serial of
the copy it stands in for (the seat keeps it after the source is gone):
wlroots refuses a window's copy whose input serial is older than the
selection's, and a fresh serial would refuse copies that would have
replaced the original, such as one a program makes when an action started
earlier is done. tw knows its own source by its `impl` and does not read
that one. A program that asks for an empty clipboard is obeyed:
`handle_request_set_selection()` calls `clipboard_cleared()` first, which
is how a copy taken back on purpose is told from one whose program exited.

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
- A paste under way holds its format until the reader has it all or closes
  the pipe, even if the copy is replaced meanwhile: cutting it would give the
  reader part of the data as if it were whole, which no program does to a
  paste it has begun. So a reader that stops reading and keeps its pipe open
  keeps that format in memory beyond `clipboard_keep_bytes` until it closes
  the pipe or tw exits. It costs no wakeup: the pipe never becomes writable.
- No timer and no polling: with a copy held and nothing happening tw does
  not wake.

What that comes to, measured: 28 kB for a line of text, and 20.4 MB for a
text of 20 MB offered in five formats (`wl-clip-persist` held 98 MB for
it), back down after the next small copy.

Writing to a reader that has left would kill tw, so tw ignores SIGPIPE.
Every program tw starts gets it back through `child_reset()` (`main.c`),
which also gives it `/dev/null` for stdin rather than the console tw may
have been started from, and lets no file tw has open, its libraries'
included, through exec: a GPU device left open in a program would keep
`tw-gpu` from switching that GPU off.

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
supply that is not a battery is online (any `online` value but 0: a USB-C
port reads 2 while it charges at a programmable voltage). Supplies of scope `Device` belong to
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
program that animates draws as often as the output refreshes. Rates count
in whole Hz, so a 60 Hz monitor is not switched to the 59.94 Hz mode it
also lists: that would blink the screen for no saving
(`tw_refresh_saves()`).
`output_mode()` is the one place that says which mode an output should be
in; a new output, an output coming back on and a change of power supply all
ask it. wlroots refuses a mode for an output that is off, so
`outputs_update_mode()` skips those and `output_set_power()` sets the mode
as it turns one on. If the output refuses it, it gets its preferred mode,
then its old one, then the first of its modes it takes:
a dock, a hub or a cable may not carry the preferred mode, and a monitor
just plugged in has no old one. A dark screen is the one failure this must
not have.

**Fewer frames.** A screen's self-refresh (PSR) starts only once the
picture has stood still for about 100 ms (six frames at 60 Hz). A program
that changes it a few times a second, such as Claude Code's status line,
keeps the display engine and its link awake throughout: on the G533ZW, ten
updates a second cost 1.7 W and two cost 0.2 W
([g533zw.md](g533zw.md#the-screen-and-what-a-frame-costs)). So on battery,
once nobody has touched anything for `battery_idle_after_seconds` (5),
`handle_frame()` shows at most `battery_idle_fps` (2) frames a second per
output. A frame that comes too soon is held back with its frame-done
events, so a client that waits for those (most do) simply draws less
often, and a one-shot timer per output (`frame_timer`) shows it when its
turn comes. Input releases held frames at once (`outputs_release_frames()`
from `idle_activity()`), so typing and scrolling never see the cap. It is
off while a visible window inhibits idling (players do while playing, and
SDL and Godot games by default), while any window on the output's workspace
is fullscreen, while a program holds the pointer (`active_constraint`), while
the session is locked (the curtain must show at once), and during a scroll
animation. Nothing is held when nothing changes, so an idle desktop still
wakes no one.

**The power profile.** `battery_command` runs when the cable is pulled,
`mains_command` when it is back, and one of the two at startup. They are
`tw-power saver` and `tw-power balanced`; see
[Power profiles](#power-profiles-tw-power). One runs at a time, as a child
tw watches through a pidfd for as long as it runs: a cable that moves twice
at once (a loose plug, a USB-C charger agreeing on its power) would
otherwise start two that race to the root helper, and the one that holds
could be the one for the state that went. When it exits and the state has
changed since, tw runs the command for the state now.

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
the bus (`ReleaseName`) and exits when the answer to that arrives, after
answering whatever came with it: whatever
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
somebody else's notification by accident. They stay below 2^31, wrapping
there: libnotify keeps an id in a signed int, and `notify-send -r` aborts
on one that does not fit. A notification may instead carry a tag (the
`x-canonical-private-synchronous` hint, which most servers take), and
replaces the one with the same tag; the level keys use it (**Levels**
under [The strip](#the-strip)). A tag longer than 63 bytes counts as none,
since two cut to the same length could replace each other.

**It speaks D-Bus by itself.** The messages are read and written by
`bus.c`, with no D-Bus library: the login line, `Hello`,
`RequestName`, the four methods of `org.freedesktop.Notifications`, `Ping`
(with or without its interface named), an empty `GetAll`, `Introspect`, and
the signals. Everything read off the socket is checked against the bytes
that are there, and a `Notify` body must be exactly what its signature
says, to the byte; a variant, in the header or in a value, holds exactly
one complete type; `bus.c` reads it (`bus_notify_read()`), so the unit test
reads it with the code that runs. `tests/bus_test.c` is mostly about
messages that are cut short or damaged. A message that cannot be read is
passed over: a call among them gets `InvalidArgs` back if its header got as
far as its sender, and a big-endian one, which is not read, is passed over
by its length. Only a message whose length cannot be told (another version
of the protocol) ends `tw-notify`; that and every other failure it cannot
go on from (no memory, no picture) close its notifications first, so their
programs are told. A protocol error from the compositor is written to
stderr.
Signals go to the program that sent the notification and to nobody else.

**Text.** The font is `font` in `notify/config.h`, found through
fontconfig and drawn with FreeType, so any character some installed font
has is shown: fontconfig's fallback list is walked for each character a
font lacks. Colour (emoji) and bitmap fonts are passed over, since their
pictures come in fixed sizes, and so are the pictures some outline fonts
(CJK ones such as WenQuanYi) carry for small sizes, one bit per pixel: only
outlines are drawn. Both libraries are opened with the first
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
plugged in. So a warning that does not expire is not left out of sight. Over
the list the cursor is the default arrow, named through cursor-shape on
each enter; otherwise it would keep what the window below set, a hidden
one included.
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

## The terminal

The terminal is foot in server mode. `config.h` starts `foot --server`
with the session, the one program in its autostart list, and Mod+T runs
`footclient -E -N`: a new window of that one process. Each `foot` of its
own is about 10 MB of its own memory (fonts, glyph caches, the parsed
configuration) besides its window's buffers; the server has these once, so
every terminal after the first is that much cheaper and opens sooner. The
first costs the same as before, but the server's 10 MB are there from login,
before any terminal is open. The server draws each window with 4 threads
(`-o main.workers=4`) rather than foot's default of one per CPU: 20 threads
a window on the G533ZW, each with its stack and its kernel stack, for about
1 MB; measured headless with two windows, 41 threads and 5.9 MB against 9
threads and 5.6 MB, the rest in the kernel.

- `-E` runs the shell with footclient's environment, which is tw's, rather
  than the server's, which was tw's at login: `DISPLAY` and anything set
  since reach the shell as they would a `foot` tw started.
- `-N` returns at once, so `|| foot` starts a `foot` of its own only when
  no server answered: the server crashed, or it is not installed.
- The server listens on `$XDG_RUNTIME_DIR/foot-$WAYLAND_DISPLAY.sock`,
  which footclient finds by the same name. A tw run nested has its own
  server, and its terminals open in it rather than in the session around it.
- It is started by tw, not by systemd's `foot-server.socket`: that unit
  only starts once systemd has been told `WAYLAND_DISPLAY`, which the
  session script does at the same moment tw starts its autostart list, and
  a unit under systemd takes every program started from a terminal with it
  when it stops.

What it costs: a crash of the server closes every terminal, and all of
them share one thread for input and output, so several terminals printing
heavily at once (two builds side by side) are a little slower than they
were in separate processes.

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
- **Hands on `DISPLAY` only if it is tw's.** Only the startup command,
  which tw runs, knows whether tw opened an X11 display. Without one, a
  `DISPLAY` an earlier Plasma session left in systemd would send the
  programs D-Bus and systemd start to an X server that is gone, so it is
  taken back: `dbus-update-activation-environment` cannot unset a variable
  and is handed `DISPLAY=` (an empty one, which Xlib reads as none), then
  systemd unsets it.
- **Takes it all back when tw exits.** systemd keeps what it was handed
  for as long as the user is logged in anywhere (the user manager outlives
  a logout by `UserStopDelaySec`, or for good while anything of the session
  is left), and Plasma imports systemd's environment as it starts: it
  would run with the accessibility bus off and its programs kept off the
  NVIDIA GPU. So the script does not `exec` tw but waits for it (one idle
  shell, about 300 KB, no wakeups), and then has systemd unset every name
  in `service_env` and `DISPLAY`. A `trap` on `HUP` and `TERM` keeps the
  shell until tw is gone when the session is ended from outside. D-Bus's
  own copy cannot be unset and stays; it reaches only the services D-Bus
  starts without systemd.
- **Says what to do when tw cannot start** because a library it was built
  against is gone after a system update (the loader's exit status, 127).
- **Runs `~/.config/tw/autostart`**, if there is one, once tw is up.

If seatd or logind goes away while tw runs, wlroots loses the seat and
with it every GPU and input device, which nothing can bring back. tw then
exits with status 1 (`handle_session_destroy()` in `main.c`) rather than
stay up with a black screen and no keyboard, so the login ends and can be
started again.

The desktop portals are not the script's: `tw-portals.conf`, installed for
xdg-desktop-portal, has screen capture and screenshots served by the wlr
backend and everything else (file dialogs) by the GTK one.

Nor is the power key. logind powers the machine off at a short press
unless a desktop takes the key over, as KDE does to ask first. tw asks
nothing, and on the G615LM, whose power key sits beside the USB ports, a
press while plugging a mouse in ended the session. `tw-logind.conf`,
installed for logind, makes a short press do nothing and a press held for
five seconds a clean power-off. On a laptop whose firmware cuts the power
after four seconds, that clean power-off never comes; `systemctl poweroff`
always works. Because it is logind's setting, it holds at the text console
too. The key still wakes a machine from sleep, which is the firmware's
doing.

`data/post-install.sh` is what `meson install` runs last: it has systemd,
polkit, udev, logind and the installing user's systemd read the new files,
and does nothing for a staged install.

## The NVIDIA GPU: tw-gpu

On a laptop that also has an Intel (or AMD) GPU the NVIDIA GPU is off until
something needs it, and none of this is in tw's C code.

**Off** means its kernel driver is unloaded and the GPU is off the PCI bus
(`remove` on the device; the root helper leaves the port's path in
`/run/tw-gpu-off`, and scans that port to bring the GPU back, removing the
note only once the GPU is back: without it the machine would pass for one
with no NVIDIA GPU). The driver goes module by module, and before the
core `nvidia` whatever else holds it, by what the kernel lists as its
holders: a module a driver release adds would otherwise keep it loaded for
good. Unloading alone is not enough: NVIDIA's libraries load the driver through their setuid
`nvidia-modprobe` as soon as a program asks about the GPU, which `btop` and
`fastfetch` do, and modprobe's configuration cannot refuse it (kmod ignores
`install` commands for a module with soft dependencies, which `nvidia`
has). With no NVIDIA GPU on the bus the driver itself refuses to load.

**`tw-session`** does three things for this. A GPU, for it, is any display
controller on the PCI bus (class `0x03`): VGA, but also 3D (`0x0302`),
which is what the NVIDIA GPU of many laptops calls itself and which has no
`boot_vga` file. The first GPU is the one with the built-in panel connected
(eDP, LVDS, DSI); without one (a desktop), the one the firmware showed the
boot screen on (`boot_vga`), or else the first that is not NVIDIA's. The
panel comes before `boot_vga` because the kernel picks the boot VGA device
among VGA controllers only: a laptop whose integrated GPU is of another
class (`0x0380`, some AMD ones) has it on the NVIDIA GPU. It names the GPUs tw may use in
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
`tw-gpu login`, unless a monitor is on the NVIDIA GPU; what that says goes
to the journal (`journalctl -t tw-gpu`). With `TW_ALL_GPUS=1` it does none
of this, but brings back a GPU that `tw-gpu` switched off earlier in the
boot, with `tw-gpu run true` before tw starts: `on` would leave it kept on
for the sessions after.

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
started by `run` inherits a shared lock on it, and the switch-off
(`settle`), arranged before the program starts and in a session of its own,
waits for the exclusive lock. So there is no counter to get out of step.
One `settle` waits at a time (a second lock, `tw-gpu.settle`, held only
while waiting): a program that stays all session, Steam, would otherwise
gather one waiting for each game started beside it.

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
when every function on it is asleep. While the GPU is off, those other
functions' `link/clkpm` in sysfs must not be read: kernel 7.3.4 frees the
link's power state along with the GPU but keeps the file, and reading it
crashes the reading program with a NULL dereference in the kernel
(`clkpm_show`). That was seen on the G615LM, from a probe that read every
`/sys/bus/pci/devices/*/link/*`. Nothing tw runs reads that file. A
program killed there may never release the file, and the next `tw-gpu on`
re-creates it, so reboot before using the GPU again.

The helper leaves alone a GPU that another driver than NVIDIA's has
(nouveau), and one whose driver is no longer on the disk: after a kernel
update the running kernel's modules are gone until the next boot, and a
driver unloaded then could not be loaded again. `tw-gpu` then keeps it on
and says to reboot. A machine with no NVIDIA driver installed at all is
told apart by the running kernel's own modules still being there, and is
told so instead. `tw-gpu` also keeps the GPU on whenever tw has it open,
and `off` says so: when `WLR_DRM_DEVICES` names it, or names no GPUs in a
tw session (`XDG_CURRENT_DESKTOP=tw`; one GPU, or `TW_ALL_GPUS=1`), so that
wlroots opened them all. Outside a tw session (Plasma, a text console) it
goes ahead, and the root helper leaves the GPU on if anything has it open.

Switching on has a trap: the kernel announces the GPU before a driver can
bind to it, udev loads the driver in answer, that load fails ("probe
routine was not called"), and a load made right then fails with it. The
helper runs `udevadm settle` first and tries the load up to five times.

Switching needs root: `tw-gpu` starts `tw-gpu@on.service` or
`tw-gpu@off.service`, which a polkit rule allows, and those run
`tw-gpu-root`. The two units could run side by side, so the helper takes
a lock (`/run/tw-gpu.lock`) and does one thing at a time. When a unit
fails, `tw-gpu` says which journal to read. `tw-power` and `tw-bluetooth`
are built the same way; `tw-power`'s lock does not order two profiles (tw
runs its own one at a time), it keeps one set by hand at the same moment
from mixing with tw's.

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
during an `on` waits for it, and then does nothing: an `idle` whose note is
less than five minutes old comes from a timer the `on` has just started
over. `tw-power`'s helper takes a lock too.

**On** leaves `/run/tw-bluetooth`, loads the drivers, waits for an adapter,
lifts the radio's block and starts `tw-bluetooth-idle.timer`. It lifts the
block a second after udev has settled, not at once, because systemd puts
the block back to what it was when the adapter was last there
(systemd-rfkill) and would undo it; once it has seen the block lifted, "not
blocked" is what it puts back from then on.

`on` also starts the service itself at the end, since an adapter that was
there already (an earlier `off` that could not unload its driver) brings
no event that would start it. Without bluez there is no service to start:
the adapter is on all the same, and the helper says what is missing.

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
helper (`idle`) switches off unless a connection exists, or a Bluetooth
module it would unload is no longer on the disk for the running kernel
(after a kernel update, until the next boot; a driver the file names that
is not loaded here does not count). Both only while
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
`powersave` first, as the daemon does. A CPU with no preference to set
(acpi-cpufreq, or either driver in passive mode) keeps its governor: there,
`powersave` means its lowest speed all the time. Nothing else in cpufreq or
intel_pstate moves with the daemon's profiles, and ASUS's
`throttle_thermal_policy` follows the platform's profile by itself.

`tw-power` has them written by a root helper through a oneshot unit, the
way `tw-gpu` does its switching, and nothing stays running. The three
helpers' units give up after 90 seconds, so a driver that hangs cannot
hold every later start forever, and are never refused for starting too
often, nor is the Bluetooth timer: a loose cable or a device dropping in
and out starts them many times. A
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

## The built-in keyboard

A laptop's own keyboard is often a USB device, and the kernel keeps a USB
device awake unless told it may sleep. Awake, it is polled every few
milliseconds, and on both of the owner's laptops that alone kept the
processor out of its deepest idle state (package C10). Letting it sleep took
the idle G615LM from 7.37 to 6.29 W and C10 from none to 56% of the time
([g615lm.md](g615lm.md#battery)), and the G533ZW from 8.36 to 6.45 W and
C10 from none to 78% ([g533zw.md](g533zw.md#before-and-after)). `tw-keyboard.rules` tells the kernel so
for a keyboard that is built in: one on a port the firmware calls `fixed`,
with a boot-keyboard interface (HID class 03, subclass 01, protocol 01) in
udev's `ID_USB_INTERFACES`. It sleeps two seconds after the last key (the
kernel's default delay), and the next key wakes it and is not lost. A
keyboard or mouse plugged in is left alone: such devices are not all able to
wake by themselves.

The rule applies as the keyboard appears, which is at boot; after an
install, `echo auto | sudo tee /sys/bus/usb/devices/<keyboard>/power/control`
does the same until then.
