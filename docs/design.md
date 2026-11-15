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
chose.

**Outputs.** Each output shows one workspace (`output->workspace`), and no
two outputs show the same one; the rest are hidden. `server->active` is the
workspace with the focus, and the output showing it is the focused output:
new windows open there. Going to a workspace that another output shows moves
the focus to that output; the workspace stays where it is. Unplugging an
output only hides its workspace, so no window is ever moved because a
monitor went away. With more than one output, a window sticking out of its
output is trimmed at the edge (`window_trim()`), or it would show up on the
neighbour.

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

A window can ask for the focus (xdg-activation). It gets it, and its
workspace is shown, only if its token was made from an input event: a token
without one can be made by any client at any time.

**The pointer.** A client may lock the pointer in place and read raw mouse
movement instead (games, 3D editors, and X11 programs through Xwayland), or
confine it. A constraint is in force only while its window has both the
keyboard and the pointer; `pointer_constraint_update()` re-derives that
after either focus changes, so every focus binding releases a lock.

**Keys.** Direction keys follow one scheme: I, J, K, L are up, left, down,
right, and the arrow keys do the same. Alone they move the focus; with Shift
they go to the monitor on that side or, with none there, to the previous or
next workspace; with Ctrl the focused window or column is taken along. The
`DIRECTION` macro in `config.h` binds all four meanings of a key at once.
The other bindings follow niri's defaults. Bindings match on the unshifted
keysym, so Shift combinations do not depend on the layout.

**Layer surfaces.** Launchers, bars and notifications are layer-shell
clients (`layer.c`). A new layer surface is told its screen's scale as it is
created: wlroots' scene tells a surface its scale only once it is on screen,
by which time a client has drawn it for scale 1 and has to draw again.
Windows still learn their scale the late way.

## Screen off and lock

**Idle** (`idle.c`). One timer, not re-armed per input event: input records
a timestamp, and when the timer fires it does what is due (blank the
outputs, suspend the machine) and sleeps until the next thing is. A visible
surface with an idle inhibitor holds both off; one on a hidden workspace
does not. While the outputs are off every window is suspended.

Nothing tells a compositor that the machine was suspended and has woken, so
`idle.c` keeps a timer that never expires and that the kernel cancels when
the wall clock jumps: free until it happens, and it is what turns the screen
back on after a wake-up.

**Lock** (`lock.c`). tw draws no lock screen. A lock client (swaylock) asks
for the lock through ext-session-lock, or tw takes it by itself with
`lock_now()`: on Mod+Alt+L, when the lid closes, when the screen blanks from
inactivity, and before a suspend. `lock_now()` sets `server->locked` at once,
which hides every window behind an opaque curtain, and then starts the
locker, which takes the lock over. So `locked` with `server->lock == NULL`
is a normal state: the locker has not arrived yet, or it died. A key press
in that state starts the locker again, at most once a second.

While locked no binding runs except VT switching, only lock surfaces get
input, and the session stays locked if the lock client dies. tw never locks
by itself unless the locker is installed: a lock nobody can open is worse
than none.

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

Writing to a reader that has left would kill tw, so tw ignores SIGPIPE, and
`spawn()` and the start of the X11 helper set it back for the programs tw
starts.

**A program that dies while being read.** A pipe ends the same way when its
program has written everything and when the program died over it. The
kernel closes a dying program's files from the highest number down, which
puts the pipe before the connection to tw, so tw can have taken a format
that was cut short as read, and asked for the next one, before it learns
that the program is gone. What tells the two apart is that a dead program
never reacts to that next request. So when a source goes away while it is
being read, the newest format read counts only if something came of the one
after it (`answered`), the format on its way is dropped, and a format that
ended with no bytes is never kept.

Known, and left: the last format a program offers has none after it and is
taken as it is, so a program that dies while writing its last format in
more than one piece (over 4 kB) leaves that one cut short on the clipboard.

## On battery

`power.c` does three things, all off one event source. What that costs on a
machine with a battery: a socket that wakes tw two to four times a minute
to read a handful of small files, and 20 kB. On one without, nothing.

**Knowing.** `server->on_battery` follows the kernel's own rule
(`power_supply_is_system_supplied()`): the machine has a battery, and no
supply that is not a battery is online. Supplies of scope `Device` belong to
a mouse or a phone and are skipped. Changes arrive through a libudev monitor
on the `power_supply` subsystem (wlroots links libudev anyway). tw does not
read what an announcement says; it reads `/sys/class/power_supply` again and
acts only if the answer changed, because batteries and chargers announce
themselves every half minute or so with nothing changed.

A machine without a battery opens no monitor and runs no command. Neither
does a tw that is not what runs the machine, one inside another session or
on the headless backend (`server->session` is NULL): it would set the power
profile a second time, next to the session that does. The tests switch it
on all the same by pointing `TW_SYS` at a made-up machine.

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
would. It is built on three decisions, each of which is what makes it small
(168 kB of its own while showing two notifications).

**It runs only while there is something to show.** Its D-Bus service file
makes the session bus start it when a program sends a notification or asks
for its capabilities, and it exits `linger_ms` (one second) after the last
notification has closed or the last question was answered. Leaving must not
lose a notification that is on its way, so it first gives up its name on
the bus (`ReleaseName`) and exits when the answer to that arrives: whatever
was sent before the bus took the name away has been delivered by then, and
whatever is sent after starts a new `tw-notify`. One that arrived in
between is shown, and the name asked for again. Ids start from the clock,
so a program still holding the id of a notification from an earlier
`tw-notify` cannot close somebody else's.

**It links against libc and the Wayland client library, nothing else.** The
D-Bus messages are read and written by `bus.c`: the login line, `Hello`,
`RequestName`, the four methods of `org.freedesktop.Notifications`, `Ping`,
an empty `GetAll`, `Introspect`, and the signals. Everything read off the
socket is checked against the bytes that are there; `tests/bus_test.c` is
mostly about messages that are cut short or damaged. Signals go to the
program that sent the notification and to nobody else.

**It loads no font.** `bake.c` runs when tw is built and draws the
characters of one font (`-Dnotify_font`, by default what fontconfig calls
sans-serif) at `font_size` for five screen scales into `font.h`: Latin with
its accented letters, and common punctuation. They end up as read-only data
in the program (320 kB of its 390 kB), which costs no memory of its own.
What that gives up: any other character is drawn as the font's "missing"
box, and there is no bold, no markup and no right-to-left text.

**The window.** The list is one layer surface on the overlay layer, top
right, drawn into a shared-memory picture that `tw-notify` unmaps as soon as
the compositor has it. On a screen with a fractional scale the picture is in
the screen's own pixels (fractional-scale and viewporter), its size rounded
up so that it is a whole number of logical pixels. The overlay layer is
above fullscreen windows, so a game does not hide the battery warning, and
below the lock screen, so a locked session shows none.

A click closes the notification under it. A left click on one whose program
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
or typed on a text console. Never run it from inside a desktop session to
try it out: it publishes its `WAYLAND_DISPLAY` to the user's D-Bus and
systemd environment. The one safe way is
`TW_SESSION_PRINT=1 sh data/tw-session`, which prints what it would do and
starts nothing.

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

`data/post-install.sh` is what `meson install` runs last: it has systemd,
polkit, udev and the installing user's systemd read the new files, and does
nothing for a staged install.

## The NVIDIA GPU: tw-gpu

On a laptop that also has an Intel (or AMD) GPU the NVIDIA GPU is off until
something needs it, and none of this is in tw's C code.

**Off** means its kernel driver is unloaded and the GPU is off the PCI bus
(`remove` on the device; the root helper leaves the port's path in
`/run/tw-gpu-off`, and scans that port to bring the GPU back). Unloading
alone is not enough: NVIDIA's libraries load the driver through their setuid
`nvidia-modprobe` as soon as a program asks about the GPU, which `btop` and
`fastfetch` do, and modprobe's configuration cannot refuse it (kmod ignores
`install` commands for a module with soft dependencies, which `nvidia`
has). With no NVIDIA GPU on the bus the driver itself refuses to load.

**`tw-session`** does three things for this. It names the GPUs tw may use in
`WLR_DRM_DEVICES` whenever the machine has a second GPU, be it loaded,
without its driver, or switched off, because wlroots opens a GPU that
appears later only when that variable is unset; so the GPU can come and go
under a running tw. It keeps programs on the first GPU
(`__EGL_VENDOR_LIBRARY_FILENAMES`, `VK_DRIVER_FILES`), so that none of them
grabs the NVIDIA GPU while it is on for another. And once tw is up it runs
`tw-gpu login`, unless a monitor is on the NVIDIA GPU.

**`tw-gpu run <program>`** switches the GPU on, starts the program with
NVIDIA's offload variables, and switches it off afterwards. The launcher
starts everything through `tw-gpu launch`, which does the same for desktop
entries with `PrefersNonDefaultGPU=true`.

Who still needs the GPU is tracked with one lock file: every program
started by `run` inherits a shared lock on it, and the switch-off, arranged
before the program starts and in a session of its own, waits for the
exclusive lock. So there is no counter to get out of step.

The root helper leaves a GPU alone that any program has open (it reads
`/proc/*/fd`), and then one `tw-gpu retry` waits for that to end: it looks
at the driver's use counts every five seconds and tries again when they
have changed. That is the only polling in all of this. It runs only while
the GPU is on although nothing asked for it, and it is also what waits for
a login screen, which keeps the GPU open for some seconds after tw has the
screen.

Switching on has a trap: the kernel announces the GPU before a driver can
bind to it, udev loads the driver in answer, that load fails ("probe
routine was not called"), and a load made right then fails with it. The
helper runs `udevadm settle` first and tries the load up to five times.

Switching needs root: `tw-gpu` starts `tw-gpu@on.service` or
`tw-gpu@off.service`, which a polkit rule allows, and those run
`tw-gpu-root`. `tw-power` and `tw-bluetooth` are built the same way.

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
root helper reads the names out of it.

**On** leaves `/run/tw-bluetooth`, loads the drivers, waits for an adapter,
lifts the radio's block and starts `tw-bluetooth-idle.timer`. It lifts the
block a second after udev has settled, not at once, because systemd puts
the block back to what it was when the adapter was last there
(systemd-rfkill) and would undo it; once it has seen the block lifted, "not
blocked" is what it puts back from then on.

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
helper (`idle`) switches off unless a connection exists. Both only while
`/run/tw-bluetooth` is there, so a machine that has Bluetooth on from boot
(built with `-Dbluetooth=false`, or before its first reboot) is left alone.
A second udev rule lets a USB adapter sleep (`power/control` to `auto`),
which its driver would do as it loads and nobody does with the driver kept
away.

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

Nothing else in cpufreq or intel_pstate moves with the daemon's profiles,
and ASUS's `throttle_thermal_policy` follows the platform's profile by
itself. `tw-power` has them written by a root helper through a oneshot
unit, the way `tw-gpu` does its switching, and nothing stays running. A
machine that lacks one of the settings is not given it, and a setting that
already reads as wanted is not written.

## Thunderbolt

Thunderbolt devices are let in by a udev rule (`tw-thunderbolt.rules`) and
not by boltd. A port at security level `user` keeps the PCIe side of a
device (a dock's network port and disks, an external GPU) off until
something writes 1 to the device's `authorized` file. boltd does that and
runs from boot for it, because the port's controller counts as a device.
Where the kernel's DMA protection is on (`iommu_dma_protection` reads 1)
boltd approves every device without asking, and the rule, which is the one
in the kernel's own guide, does the same as the device appears. Where the
protection is off the rule does nothing. `-Dthunderbolt=false` leaves the
rule out.
