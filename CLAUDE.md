# tw

A scrolling-tiling Wayland compositor in C on wlroots, in the spirit of niri's
layout and dwl's size. Priorities, in order: no work when idle, low memory,
low latency, small code.

## Build, test, run

If `meson` is not installed, `uvx meson` is a drop-in replacement.

```sh
# Dev build: ASan + UBSan. Use this for all day-to-day work.
meson setup build -Db_sanitize=address,undefined
meson test -C build --suite tw

# Optimized build: static wlroots + LTO. Adds the budget test.
meson setup build-release --buildtype=release -Db_lto=true
meson test -C build-release --suite tw

# Run in a window inside the current session (Mod is Alt when nested).
./build/tw -d -s foot

# Install the binary and the login-screen entry (tw, tw-session, tw.desktop).
# --skip-subprojects keeps wlroots' headers and library out of /usr/local.
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

Always pass `--suite tw`; without it Meson also runs the vendored
subprojects' own tests.

`-d` turns on debug logging; tw logs a `map <app_id> <w>x<h>` line per window
and an `output <name>` line per monitor. `-b <seconds>` and `-z <seconds>`
override the screen-blanking and suspend timeouts from `config.h`, and
`-x <seconds>` how long after the last X11 program the X server is stopped.

Never run `data/tw-session` from inside a desktop session to try it out: it
publishes its `WAYLAND_DISPLAY` to the user's D-Bus and systemd environment.
The one safe way is `TW_SESSION_PRINT=1 sh data/tw-session`, which prints
which GPUs it would use and starts nothing.

## Code map

| File | Role |
| --- | --- |
| `src/config.h` | All configuration, compile-time: gaps, column widths, outputs, autostart, timeouts, key bindings |
| `src/layout.c` | Strip maths: column widths, stacking, view scrolling, the scroll spring. Also the two decisions of the battery mode (which refresh rate, what a falling charge calls for). Pure functions, no wlroots, unit tested |
| `src/window.c` | xdg-shell windows, columns, floating dialogs, popups, decorations, workspaces, focus, focus requests, `strip_arrange()`, scroll animation and swipes |
| `src/layer.c` | Layer shell: launchers, notifications, wallpapers, bars |
| `src/idle.c` | Screen blanking and suspend after inactivity, idle inhibitors, noticing a wake-up |
| `src/lock.c` | Session lock for `swaylock`; locking by itself (`lock_now()`) and starting the locker |
| `src/x11.c` | X11 display sockets; starts xwayland-satellite when the first X11 client connects, and stops it when none is left |
| `src/power.c` | Running on battery: learns from udev when the power cable goes in or out, slows the outputs down and runs the power-profile command; warns and then suspends as the battery runs low |
| `src/output.c` | Outputs, which workspace each one shows, which mode each one runs in, scene layers, the frame handler |
| `src/input.c` | Keyboards and pointers (real and virtual), bindings, pointer lock and confinement, touchpad settings and gestures, lid switch, seat |
| `src/main.c` | Startup, autostart, teardown, `spawn()`, `program_find()` |
| `notify/notify.c` | `tw-notify`, a second program: shows notifications. The notification list, its window, the conversation with the session bus, leaving when nothing is left to show |
| `notify/bus.c` | D-Bus messages taken apart and put together, with no library. Pure functions on bytes, unit tested |
| `notify/bake.c` | Build tool: draws the characters `tw-notify` can show into `font.h`, with FreeType, so that the program itself needs no font library |
| `notify/config.h` | `tw-notify`'s settings, compile-time: sizes, colours, how long a notification stays |
| `data/tw-session` | Session start script: picks the GPUs, switches an unused NVIDIA GPU off, points programs at the keyring service, keeps the accessibility bus off |
| `data/tw-gpu` | Switches the NVIDIA GPU on for the programs that want it and off after them |
| `data/tw-gpu-root`, `tw-gpu@.service.in`, `tw-gpu.rules`, `tw-gpu-udev.rules` | What lets `tw-gpu` load and unload the driver: root helper, system unit, polkit rule, udev rule |
| `data/tw-bluetooth` | Switches Bluetooth on when asked, and off by hand; says which it is |
| `data/tw-bluetooth-root`, `tw-bluetooth@.service.in`, `tw-bluetooth-idle.timer`, `tw-bluetooth.rules`, `tw-bluetooth-udev.rules`, `tw-bluetooth.conf`, `tw-bluetooth-service.conf` | What lets `tw-bluetooth` load and unload the adapter's driver, and what switches Bluetooth off when nothing is connected: root helper, system unit, timer, polkit rule, udev rules, the modprobe file that keeps the driver from loading at boot, and the line added to `bluetooth.service` that keeps the bus from starting it while Bluetooth is off |
| `data/` (rest) | The login-screen entry, the portal configuration, the D-Bus entry that starts `tw-notify` |
| `README.md` | For the owner: what a new machine needs. Packages, install steps, and fixes outside tw (the keyring, shell plugins that wake Xwayland). Add to it whenever a change needs a new program or a step done by hand |
| `protocols/` | Protocol XML that wayland-protocols does not ship, copied from the pinned wlroots |
| `tests/client.c` | Minimal Wayland client (window, layer surface, locker, ...) that prints what the compositor tells it |
| `tests/keys.c` | Presses key combinations and moves and clicks the mouse, through the virtual-keyboard and virtual-pointer protocols |
| `tests/smoke.sh` | End-to-end behaviour on the headless backend |
| `tests/columns.sh` | Windows sharing a column, floating dialogs |
| `tests/pointer.sh` | The mouse: pointer focus, click to focus, pointer lock, confinement |
| `tests/outputs.sh` | Two monitors: a workspace on each, focus and columns between them |
| `tests/idle.sh` | Screen blanking and inhibitors, with a two-second timeout; locking on idle; suspend |
| `tests/x11.sh` | X11 sockets, starting the helper on demand and stopping it when unused, with a stand-in helper |
| `tests/power.sh` | What tw makes of the power supplies it finds as it starts, and the command it runs, against made-up ones |
| `tests/bus_test.c` | `notify/bus.c`: messages written and read back, cut short, damaged; nothing may be read past its end |
| `tests/notify.sh` | `tw-notify` on a private session bus and a headless tw: started on demand, expiry, clicks, leaving |
| `tests/iwd.sh` | `tools/iwd.sh` against a made-up machine: which networks are copied and how, and that every failed or interrupted trial ends on NetworkManager |
| `tests/keyring.sh` | `tools/keyring.sh` against a made-up login file and session: the file comes out and goes back exactly, and what `start` does when oo7 is open, locked or broken |
| `tests/gpu.sh` | `tw-gpu`, its root helper, and `tw-session` (the GPU choice, what it passes on to services), against a made-up `/sys` and stand-ins for `systemctl` and `modprobe` |
| `tests/bluetooth.sh` | `tw-bluetooth` and its root helper against a made-up machine: off at boot, on, off by itself and by hand, a program that still has Bluetooth open, and every way switching on can fail |
| `tests/budget.sh` | Idle-wakeup and memory budgets (release build only) |
| `tests/lib.sh` | Shared by the shell tests: starts tw headless with a private runtime directory and PATH, and no power supplies |
| `tools/power.sh` | Battery draw every two seconds next to frames shown and CPU in use. For telling what an animation or a refresh rate costs |
| `tools/power-root.sh` | What only root can read about the draw at idle: watts by part (RAPL), how deep the package sleeps, the display's power saving, PCIe link states, what the kernel said. The owner runs it with sudo |
| `tools/iwd.sh` | Moves Wi-Fi from NetworkManager to iwd for a trial, and back by itself if the internet is not reachable; `keep` and `back` make either permanent. The owner runs it with sudo |
| `tools/keyring.sh` | Makes the login unlock oo7, the keyring (edits `/etc/pam.d/login`, keeps the old file), switches the running session over and checks it; `back` restores the file. The owner runs it |
| `tools/mem.sh` | Splits the "used" figure of `btop` and `free` into parts that add up to it: held back by the kernel, kernel, shared buffers, each program. For when the owner asks where the memory goes |

There are `TW_WORKSPACES` workspaces, each a strip: a list of columns, left to
right. A column (`struct tw_column`) is a stack of windows, top to bottom,
sharing the column's width and splitting its height; it is never empty and is
freed when its last window leaves (`column_take()`). A fullscreen window is
always alone in its column. Dialogs (windows with a parent or a fixed size)
are not in the strip: they sit in the workspace's `floating` list, centred, at
the size their client chose.

Each output shows one workspace (`output->workspace`), and no two outputs show
the same one; the rest are hidden. `server->active` is the workspace with the
focus, and the output showing it is the focused output: new windows open
there. Going to a workspace that another output shows moves the focus to that
output; the workspace stays where it is. Unplugging an output only hides its
workspace, so no window is ever moved because a monitor went away. With more
than one output, a window sticking out of its output is trimmed at the edge
(`window_trim()`), or it would show up on the neighbour.

`strip_arrange()` decides where the view of each workspace on screen should be
(`view_x`) and places everything; call it after anything that changes a
strip, the focus, a column width, which workspace an output shows, or an
output. Nothing else positions windows. A hidden workspace is not laid out at
all until it is shown again, because sizes depend on the output.

The view glides to `view_x` on a spring. `view_pos` is where it is drawn now;
`strip_animate()` advances it once per frame of the output showing it and
stops asking for frames when it arrives. A three-finger swipe moves `view_pos` directly and
`strip_swipe_end()` picks the column edge to settle on. A window is drawn
where `view_pos` puts it, but told to draw (not `suspended`) as soon as it is
visible at either `view_pos` or `view_x`.

Each workspace has `focused` (tiled or floating; gets the keyboard) and `tiled`
(the strip's focus, which the view scrolls to). They differ only while a
dialog is focused.

Focus has two parts, both derived by `focus_update()`: the active window (the
focused window of the active workspace; its client is told it is activated,
and tw draws nothing to mark it), and
the keyboard, which goes to the lock screen if locked, else to a layer surface
that asked for exclusive focus, else to the active window. Change
`workspace->focused`, `server->active`, `server->keyboard_layer` or the lock
state, then call `focus_update()`; do not send focus events anywhere else.

A window is on screen only if its column is in view, an output shows its
workspace, the outputs are on (`!server->blanked`) and the session is
unlocked. In every other case `strip_arrange()` disables its scene node and
suspends it.

A client may lock the pointer in place and read raw mouse movement instead
(games, 3D editors, and X11 programs through Xwayland), or confine it. A
constraint is in force only while its window has both the keyboard and the
pointer; `pointer_constraint_update()` re-derives that after either focus
changes, so every focus binding releases a lock.

tw locks in two ways. A lock client (swaylock) asks for the lock, or tw takes
it by itself with `lock_now()`: on Mod+Alt+L, when the lid closes, and when
the screen blanks from inactivity. `lock_now()` sets `server->locked` at once
and then starts the locker, which takes the lock over. So `locked` with
`server->lock == NULL` is a normal state: the locker has not arrived yet, or
it died. A key press in that state starts the locker again.

X11 applications are not tw's business beyond `x11.c`: it creates the X11
display sockets, sets `DISPLAY` for the programs tw starts, and on the first
connection hands the sockets to xwayland-satellite, whose windows then arrive
as ordinary xdg-shell windows. While the helper runs tw does not watch the
sockets; when it exits (a pidfd tells us) tw watches them again.

The helper and Xwayland are stopped again when no X11 program is connected,
because many programs only ask X11 a question as they start (the Claude app
does) and would leave 25 MB of X server behind for the session. tw cannot
ask the server who is connected, and neither Xwayland's own `-terminate`
nor anything else can be passed through the helper, so `clients_connected()`
reads it off the kernel: it lists the sockets the helper and its child have
open (`/proc/<pid>/fd`) and looks each one up in `/proc/net/unix`. A
connection that came in through the display's sockets bears their name; one
that is not in that list at all came from another network namespace (a
sandbox) and counts as a client too; the rest are the helper's own. If
anything cannot be read, the helper stays. There is no event for an X11
client leaving, and looking now and then would be polling, so tw looks at
two moments only, `x11_idle_seconds` after each: the helper's start, and
any window closing (`x11_window_closed()`). A client that leaves with no
window closing is therefore noticed at the next window that does. A client
queued behind a helper tw stopped is served by the next one, not turned
away like one queued behind a helper that died.

The NVIDIA GPU of a laptop that also has an Intel one is handled like X11:
off until something needs it, and none of it is in tw's C code. Off means its
kernel driver is unloaded and the GPU is off the PCI bus (`remove` on the
device; the root helper leaves the port's path in `/run/tw-gpu-off`, and
scans that port to bring the GPU back). Unloading alone is not enough:
NVIDIA's libraries load the driver through their setuid `nvidia-modprobe` as
soon as a program asks about the GPU, which `btop` and `fastfetch` do, and
modprobe's configuration cannot refuse it (kmod ignores `install` commands
for a module with soft dependencies, which `nvidia` has). With no NVIDIA GPU
on the bus the driver itself refuses to load.

`tw-session` does three things for this. It names the GPUs tw may use in
`WLR_DRM_DEVICES` whenever the machine has a second GPU, be it loaded,
without its driver, or switched off, because wlroots opens a GPU that
appears later only when that variable is unset; so the GPU can come and go
under a running tw. It keeps programs on the Intel GPU
(`__EGL_VENDOR_LIBRARY_FILENAMES`, `VK_DRIVER_FILES`), so that none of them
grabs the NVIDIA GPU while it is on for another. And once tw is up it runs
`tw-gpu login`, unless a monitor is on the NVIDIA GPU.

`tw-gpu run <program>` switches the GPU on, starts the program with NVIDIA's
offload variables, and switches it off afterwards; the launcher starts
everything through `tw-gpu launch`, which does the same for desktop entries
with `PrefersNonDefaultGPU=true`. Who still needs the GPU is tracked with one
lock file: every program started by `run` inherits a shared lock on it, and
the switch-off, arranged before the program starts and in a session of its
own, waits for the exclusive lock. So there is no counter to get out of
step. The root helper leaves a GPU alone that any program has open (it reads
`/proc/*/fd`), and then one `tw-gpu retry` waits for that to end: it looks
at the driver's use counts every five seconds and tries again when they have
changed. That is the only polling in all of this, it runs only while the GPU
is on although nothing asked for it, and it is also what waits for the login
screen, which keeps the GPU open for some seconds after tw has the screen.
Switching needs root: `tw-gpu` starts `tw-gpu@on.service` or
`tw-gpu@off.service`, which a polkit rule allows, and those run
`tw-gpu-root`.

Bluetooth is handled like the NVIDIA GPU, off until asked for and none of
it in tw's C code, because the owner uses it "VERY rarely" and found
`bluetoothd` running all day. It ran because systemd starts it whenever the
kernel has an adapter (udev gives every Bluetooth device a wish for
`bluetooth.target`), whether the radio is blocked or not. So off is not a
stopped service but a driver that is not loaded: with no adapter nothing
starts the service, the stock `bluetooth.service` stays enabled as shipped,
and the Bluetooth modules are out of the kernel too.

`tw-bluetooth.conf`, installed into `modprobe.d`, blacklists the adapter's
driver (`btusb`). A blacklist stops what udev does at boot, loading by the
device's alias, and not a load by name, so the same file is the list of
what `tw-bluetooth on` loads: the root helper reads the names out of it.
`on` leaves `/run/tw-bluetooth`, loads them, waits for an adapter, lifts
the radio's block and starts `tw-bluetooth-idle.timer`. It lifts the block
a second after udev has settled, not at once, because systemd puts the
block back to what it was when the adapter was last there (systemd-rfkill)
and would undo it; once it has seen the block lifted, "not blocked" is what
it puts back from then on. `off` stops `bluetooth.service` and unloads every
module that holds the Bluetooth core and then the core, in up to five
passes half a second apart: the sound service keeps a Bluetooth socket open
until it has seen the daemon go.

`bluetoothd` cannot simply be stopped, which the first switch-off on the
owner's machine showed. When it leaves the bus, WirePlumber calls it to
take back what it had registered, and the system bus starts a service that
is called while it is not running (the calls that take leave have
auto-start on, unlike the one that looks for it). The service was running
again 7 ms after it had stopped, held `bnep` and the core, and `off`
failed. A second `off` did worse: the service was started again 13 ms
after the stop, the helper unloaded the core while the new daemon had not
opened anything yet and reported success, and the daemon's first socket
made the kernel load the core again. So `tw-bluetooth-service.conf` adds
`ConditionPathExists=/run/tw-bluetooth` to `bluetooth.service`: the service
starts only while Bluetooth is switched on. That is why `on` leaves the
note before it loads the driver, and `off` removes it before it stops the
service. A start that the condition refuses is one line in the journal.

Bluetooth goes off by itself when nothing has been connected for ten
minutes, with no process waiting for that. The kernel keeps each connection
as a device next to its adapter (`hci0:256`); a udev rule starts the timer
over whenever one is removed, and when the timer runs out the helper
(`idle`) switches off unless a connection exists. Both only while
`/run/tw-bluetooth` is there, so a machine that has Bluetooth on from boot
(built with `-Dbluetooth=false`, or before its first reboot) is left alone.
A second udev rule lets a USB adapter sleep (`power/control` to `auto`),
which its driver would do as it loads and nobody does with the driver kept
away. Switching needs root and goes the way `tw-gpu` does it:
`tw-bluetooth` starts `tw-bluetooth@on.service` or `@off.service`, which a
polkit rule allows.

`tw-session` also keeps the accessibility bus from starting (`NO_AT_BRIDGE`
for GTK 3, `GTK_A11Y` for GTK 4), unless `TW_A11Y=1` is set. The programs tw
starts inherit that, but the one that started the bus at every login is
the file-dialog portal, a GTK 3 program that systemd starts, and such
programs only see what `dbus-update-activation-environment` hands on. So
the names to hand on are kept in one list (`service_env`), which the
startup command passes along with where the session is.

On battery tw does two things. Every output runs in the slowest mode of its
preferred size that is not below `battery_refresh_hz` (60), because a
program that animates draws as often as the output refreshes: on the
owner's 300 Hz panel that was 5 W of 16.5. And `battery_command` runs, with
`mains_command` when the cable is back and one of the two at startup, which
puts the CPU on its power-saver profile and back. `output_mode()` is the
one place that says which mode an output should be in; a new output, an
output coming back on and a change of power supply all ask it. wlroots
refuses a mode for an output that is off, so `outputs_update_mode()` skips
those and `output_set_power()` sets the mode as it turns one on, and turns
it on in its old mode if the new one is refused: a dark screen is the one
failure this must not have.

`server->on_battery` follows the kernel's own rule
(`power_supply_is_system_supplied()`): the machine has a battery, and no
supply that is not a battery is online. Supplies of scope `Device` belong
to a mouse or a phone and are skipped. Changes arrive through a libudev
monitor on the `power_supply` subsystem (wlroots links libudev anyway).
tw does not read what an announcement says; it reads
`/sys/class/power_supply` again and acts only if the answer changed. That
matters because the owner's battery and charger both announce themselves
every 35 seconds or so with nothing changed. A machine without a battery
opens no monitor and runs no command. Neither does a tw that is not what
runs the machine, one inside another session or on the headless backend
(`server->session` is NULL): it would set the power profile and suspend
the machine a second time, next to the session that does. The tests
switch it on all the same by pointing `TW_SYS` at a made-up machine.

The owner has no bar, so nothing shows the charge. With each announcement
on battery `handle_change()` reads it (the fullest battery's `capacity`)
and asks `tw_battery_look()` what is due: `battery_warn_command` once at
`battery_warn_percent` (10), and a suspend when the charge has fallen to
`battery_suspend_percent` (5) and again for each percent below that, so
that a machine woken and left unplugged goes back to sleep before it runs
out. Suspending goes the way it does when idle: `lock_now()`, then
`suspend_command`. A reading is `fresh` when somebody has just arrived (the
cable was pulled, tw started, the machine woke up), and then it only sets
the mark the charge has to fall below: nobody is put to sleep the moment
they open the lid. Waking is told from the clocks, not from an event that
could come after the battery's: CLOCK_BOOTTIME runs during sleep and
CLOCK_MONOTONIC does not, so their difference grows only then
(`asleep_ms()`). tw does not look at the charge as it starts, because a
notification sent before `tw-session` has told D-Bus where the session is
would start the notification service without a display, and it would be
dropped; the battery's next announcement comes soon enough. The machine cannot hibernate (swap is zram), so suspending
only buys hours.

Notifications are not tw's business: `tw-notify` shows them, a program of
its own in `notify/` that shares no code with tw and talks to it as any
layer-shell client would. The owner asked for it because fnott, which did
the job before, held 9.5 MB of its own for the whole session (27 MB in
`btop`) from the moment a program first asked what it could do. `tw-notify`
is built on three decisions, each of which is what makes it small.

It runs only while there is something to show. Its D-Bus service file makes
the session bus start it when a program sends a notification or asks for
its capabilities, and it exits `linger_ms` (one second) after the last
notification has closed or the last question was answered. Leaving must not
lose a notification that is on its way, so it first gives up its name on
the bus (`ReleaseName`) and exits when the answer to that arrives: whatever
was sent before the bus took the name away has been delivered by then, and
whatever is sent after starts a new `tw-notify`. One that arrived in between
is shown, and the name asked for again. Ids start from the clock, so a
program still holding the id of a notification from an earlier `tw-notify`
cannot close somebody else's.

It links against libc and the Wayland client library, nothing else. The
D-Bus messages are read and written by `bus.c`: the login line, `Hello`,
`RequestName`, the four methods of `org.freedesktop.Notifications`, `Ping`,
an empty `GetAll`, `Introspect`, and the signals. Everything read off the
socket is checked against the bytes that are there; `tests/bus_test.c` is
mostly about messages that are cut short or damaged. Signals go to the
program that sent the notification and to nobody else.

It loads no font. `bake.c` runs when tw is built and draws the characters
of one font (`-Dnotify_font`, by default what fontconfig calls sans-serif)
at `font_size` for five screen scales into `font.h`: Latin with its accented
letters, and common punctuation. They end up as read-only data in the
program (320 kB of its 390 kB), which costs no memory of its own. What that
gives up: any other character is drawn as the font's "missing" box, and
there is no bold, no markup and no right-to-left text.

The list is one layer surface on the overlay layer, top right, drawn into a
shared-memory picture that `tw-notify` unmaps as soon as the compositor has
it. On a screen with a fractional scale the picture is in the screen's own
pixels (fractional-scale and viewporter), its size rounded up so that it is
a whole number of logical pixels. The overlay layer is above fullscreen
windows, so a game does not hide the battery warning, and below the lock
screen, so a locked session shows none. A click closes the notification
under it; a left click on one whose program listed the action `default`
first gets an activation token from the compositor for that click and sends
it on (`ActivationToken`, then `ActionInvoked`), which is what lets the
program bring its window forward under tw's rule that only a token made
from input may take the focus. `GetCapabilities` says `actions` for that
reason and because Chromium falls back to drawing its own notifications
when a service does not list it.

For the first picture to be right, `layer.c` tells a new layer surface its
screen's scale as it is created. wlroots' scene tells a surface its scale
only once it is on screen, by which time a client has drawn it for scale 1
and has to draw again: seen with `tw-notify` on a screen at 1.25, two
pictures before the change and one after. That holds for the launcher too.
Windows still learn their scale the late way.

A window can ask for the focus (xdg-activation). It gets it, and its
workspace is shown, only if its token was made from an input event.

Direction keys follow the owner's scheme: I, J, K, L are up, left, down,
right, and the arrow keys do the same. Alone they move the focus; with Shift
they go to the monitor on that side or, with none there, to the previous or
next workspace; with Ctrl the focused window or column is taken along. The
`DIRECTION` macro in `config.h` binds all four meanings of a key at once. The
other bindings follow niri's defaults. A new binding gets a `press` line and
an expectation in `tests/smoke.sh`.

## Rules

**The owner's standing instruction: ultimate memory saving, ultimate
performance, ultimate battery life.** Every feature is judged on those three
before anything else. Concretely: a feature that is not in use costs nothing
(no wakeups, no allocation that grows, no helper process), and a feature in
use says what it costs. Prefer the design that does less. Measure with the
budget test, not by guessing.

**wlroots is pinned to 0.20.x and its API changes every release.** Do not write
wlroots calls from memory. The exact source is in `subprojects/wlroots`: read
`include/wlr/` for signatures and `tinywl/tinywl.c` for usage. Bumping the pin
is its own change.

**Every `wl_signal_add` needs a `wl_list_remove` before the listener's memory
is freed or the signal's owner is destroyed.** wlroots asserts on leftover
listeners, and a listener that outlives its struct is a use-after-free. When
one object's listeners live inside another object's struct (the decoration
listeners in `tw_window`), detach them in both destroy paths. Startup failures
after listeners are attached must go through the teardown in `main()`.

**Idle means zero wakeups.** No polling, no periodic work, no helper threads.
Anything time-based must be driven by output frame events, or be a one-shot
timer that is not re-armed per input event (see `idle.c`: input records a
timestamp, the one timer sorts out blanking and suspending when it fires),
and must stop when it has nothing left to do. Waiting for something that may
never happen has to be free: the wake-up watch in `idle.c` is a timer that
never expires and that the kernel cancels when the clock jumps. The budget
test enforces 0.

**Do not send what has not changed.** A redundant configure wakes a client and
a redundant scene update causes a repaint. Cache the last value sent and
compare, as `window_set_size()` and `window_update_clip()` do.

**Off-screen windows cost nothing.** Windows nobody can see have their scene
node disabled (no frame callbacks) and are sent the xdg `suspended` state.
Keep both when changing the layout code.

**A locked session stays locked.** While `server->locked`, no binding runs
(only VT switching), only lock surfaces get input, and windows stay hidden. If
the lock client dies the session remains locked until a new one unlocks it.
Any new input path or binding mechanism must respect this, with a test. tw
never locks by itself unless the locker is installed (`program_find()`): a
lock nobody can open is worse than none.

**Stay out of the pixel business.** tw draws nothing around windows: no
borders (the owner uses none), blur, shadows, rounded corners or text. The
latter need custom shaders or a font stack, and they defeat direct scanout.

**Keep everything else out of process.** X11 apps go through
xwayland-satellite, bars and launchers are layer-shell clients, configuration
is `config.h`. No in-process Xwayland, scripting runtime or config parser.
A helper is started when it is first needed, never at startup, and tw checks
that it is installed (`program_find()`) instead of running it to find out.

**A behaviour change comes with a test.** Layout maths goes in
`tests/layout_test.c`; anything a client can observe goes in `tests/smoke.sh`,
extending `tests/client.c` if needed. Run the dev-build tests before calling
something done: a sanitizer report or a leak fails the smoke test. In the
tests tw's PATH is a private, empty directory, so it never starts a program
installed on the machine; a test that needs a locker or an X11 helper puts a
stand-in there with `fake_program` (see `tests/lib.sh`).

**Budgets ratchet down.** They are the three numbers at the bottom of
`meson.build`. Raising one needs a stated reason.

## Not covered by tests

The headless backend has no touchpad and no way to inject gestures, so the
gesture handlers in `input.c` and the swipe functions in `window.c`
(`strip_swipe_*`, `workspace_snap()`) are untested glue. Keep them thin, and
put anything with logic in `layout.c` where it can be unit tested.

It has no lid switch either, so the switch handler in `input.c` is untested;
what it calls (`lock_now()`) is tested through locking on idle.

Its outputs cannot be unplugged, so `handle_destroy()` in `output.c` (the
remaining output taking over the active workspace) is untested. Nor can the
machine be suspended or the clock set from a test, so the wake-up watch in
`idle.c` is untested, and the suspend command is a stand-in.

`tests/gpu.sh` runs the GPU choice in `data/tw-session` and all of `tw-gpu`
against a made-up machine, so it cannot show that any of it works on a real
GPU. Seen to work on the owner's laptop (2026-11-14, RTX 3070 Ti, driver
620.70): the polkit rule; loading the driver (2.4 s) and unloading it
(0.5 s); the GPU powered down afterwards (`power_state` D3cold, also for the
port above it); Vulkan on Wayland, and OpenGL and Vulkan on X11, drawing
with the NVIDIA GPU into a headless tw that draws with the Intel one;
`tw-gpu launch` with a real desktop entry; the switch-off after the
program's process group was hung up on; fuzzel starting a program through
`tw-gpu launch` in the real session (the program's environment has
`DESKTOP_ENTRY_ID`). Real runs found four faults the made-up machine had
not, all now in the test; the third was the switch-off at the first real
login, which came two seconds after tw started and three seconds before the
login screen let go of the GPU. `tw-gpu login` then worked at a real login:
refused once, and the driver was unloaded on the second try, eight seconds
after tw started. Not seen yet: a window on the real screen (the probe had
the same renderer but no monitor), a monitor on the NVIDIA GPU, and suspend
with the driver unloaded.

Taking the GPU off the bus came out of the second real login: the owner ran
`fastfetch` and `btop` ten seconds in, and the driver was back two seconds
after it had been unloaded (the sign: `nvidia` and `nvidia_uvm` loaded,
`nvidia_drm` not). Seen on the machine since: the GPU leaves the bus and the
port above it reports D3cold; `nvidia-modprobe`, `nvidia-smi`, `fastfetch`
and `btop` are all refused ("NVRM: No NVIDIA GPU found" in the kernel log)
and leave it off; scanning the port brings it back, driver loaded, in 2.4 s;
a GPU that another program has open is left alone, named in the journal
("in use by ..."), and goes off within seconds of that program exiting.

Scanning the port found a fourth fault, in two of five switch-ons: the
kernel announces the GPU before a driver can bind to it, udev loads the
driver in answer, that load fails ("probe routine was not called"), and the
helper's own load, a moment later, failed with it. The helper now runs
`udevadm settle` first and tries the load up to five times. With that, ten
switch-ons in a row put the program on the NVIDIA GPU, after the slot had
been cold for 2 to 60 seconds; udev's early load failed in two of them and
did no harm. A real login followed: the first two tries were refused ("in
use by kwin_wayland systemd systemd-logind", the login screen), the login
screen stopped five seconds after tw started, and the GPU was off the bus
three seconds later; `fastfetch` and `btop`, run by the owner in those same
seconds, were refused and it stayed off. A login while the GPU is off has
been seen since (logging out and in again from a text console): it stayed
off. Still not seen: suspend while it is off.

A GPU appearing while tw runs with `WLR_DRM_DEVICES` unset (tw started
without `tw-session`) makes wlroots open it (`backend/drm/monitor.c`); that
path has never been run.

`tests/bluetooth.sh` runs `tw-bluetooth` and its helper against a made-up
machine, whose stand-in for `sleep` is also the moment in which systemd
puts the radio's block back and a program lets go of its socket, and whose
stand-in for stopping the service starts it again at once, as the bus
does, if the unit's conditions allow. Breaking the scripts and the line
added to the service 38 ways, one at a time, each made it fail. It cannot
show anything about the real machine. Seen there (2026-11-15): the polkit
rule, since the owner started the unit without `sudo`; the driver and its
four helper modules unloading, which takes the adapter away; and the
adapter's USB device asleep with no driver bound (`power/control` was
still `auto` from the driver, so that was not the udev rule's doing). The
made-up machine had not known about the bus: the first two switch-offs
failed as described above, and the model and the condition came out of
that. With the condition installed, the same day: `off` stopped the
service, the bus asked for it twice within 18 ms and systemd skipped both
("unmet condition check ConditionPathExists=/run/tw-bluetooth"), and the
core was out of the kernel 46 ms after the stop, in the first pass; all of
`off` took 0.13 s, and nothing of Bluetooth was back twelve seconds later,
when it was looked at. `on` took 1.25 s: the core and `btusb` loaded, systemd started
`bluetoothd` for the new adapter by itself 17 ms after the driver had
registered, the radio came up unblocked and powered although systemd had
it down as blocked, the timer was set for ten minutes on, and WirePlumber
registered its 21 endpoints with the new daemon. A WirePlumber started
while Bluetooth is off logs one warning ("BlueZ system service is not
available") and takes the daemon up when it appears.

A boot with the blacklist followed (14:08 that day), looked at six minutes
in: no adapter, no Bluetooth module in the kernel, no `bluetoothd`, and
the adapter's USB device asleep for 390 of its 395 seconds with
`power/control` at `auto`, which this time only the udev rule can have
set, no driver having been bound. Three seconds after the Claude app
started the bus was asked for the Bluetooth service and systemd skipped
it, so Chromium is another caller with auto-start on.

Not seen yet: Bluetooth going off by itself (it was left on to watch for
that, and the owner rebooted seven minutes before the timer was due); a
device connecting, and a connection ending and starting the timer over
(the rule matches `DEVTYPE=link`, a name taken from the kernel module's
strings). The owner has no device paired.
Checked without root: a blacklisted module loads by name and not the way
udev asks (`modprobe -b`), `udevadm verify` accepts the rules,
`systemd-analyze verify` the unit and the timer, and a staged install puts
every file where its reader looks.

Keeping the accessibility bus off is tested as far as the session script
goes: what it exports and what it hands to D-Bus and systemd. At a real
login the portal then left the bus alone, as intended. A Chromium or
Electron program still asks for the bus's address as it starts, which
starts the launcher but not the registry daemon.

The keyring is not tw's business: oo7 is started by the user's systemd as
they log in and unlocked by its PAM module (README step 15). What
`data/tw-session` does for it is set `GNOME_DESKTOP_SESSION_ID`, without
which Chromium and Electron programs use no keyring at all (their
`safeStorage` reports `basic_text`); seen to work at a real login. Until
KDE was removed (2026-11-15) the script also passed the session
environment to the KDE wallet daemon that the login had left waiting.

`tests/x11.sh` uses a stand-in for xwayland-satellite. It proves tw's half of
the hand-over (display, arguments, sockets, restart, cleanup, stopping when
unused), not that the real helper works with tw. Stopping it was seen with
the real one (2026-11-14, xwayland-satellite 0.8.3, Xwayland 24.1, in a
headless tw run with `-x 5`): after `xdpyinfo` both were gone five seconds
later, Xwayland along with the helper, and the next `xdpyinfo` started them
again; `xprop -spy` kept them up, through a window closing too, and they
went after it had left and the next window closed; the same from inside
`bwrap --unshare-net`, whose connection is indeed missing from
`/proc/net/unix`; and `vkcube --wsi xcb` kept them up while its window was
open and they went five seconds after it closed. Then in the real session,
after a reboot: the Claude app started, the helper two seconds later, and 43
seconds after that neither it nor Xwayland was running. The helper has no
chance to tidy up when stopped and leaves its own socket and lock file
(`xwls-1`) in the runtime directory; the next helper takes the same name,
so they do not pile up.

The headless backend's outputs have no modes, so no test changes a refresh
rate: of that path only the choice is tested (`tw_refresh_saves()`, in the
unit test), not the loop over an output's modes in `output_mode()`, nor
`outputs_update_mode()`, nor the mode half of `output_set_power()`. And only
the kernel can announce that a power supply changed, so `handle_change()`
in `power.c` is untested, and with it everything about a low battery but
the decision (`tw_battery_look()`, in the unit test); `tests/power.sh`
covers what tw does with the state it finds as it starts. Seen on the owner's laptop (2026-11-14), in a
headless tw whose supplies were made up (`TW_SYS`) but whose announcements
were real: the battery and the charger each announced a change about
every 35 seconds while charging, tw noticed the made-up charger going offline at
the next announcement (33 s) and coming back (35 s), and ran `set
power-saver` and `set balanced` once each. With the monitor open tw holds
20 kB more of its own. `powerprofilesctl set power-saver` needs no
password there and sets the CPUs' `energy_performance_preference` to
`power` and the platform profile to `quiet`; the daemon behind it was
already running in a session where tw never called it. A second such run
had the made-up battery run down, with stand-ins for `notify-send` and
`systemctl`: tw warned once at 9%, suspended at 5%, did nothing more while
the charge stayed at 5%, suspended again at 4%, did nothing at 3% on the
charger, warned again but did not suspend when the charger went at 3%, and
suspended at 2%. The announcements came 55 to 110 seconds apart that time,
so how soon tw notices varies. In the real session `notify-send` with the
warning's urgency was accepted by fnott. Then at a real login on battery
(2026-11-15): the panel came up at 60 Hz from its first modeset
(`i915_display_info`: 60 Hz, 142270 kHz) and the profile was power-saver.
Not seen yet: the panel changing mode while tw runs (the cable pulled or
plugged in), whether the screen blinks as it does, whether a USB-C charger
shows up as a supply that is online, a real suspend from a low battery,
and a wake-up being told from the clocks.

`tests/notify.sh` runs `tw-notify` against dbus-daemon, with `busctl` as the
program sending notifications. It reads what `tw-notify` says it does (`-d`)
and what `dbus-monitor` sees on the bus, not the screen, so what a
notification looks like is untested, and so is the text layout but for not
crashing. The owner's session bus is dbus-broker, which starts services
through systemd and has not started `tw-notify` yet. Its source says each
start is a transient unit with a name of its own
(`dbus-<launcher>-org.freedesktop.Notifications@<n>.service`), started
without a look at the one before, so leaving and being started again a
moment later do not collide; and that service files are read from
`/usr/local/share` before `/usr/share` with the first to claim a name
winning, so tw's entry beats fnott's and KDE's. Each start writes a line or
two to the user's journal.

Seen on the owner's laptop (2026-11-15). In a headless tw: notifications
from `notify-send` (GLib), `busctl` (systemd) and `dbus-send` (libdbus), so
three other implementations agree with `bus.c`; screenshots at scale 1 and
1.25 with sharp text, wrapping and the urgent border; one picture drawn
per change at 1.25 with the change to `layer.c` and two without. On the
real screen, through a private bus: two notifications at 1.25, in place
and gone after their time, `tw-notify` gone a second later. Measured there
with two notifications showing: 168 kB of its own, 2.6 MB resident, which
is what `btop` shows and is mostly the C library every program shares,
451 kB by PSS. From sending a notification to the answer takes 9 ms when
`tw-notify` has to be started and 5 ms when it runs, most of both being
`busctl`; `tw-notify` uses 1.7 ms of processor time to start, read the
notification and draw it. Breaking the code 18 ways, one at a time, each
made a test fail.

Then installed, at a real login the same day: dbus-broker logged KDE's
entry as the duplicate ("Ignoring duplicate name ... in service file
'/usr/share/dbus-1/services/org.kde.plasma.Notifications.service'"), so
tw's entry in `/usr/local` won, and two minutes later systemd started
`dbus-:1.2-org.freedesktop.Notifications@0.service` for the first
notification. The owner removed fnott and reported "that worked!", with
`btop` at about 630 MB at startup. Not seen yet: a notification from the
Claude app, its window coming forward after a click (its Electron has the
call that reads the token), and a monitor going away while the list is on
it.

## Known noise

With a GPU renderer, LeakSanitizer reports small leaks at exit from inside the
GPU driver (seen with the NVIDIA driver and the libdbus it loads). They are not
ours. The tests use the software renderer, where the report must be empty.

A program that only asks X11 a question starts Xwayland, which tw stops
again half a minute later. Two such programs are known. A shell plugin:
fish's `done` runs `xprop` each time a shell starts when `DISPLAY` is set,
which now means a start and a stop of the X server with every command
(README.md says how to remove it). And the Claude desktop app, which
connects to X11 for a moment as it starts (seen with the plugin gone: the
helper started in the same second as the app's GPU process, and no X11
client was left afterwards). To find one, open a terminal in a
private headless tw run with `-d`, giving it the environment of a terminal
from the real session, and look for "started xwayland-satellite" in its log.
Xwayland still running with no X11 window open means a program without a
window is connected, or one left and no window has closed since.

The memory figure of the budget test moves by 100 to 200 kB with what else is
running on the machine, because shared library pages are divided among the
processes that map them. To tell what a change costs, compare the line
"of which allocated by tw", which does not move, and for the rest build the
previous commit too and run both budget tests back to back. After a reboot
on 2026-11-14 the figure read 2320 to 2484 kB over 17 runs, against a budget
of 2500, and the test failed once in a full run with nothing in tw changed:
when it fails, run it again and look at the allocated figure before looking
for a regression. On the first boot without the login manager it read 2451
to 2536 kB for the commit before the battery mode and 2483 to 2565 with it
(six runs each, back to back; the binary's code had grown by 3.8 kB), over
the budget half the time, while the allocated figure stayed at 808 to 812.
The likely reason is that the login screen's compositor mapped the same
libraries and no longer shares them. A second tw from the same build
running during the test (a probe) shares the binary too, and the figure
then reads about 1960. So that budget went from 2500 to
2650, and the allocated figure got a budget of its own, 850, which is the
one that means something.
With KDE's wallet and NetworkManager no longer running (2026-11-15) it read
2518 to 2613 kB over seven runs, the allocated figure 808 to 812 as ever:
fewer programs share the C library and GLib with tw. Still inside the
budget, with 37 kB to spare at the top.

At a real login tw's private memory is 15 to 20 MB, not the budget test's
0.8. The difference is the GPU driver (Mesa), which the tests' software
renderer never loads: 14 MB the moment the GLES2 renderer exists on Intel, in
two driver instances, because wlroots opens one for the allocator and one for
the renderer. Turning off Mesa's shader cache makes it larger, not smaller.
tw also holds the screen's buffers, three at the panel's size (8 MB each on
the owner's 1920x1080 panel), which show up as shared memory (`Shmem`,
`Unevictable`) and not in any process. A client's window buffers are counted
there too, once, however many processes have them open.

When the owner asks where the machine's memory goes, account for the whole
machine before looking at tw: `sh tools/mem.sh` for the split of the figure
they are looking at, then `/proc/meminfo`, `smaps_rollup` per process,
and `drm-total-*` in `/proc/<pid>/fdinfo/*` for GPU buffers. They have
asked four times why `btop` shows 900 MB after login "if literally nothing
is going on". The split has been measured with nothing but a terminal open
(below) and is what the estimates said, so answer from it: more than 700 MB
is the kernel, and no program is behind it. The first time
(2026-11-14, 2.7 GB in use) tw and everything its session starts came to
under 200 MB; the rest was the one application running and 760 MB of kernel
memory that no counter in `/proc/meminfo` explains. The NVIDIA driver held
376 MB of that although its GPU was asleep: the owner unloaded its modules
and the figure fell to 384 MB (which is why `tw-gpu` exists). With the driver
loaded the GPU was also awake about 40% of the time with nothing using it
(`power/runtime_active_time` of its PCI device). Of what remains, about 100 MB is free pages
parked per CPU (`count:` lines in `/proc/zoneinfo`) and about 100 MB is
vmalloc.

The "used" figure of `btop` and `free` is MemTotal minus MemAvailable, and
on this machine about 290 MB of it is in use by nothing. The kernel leaves
out of MemAvailable its own reserve (the zones' high watermarks and
low-memory reserves, 172 MB; large because transparent huge pages raise
`min_free_kbytes` to 66 MB), 82 MB of page cache, and half of the
reclaimable slab. Say so when the owner quotes that figure. Their reading
right after login, before any application, was about 900 MB (2026-11-14).
Taking the Claude app out of a reading made with it open gives roughly:
290 MB of that reserve, 340 MB of kernel memory outside every counter,
130 MB of slab, kernel stacks and page tables, 45 MB of system daemons,
35 MB of session helpers, 45 MB of tw with its screen buffers, and the
terminal showing the figure. Measured later that day with `tools/mem.sh`,
the Claude app closed and only a terminal open: 893 MB, of which 285 held
back, 302 kernel outside the counters (65 of it parked per CPU), 143 slab,
stacks, page tables and per-CPU data, 57 shared buffers (tw's three and the
terminal's), and 107 in programs (tw 19, the terminal and its shell 12).
With the application open the kernel part outside the counters is about
40 MB larger.

The owner then ran a dump of what only root can read (`/proc/slabinfo`,
`/proc/vmallocinfo`). Of the 340 MB outside every counter, 61 MB is the
table of the swap device (zram's `disksize_store`: 16 bytes per page of a
device as large as the RAM, allocated whether or not anything is swapped),
42 MB is the 215 loaded modules (`xe` and `kvm` among them, 6.5 MB, unused),
12 MB is other vmalloc, and 67 MB is free pages parked per CPU. The last
160 MB are pages that drivers took straight from the page allocator, and
this kernel cannot say whose: it is built without `PAGE_OWNER` and
`MEM_ALLOC_PROFILING`. The slab holds nothing unusual (XFS inodes, dentries
and the page cache's tree nodes lead). So the one thing found that can be
shrunk is the swap device, with a drop-in in
`/etc/systemd/zram-generator.conf.d/`: 8 GB instead of 16 frees 30 MB.

The owner found that too little ("saving 61 mb on that isn't much") and
expects the figure well below 900. What moves it most is the reserve:
`vm.min_free_kbytes` at 16384, which is what the kernel picks by itself when
huge pages are off, takes 105 MB out of what MemAvailable leaves out (288 to
183 MB by the kernel's formula) and should shrink the per-CPU parked pages
with it, since their limit follows the low watermark. Nothing on this
machine writes that setting or the huge-page `enabled` file after boot
(writing `enabled` makes the kernel raise the reserve again). Computed,
never applied: the owner first wanted CachyOS's huge pages left as they are
for performance, and the kernel raises this reserve precisely to keep huge
pages available.

After the measured split the owner asked which kernel settings would lower
the figure, "ideally things that won't negatively effect performance or
battery life", compromises allowed. What was put to them, sized against the
893 MB reading. Settings that only change what is counted, since the memory
is free either way: `vm.min_free_kbytes=16384` (105 MB by the formula,
probably more through the per-CPU parked pages; the cost shows once the
file cache has filled the RAM, when a new program more often gets small
pages first, and `thp_fault_fallback` in `/proc/vmstat`, 0 so far, is where
that would be seen) and `vm.lowmem_reserve_ratio = 1024 1024 32 0 0` (43 MB;
the reserve for devices that need memory below 4 GB, 58 MB by default).
Settings that free memory really in use: the swap device at 8 GB (30 MB) or
4 GB (46 MB); `nvme.io_queue_depth=128` (9 MB: 20 queues of 1023 requests
of 512 bytes are allocated up front); and blacklisting `xe` (5 MB). `kvm`
stays, qemu is installed. Not worth it: `transparent_hugepage=never` (what
the owner wants to keep), `cgroup_disable=memory` (5 to 10 MB estimated, and
the per-service memory accounting goes). The rest of the kernel's share
(slab, per-CPU data, modules in use, the drivers' own pages) has no setting.

The owner tried the first two with `sysctl -w` (they last until a reboot).
The part held back went from 285 to 140 MB, exactly as computed. The pages
parked per CPU followed within a minute, from 65 to 97 MB down to 39 to
54 MB: their lower limit is tied to the low watermark, and the limit in
force drifts down to it while the machine is quiet. Together about 170 MB
off the figure. `thp_fault_fallback` was still 0 just after; it only means
something after days of use. To make any of it permanent: the two sysctls
go in `/etc/sysctl.d/`, the swap size in a zram-generator drop-in; the NVMe
queue depth and the `xe` blacklist have to reach the initramfs, either as
files in `/etc/modprobe.d/` (the `modconf` hook copies them) followed by
`mkinitcpio -P`, or on the kernel command line, which on this machine is
`KERNEL_CMDLINE` in `/etc/default/limine` (Limine with CachyOS's entry
tool). `/etc/sysctl.d` and `/etc/modprobe.d` were empty before. Asked which
to keep, the owner chose "Only these two": the sysctls, as README step 8.
The swap size, the NVMe queue depth and the `xe` blacklist were left. The
file is in place (`/etc/sysctl.d/90-memory.conf`) and `systemd-sysctl
--cat-config` shows no other file setting either value. After a reboot both
values were back by themselves, and the owner read "like 680" at startup.

They then set a target: under 500 at startup, by `btop` in a terminal
("I think we can do it!"). What that reading is made of now, roughly: 140
held back, 280 kernel outside the counters, 143 slab and the like, 57
buffers and 107 programs, the terminal showing it being 45 of the last two.
Sized against it. Free of any cost found, 94 MB: the swap device at 4 GB
(46), the NVMe queue depth (9), no `xe` (5), `vm.lowmem_reserve_ratio` at
4096 (22), another login manager (6, and upower, 3, is then no longer
started by the KDE login screen), no accessibility bus (2). At startup only,
25 MB: the portals and PipeWire not started at login, which needs a keyring
daemon that does not wake the portal; they start with the first application
anyway. Small or uncertain, 20 MB: one GPU driver instance in tw instead of
two, `cgroup_disable=memory`, iwd in place of NetworkManager and
wpa_supplicant, no `systemd-userdbd`. That makes 540 to 590. The last step
to 500 would be the distance between the kernel's watermarks
(`vm.watermark_scale_factor` from 10 to 1, about 65 MB with the per-CPU
pages that follow): the band in which the kernel frees memory in the
background shrinks from 16 to 4 MB, and since the file cache keeps the RAM
full on a machine that has run for a while, a burst of allocations then
stalls the program making it. That is the smoothness the owner wants, so it
was advised against. About 115 MB of driver pages remain that nothing can
attribute. Also found: CachyOS's hardware detection puts the NVIDIA modules
into the boot image (`/etc/mkinitcpio.conf.d/10-chwd.conf`), so the driver
loads at every boot before tw takes the GPU off the bus again.

Of that list the owner took two as "totally free wins": the accessibility
bus (now in `tw-session`) and `xe` (README step 9; `xe` is loaded from the
boot image, before the root file system's driver, so the blacklist needs a
rebuild of it). They asked what the NVMe queue depth is and whether 4 GB of
swap would hurt compiling C++ or Rust and playing games, and were told:
the queue depth is how many requests may be under way per queue, 128 is
far more than a laptop uses and going lower gains under 1 MB; swap only
matters once RAM is full, which on 16 GB is what a large parallel build
does, so 8 GB rather than 4 if they shrink it at all. They then took the
NVMe setting as well: `/etc/modprobe.d/memory.conf` holds both lines
(README step 9). After `limine-mkinitcpio` and a reboot all three were in
effect: no `xe` (208 modules, vmalloc 4.7 MB smaller), queue depth 128, and
the accessibility bus not started at login. The Claude app still starts the
bus's launcher three seconds after it starts itself (one process where
there were three). The owner saw no change: "still at like 680 on boot".
Together these are about 16 MB, and the reading swings by more than that
from one boot to the next, because the pages parked per CPU were anywhere
from 55 to 97 MB in the first minutes. `btop` cannot show steps of this
size; compare `tools/mem.sh` readings line by line instead.

This system runs from the external USB SSD (`sda`, a Samsung T7: `/boot` on
`sda1`, an XFS root on `sda2`). The internal NVMe disk holds another
install and is not mounted. A lead on the kernel pages nothing could
attribute: XFS keeps its metadata buffers outside the page cache, in pages
no counter in `/proc/meminfo` shows, and `/sys/fs/xfs/sda2/stats/stats`
says about 7,000 were created by three minutes after boot (the `buf`
line), a good part of them 16 kB inode clusters. That could be most of
those pages, and it would be cache the kernel gives back when asked, not
drivers. Put to the owner as a test: `sysctl -w vm.drop_caches=2` and a
reading before and after.

Userspace outside the one application was 122 MB of private memory at that
reading (`RssAnon` of every process): tw 19; an Xwayland nobody was
connected to 19, plus 6 MB of buffers (the Claude app asks X11 a question
as it starts; `ss -x` showed no socket on the display; tw stops it since);
PipeWire and
WirePlumber 13; the portals 11; systemd, D-Bus, udev, polkit and the
journal 23; CachyOS's `ananicy-cpp` 7; the login manager, which stays
loaded all session, 6; NetworkManager and wpa_supplicant 6; the keyring 5;
upower and power-profiles-daemon 4; `systemd-userdbd` 2; the accessibility
bus 2; fnott and wl-clip-persist 2; bolt, switcheroo-control, avahi and
bluetoothd under 1 each. None of the daemons wakes more than once a second.
tw itself woke 610 times a second during that reading and had used 149 s
of CPU in 45 minutes: the application was animating, and tw draws at the
panel's 300 Hz. The panel also has a 60 Hz mode. With Xwayland stopped by
tw and `ananicy-cpp` and `switcheroo-control` disabled by the owner, the
same sum read 95 MB at the next login.

Two ideas for tw's own share were tried in a headless tw drawing with the
Intel GPU, and dropped. Keeping huge pages out of tw (`PR_SET_THP_DISABLE`):
the real tw has 8 MB of its 13 MB heap in huge pages, but that heap is
memory tw and Mesa really touch (in the probe 8.5 MB resident of 9.4 MB of
address space, with no huge pages at all), so at most 1 MB would come back.
A `malloc_trim()` after startup gave back 170 kB.

The session helpers (the portals, PipeWire, the accessibility bus) all
start at login although tw starts none of them: the KDE wallet daemon
registers with the portal as it starts, and the portal starts the rest.

The owner reads memory per program off `btop`, whose figure is resident
memory: what the program holds for itself plus every library page it has
mapped, most of which other programs share and which is file cache, not
part of the "used" total. Asked about five programs by those figures
(2026-11-14): NetworkManager 27.8 MB there, 3.7 its own (wpa_supplicant
12.7 and 1.8); ksecretd 51.1 and 5.1; plasmalogin and its helper 54.4 and
6.4; wireplumber 36.4 and 8.8; fnott 10.2 and 1.7. All five hold 27.5 MB
for themselves. Library pages only that one program has mapped, which
would leave the cache with it: ksecretd 16.4, wireplumber 8.6, fnott 1.0.
`tools/mem.sh` lists the "for itself" figure, so point them at that.
Alternatives on this machine: iwd is installed and could replace
NetworkManager and wpa_supplicant; gnome-keyring and greetd are not
installed; nothing maintained replaces WirePlumber, which sound and OBS's
screen capture need.

How login and the keyring hang together on this machine, looked up when
the owner asked whether they could still log in to KDE or tw without
plasmalogin (they are wary: "this can get my computer in a broken state if
i can't login"). The login screen runs on tty1 and the session on tty2.
Text consoles come from systemd's getty and do not depend on plasmalogin
(`plasmalogin.service` only conflicts with `getty@tty1`). The wallet daemon
is started by PAM and by nothing else: `/usr/lib/pam.d/plasmalogin` has
`pam_kwallet5.so` with `auto_start` (and the same lines for
`pam_gnome_keyring.so`, so installing gnome-keyring would start both), no
D-Bus service file offers `org.freedesktop.secrets`, and `/etc/pam.d/login`
has no keyring line, so a console login gets no keyring until two lines
are added there. Plasma is installed (`plasma.desktop` runs
`/usr/lib/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland`)
and `plasma-workspace` requires the `kwallet` package, which is where
`ksecretd` comes from. Advice given: replace the login manager in steps
that can each be undone and tested from a second console, and leave the
keyring as it is. Never run on this machine: tw started from a console
login, the wallet unlocked by one, and KDE started from one.

The owner agreed to the first step: two lines appended to
`/etc/pam.d/login`, `-auth optional pam_kwallet5.so` and
`-session optional pam_kwallet5.so auto_start force_run`. `force_run` is
needed because the module skips a session that is not graphical, which a
console login is (its own message says so). Mind that the lock screen
authenticates through the same file (`/etc/pam.d/swaylock` has
`auth include login`), so a mistake there would also lock the owner out at
the lock screen; `sudo` and plasmalogin use other files and stay as a way
back. A console login made while a session is running leaves a second
`ksecretd --pam-login` waiting for an environment nobody sends.

The owner then went further than the test asked (2026-11-14, without a
reboot): logged in on tty3, logged out of the session plasmalogin had
started, logged in on tty2's text console and typed `tw-session`. Seen in
the log and on the machine: the console login ran the wallet module
("final socket path"), tw started as a child of the login shell, logind
shows the session as type wayland and active, `ksecretd` owns
`org.freedesktop.secrets` and the `kdewallet` collection is unlocked, the
Claude app kept its login, and the NVIDIA GPU, off since boot, stayed off
through the new login. So tw from a console login works, keyring
included; KDE from a console has still not been tried. Two things to know
about that state: plasmalogin puts its login screen back up on tty1 when
its session ends, and that screen holds 221 MB for itself (kwin_wayland
100, the greeter 61, its wallpaper 44, its own PipeWire) for as long as
plasmalogin runs; and a console left logged in, as tty3 was, is a shell
anyone at the keyboard reaches past the lock screen.

The owner then ran `sudo systemctl disable --now plasmalogin`: the service
is disabled and stopped, the `display-manager.service` link is gone, no
process of the login screen is left, and the running session was not
touched (still active, wallet unlocked, GPU off). The "used" figure went
from 2366 to 2112 MB within minutes, the login screen's 221 MB and its
buffers. Against a boot where one logs in through plasmalogin the saving
is its two resident processes (6 MB of their own) and upower, which the
login screen started. All of it is README step 10. A boot without
plasmalogin has been seen since: a login on tty1's text prompt, then
`tw-session`, with the wallet daemon running, the NVIDIA GPU off the bus
and no login-screen process. Not seen yet: KDE started from a console.
`tw-session` has to be typed after logging in, and the owner
wants it that way: offered a file in fish's vendor directory
(`/usr/local/share/fish/vendor_conf.d`, which fish reads) that would start
tw on the first console, they answered "I'll type it". Do not add an
autostart for the session.

fnott was taken out of `autostart[]` first: its package ships a D-Bus
service file, so the bus started it when a program sent the first
notification. The owner called that a "hack" that does not fix what they
see ("i don't like fnott. why is it so much memory"), and a day later
asked: "Can we write a lighterweight notification service than fnott???"
Measured then, 54 minutes into a session in which fnott had been started
13 seconds after login (most likely by the Claude app asking what the
service can do): 9.5 MB of its own, 8.9 of it heap, 27.5 MB resident, 24
libraries, fontconfig, FreeType, HarfBuzz and GLib among them. Before it
had shown anything it had been 1.7 MB. `tw-notify` replaces it (see above
for how and "Not covered by tests" for the figures); fnott's package can
be removed (README step 13).

In the same message they asked whether the text console could start bash
instead of fish "so it uses slightly less memory there". The login shell
stays for the whole session as tw's parent: fish, 2.5 MB of its own and
13.2 MB resident. bash would be 0.16 and 4.6. Neither is needed:
`tw-session` ends in `exec`, so typing `exec tw-session` leaves no shell at
all, and a tw that exits or crashes then ends the login instead of
dropping to a shell behind the lock screen. Put to them as that; changing
the login shell would also change the shell their terminals start. Not
tried on the machine.

With `tw-notify` in, the owner named the next two (2026-11-15): "ksecretd"
and "NetworkManager", "I think we can get much cheaper alternatives that
work the same", and added that they no longer mind "dropping KDE desktop
environment entirely". Measured for the answer, with nothing changed.

The keyring. `ksecretd` is 53 MB resident, 8.3 MB of it its own, and
24.7 MB are pages of Qt and KDE libraries that no other process in a tw
session maps, so about 33 MB leave memory with it. It is the largest
helper left. The wallet file is 1.4 kB: its one user is the Claude app
(the only process with libsecret mapped), and VS Code and `gh` would use
it if signed in. The disk is not encrypted (XFS straight on the USB SSD),
so the wallet is what keeps that token from whoever finds the disk; other
sign-ins are on the same disk without it (`~/.claude/.credentials.json`,
`~/.config/gh/hosts.yml`, an SSH key). In the repositories: gnome-keyring
51.1 (needs `gcr` in addition to what is installed; its PAM lines for a
console login are the documented ones) and oo7 0.7.1 (Rust, needs
nothing, ships a PAM module). Neither could be measured without being
installed, and the owner was told so. A keyring of our own was not
offered: the service holds a key derived from the login password for the
whole session by design, so it cannot leave when idle the way `tw-notify`
does, and it is cryptography for credentials. Running with no keyring at
all costs nothing and stores the app's token under a fixed key; whether
the Claude app then stays signed in was never tried. Whichever replaces
it, the app needs one fresh sign-in, the two `pam_kwallet5` lines in
`/etc/pam.d/login` change, and the keyring part of `tw-session` (which
runs `pam_kwallet_init` when the wallet is waiting) has to learn the new
one.

The network. NetworkManager is 28.6 MB resident and 3.8 MB its own,
`wpa_supplicant`, which it starts, 13.0 and 1.9; `systemd-resolved` (8.9
and 1.0) stays either way. NetworkManager also fetches
`http://ping.archlinux.org/nm-check.txt` every five minutes to see whether
the internet is reachable (`/usr/lib/NetworkManager/conf.d/20-connectivity.conf`).
iwd 3.12 is installed and not enabled; with `EnableNetworkConfiguration=true`
under `[General]` in `/etc/iwd/main.conf` (the directory does not exist
yet) it gets addresses by itself and hands DNS to systemd-resolved, its
default. What the machine has: two saved Wi-Fi networks, both with a
password (WPA2/WPA3, nothing enterprise), one wired profile with no cable
in, no VPN profile although the OpenVPN plugin is installed, ModemManager
installed and not running, and only `networkmanager-qt` requires
NetworkManager. iwd does nothing for a wired port: that would be
systemd-networkd with one `.network` file. Not measured: iwd itself.

The owner agreed to iwd on one condition: "want to be sure I don't lose
wifi access before we commit to that". `tools/iwd.sh try` is the answer
(README step 14): it refuses unless the internet is reachable to begin
with, copies NetworkManager's Wi-Fi networks to `/var/lib/iwd` (the
passwords go from one root-only file to another and are never printed),
writes `/etc/iwd/main.conf` if there is none, stops NetworkManager and
`wpa_supplicant`, starts iwd, and waits a minute for a default route and a
fetch by name of the page NetworkManager itself checks, after flushing the
resolver's cache so that the fetch shows iwd has set the DNS servers. On
failure, or on a signal, it goes back. Enablement is not touched until
`keep`, so a reboot is always a way back. One trap is built into the way
back: iwd deletes the kernel's Wi-Fi interface and makes its own with
nl80211's socket-owner attribute (`manager.c` in iwd's source), so the
interface is gone the moment iwd stops and NetworkManager would find no
Wi-Fi device. `restore_interfaces()` gives each card without one an
interface again (`iw phy phy0 interface add wlan0 type managed`).
`/etc/resolv.conf` is the file NetworkManager wrote, pointing at
systemd-resolved's stub, and is left as it is. Checked on the machine
without root: both saved networks are in
`/etc/NetworkManager/system-connections/*.nmconnection` with their
password in the file (`psk-flags` 0), one `sae` and one `wpa-psk`, neither
hidden, both on DHCP. Breaking the script 16 ways, one at a time, each
made `tests/iwd.sh` fail.

The owner ran `try` and then `keep` the same day ("iwd worked"). Seen
afterwards: iwd running and enabled, NetworkManager and `wpa_supplicant`
gone, connected seven seconds after it started (the WPA3 network), an
address, DNS set in systemd-resolved, power save on, and the wired chip
still suspended with its port down. iwd is 14.8 MB resident and 0.4 MB its
own, where the two before it were 41.6 and 5.7. Not seen: the way back
(`back`, and with it the interface being given back to the card), a boot
on iwd, and roaming to the second saved network.

For the keyring they picked oo7 ("that sounds fun"). How it is packaged
(Arch file list and upstream's sources, read 2026-11-15):
the daemon is `/usr/lib/oo7-daemon`, a user unit `oo7-daemon.service` of
`Type=dbus` that nothing enables on install, with no D-Bus service file
for `org.freedesktop.secrets`, so installing it changes nothing while
`ksecretd` runs. `pam_oo7.so` goes in the auth, session (`auto_start`) and
password stacks as `optional`; at login it hands the password to the
daemon over `/run/user/<uid>/oo7-pam.sock`, or with `auto_start` to a
helper (`oo7-daemon-login`) that holds it until the daemon asks.
`oo7-daemon --login` reads the password from stdin. Keyrings are in
`~/.local/share/keyrings/v1/`. Not settled by what was read: whether a
default collection is made on first use, and what happens when a locked
keyring is asked for a secret with no prompter around. Its portal backend
is not used under tw (`tw-portals.conf` names gtk). For Dolphin they want
"some alternative like thunar": Thunar 4.20 needs exo, libxfce4util and
libxfce4ui in addition; gvfs is not installed, so it would have no trash
and mount no drives by itself.

With oo7 0.7.1 installed, tried on a private bus with a made-up password
and keyring directory (`XDG_DATA_HOME`, `OO7_PAM_SOCKET` pointed away from
the real ones). Started with `--login` it makes a keyring called Login,
open and the default, and `secret-tool` stores and reads through it, so
libsecret programs work unprompted. It is 8.4 MB resident, 1.2 MB its own
and 4.7 MB of its own binary's pages, in 22 threads that do not wake when
idle. Started again with the right password the keyring is open; with a
wrong one or none it is locked, and a lookup or a store fails at once (it
looks for a prompter on the bus, `org.freedesktop.secrets.CliPrompter`, and
finds none) and nothing hangs. The hand-over the login module uses works:
`oo7-daemon-login`, given the password on stdin, waits on
`/run/user/<uid>/oo7-daemon-login.sock` (mode 600), the daemon fetches it as
it starts, the keyring is open, and helper and socket are gone. The helper
waits for as long as nobody fetches (it sat for two minutes when started
by mistake). The daemon's strings also speak of taking a KDE wallet over
("KWallet migration requires a secret"); whether that brings the Claude
app's entry across was not tried, since the real wallet opens only with the
owner's password.

`tools/keyring.sh` does the switch (README step 15). `oo7`, as root,
rewrites `/etc/pam.d/login` (the wallet's two lines out, three for oo7 in:
`-auth optional`, `-session optional ... auto_start`, `-password optional`),
keeps the old file as `login.before-oo7`, checks that nothing else changed
before moving the new file into place, and enables the unit for every user
(`systemctl --global`). The file is also what swaylock authenticates
through, and a login module that crashed would shut the owner out of
console and lock screen alike, with no login manager left as a way round.
So the script tells them to log in on a second console at once, with the
session open, and `back` (then called `kwallet`) restores the file. That login also leaves the
helper waiting with the password, which `start` (no root) then uses: it
keeps a copy of `~/.local/share/kwalletd`, stops `ksecretd`, starts the
unit, and reports whether the keyring is open and whether a secret can be
stored and read back. `tw-session` needs no change: its wallet step runs
only when the wallet's login module has set `PAM_KWALLET5_LOGIN`. Applied
to a copy of the real login file the change is exactly those lines, and
going back gives the file byte for byte. Breaking the script 16 ways, one
at a time, each made `tests/keyring.sh` fail.

The owner ran it the same day. `oo7` left the real login file with exactly
those three lines and the unit enabled. The login on the third console
worked: the journal has `login` starting `oo7-daemon-login` and "LOGIN ON
tty3". `start` 22 seconds later: `ksecretd` gone, the unit started under
systemd, "Secret delivered to daemon", the keyring open, a secret stored
and read back. oo7 there is 8.8 MB resident and 1.3 MB its own (6.2 MB by
PSS, 21 threads). It also took the KDE wallet over by itself:
`~/.local/share/keyrings/v1/` holds `kdewallet.keyring` next to
`login.keyring`, with its eight entries, open. The owner then reported
that locking and unlocking worked. Not seen yet: a boot with oo7, and
whether the Claude app finds its sign-in: they said its restart worked,
but its main process was still the one started before the switch, so it
had not asked the keyring again.

KDE. Nothing of it runs in a tw session but `ksecretd`, so removing it
frees disk and no memory. Worked out once oo7 was in (README step 16), on
a copy of pacman's database with the keepers marked explicit, so the
command given is one pacman has accepted: the `plasma` group, the KDE
applications, the CachyOS KDE settings and themes, NetworkManager, its
OpenVPN plugin and `wpa_supplicant` come to 197 packages and 1121 MB. Kept
by marking them explicit, because they came in as dependencies of KDE and
are of use without it: `bolt` (the Thunderbolt security level is `user`,
so a dock needs it), `vulkan-tools` (README step 6), `mpv` (Haruna's
player, and the only one left), `ttf-hack` and `ttf-fira-sans`. Checked
against the packages behind every running process and everything the
README names: nothing else needed is taken. KDE Connect cannot stay
without `modemmanager` (it needs `modemmanager-qt`), so the default keeps
all three; with them gone too it is 217 packages. `heaptrack`, installed
on purpose, keeps `kio` and through it `kwallet`, so `ksecretd` stays on
disk; nothing starts it. OBS is not installed at present. What changes to
the eye: GTK programs are set to KDE's pointer (`breeze_cursors`), which
goes, while tw itself has shown Adwaita's all along (`XCURSOR_THEME` is
unset and the default theme inherits Adwaita); `breeze-icons` stays,
`heaptrack` needs it; the GTK theme comes from `GTK_THEME` and the user's
own `~/.themes`; `QT_STYLE_OVERRIDE=kvantum` in their fish config names a
style that is not installed.

The owner ran both commands the same day, without the reboot that was
advised first. 197 packages went, as simulated (1393 to 1196 before Thunar
came in). Checked right after: nothing failed in either systemd, every
command tw calls is there, tw, iwd, oo7, PipeWire, the portals, polkit,
bolt and the Claude app still running, the login file still with oo7's
three lines, and `tw-notify`'s the only notification entry left. With
`kwallet-pam` and NetworkManager gone, `tools/keyring.sh back` and
`tools/iwd.sh back` have nothing to go back to on this machine. So the
wallet step came out of `tw-session`, the script's `kwallet` command
became `back`, and README steps 4, 10 and 15 no longer assume KDE. A boot on
iwd and oo7 followed the same day; the owner's word on it: "All of that
seems to have worked." Eight minutes into it iwd was running with the
internet reachable and the keyring was oo7, open.

Thunar, which they chose for Dolphin's place, was measured in a headless
tw on a private bus with a scratch home: 52.9 MB resident and 11.2 MB its
own while a window is open. Closing the window ends it within a second,
also when the bus started it for another program's "show in folder"
(`Thunar --gapplication-service`). What stays is `xfconfd`, its settings
daemon, which the bus starts on first use: 9.0 MB resident, 0.57 MB its
own, no wake-ups, until logout. Without `gvfs` and `tumbler`, which are
not installed, it has no trash, mounts no drives and shows no thumbnails.

Bluetooth, measured when the owner asked why `bluetoothd` runs all the
time (2026-11-15, eight minutes after a boot, the first on iwd and oo7).
The daemon: 6.8 MB resident, 0.58 MB its own, no wake-ups to speak of. The
kernel modules: 1.6 MB (`bluetooth` 1.2, `btusb` and the four vendor
modules under it, `rfcomm` and `bnep`, which the daemon loads). The radio:
blocked (systemd restores the block at every boot) and its USB device
(Intel 8087:0033) asleep for 99% of the time, so there was no power to
save. And inside WirePlumber, whose Bluetooth half loads every audio codec
at login: 2.9 MB resident, of which 2.6 MB are library pages nothing else
maps and 0.28 MB is its own. `tw-bluetooth` takes the first two away. Two
other designs were weighed and dropped. Following the radio's block
(`rfkill unblock` starts the daemon, `block` stops it) needs no root
helper, but leaves the modules loaded and has no clean way to start: a new
adapter is unblocked until systemd blocks it again, so the daemon would
start and stop at every boot. Running WirePlumber's Bluetooth half as a
second instance only while Bluetooth is on (upstream's split mode,
`wireplumber@bluetooth.service`, with `hardware.bluetooth = disabled` for
the main one) would take the 2.9 MB off its line in `btop`, but only
0.28 MB of that is memory in use, a whole second WirePlumber runs while
Bluetooth is on, and whether a headset then plays cannot be tested here.
WirePlumber does not start `bluetoothd` by looking for it (its
`GetManagedObjects` call has auto-start off), though it does by taking its
leave of one that has just stopped, and `bluetoothctl` only waits for it. The user's `bluetooth.target` was inactive although the
adapter was there from boot, so nothing should be hung on it.

Found on the way, and larger than all of Bluetooth: 79 of the 161
libraries WirePlumber has mapped are there because of one ALSA plugin,
`libasound_module_pcm_a52.so` from `alsa-plugins`, which encodes AC-3 for
an S/PDIF output and links FFmpeg's `libavcodec` with everything that
drags in (x265, rav1e, librsvg, libjxl). Together 12.1 MB resident, 7.3 MB
by PSS, 2.6 MB of WirePlumber's own 9 MB. `alsa-plugins` was installed on
purpose by somebody, nothing requires it, removing it takes nothing else
along, ALSA programs reach PipeWire through `pipewire-audio`'s own files,
and no other process has the plugin loaded. Put to the owner as
`sudo pacman -Rns alsa-plugins`, which they ran at once, and restarted
WirePlumber. Read after that: 24.7 MB resident where it was 37.1, 6.0 MB
its own where it was 9.0, 12.7 MB by PSS where it was 20.3, and 82
libraries mapped where there were 161. The built-in sink and source are
there as before. It is README step 7 now.

With Bluetooth and `alsa-plugins` gone the owner looked at what was left
in `btop` and asked: "the rest of the processes seem to be systemd bloat
slop is there anything we can do about that or is my only real option to
just move to void linux". Measured for the answer (2026-11-15, six minutes
after a boot, the Claude app open; resident memory as `btop` shows it,
and `RssAnon` for what a process holds for itself, since the `smaps` of
systemd's processes, the user manager included, cannot be read without
root). Twelve processes are systemd's: 109 MB if `btop`'s lines are added
up, 18 MB held. The difference is one library, `libsystemd-shared`, 6 MB
on disk, which each of them maps and `btop` counts again in every line.
By part: init 15.7 and 4.1, udevd 14.5 and 4.4, the user manager 13.6 and
2.3 with its `(sd-pam)` 5.3 and 1.7, journald 12.2 and 0.9, logind 7.8
and 0.9, resolved 8.7 and 0.9, timesyncd 7.2 and 0.7, and userdbd with
its three workers 25.4 and 2.5. Everything running but the Claude app:
411 MB of lines, 72 MB held, of which tw 19, the portals 11, sound 10,
the login and its fish 3.5, D-Bus 2.9, polkit 2.4, oo7, iwd and
wl-clip-persist 2.3, and under 1 each for bolt, power-profiles-daemon,
avahi and the accessibility bus's launcher.

What was put to them. Of systemd's own, only `systemd-userdbd` goes
without a loss: nothing on the machine uses it (no homed, no JSON user
records, sshd not running), only `sockets.target` wants its socket, and
`nss-systemd` works without it. `systemd-timesyncd` can go at the price of
a clock that drifts; `systemd-resolved` only with `openresolv` and
`NameResolvingService=resolvconf` in iwd, which touches name resolution
and so wants a trial first. The rest is what any systemd machine runs.
Not systemd's and open to the same question: `bolt` (started by udev for
the Thunderbolt controller at every boot; no device has ever been
enrolled), the accessibility bus's launcher (the Claude app asks for it;
masking `at-spi-dbus-bus.service` for the user, or a condition on that
unit as for `bluetooth.service`, would refuse it), power-profiles-daemon
(not enabled: started by tw's own `powerprofilesctl` at login, and it
stays; a root helper writing the two sysfs files would do without it),
avahi (KDE Connect), and the login shell (`exec tw-session`). A move to
Void was sized as an estimate, nothing of it measured: runit in place of
init, the journal and the user manager, with eudev and elogind being the
same code as udevd and logind, would hold 9 to 12 MB less, against
everything in `data/` that is built on systemd units, CachyOS's kernel and
optimised packages, and the Claude app's package.

## Battery

The owner called the memory "good now" (2026-11-14) and asked what tw
should do next for battery life. This is what was measured, what was put
to them, and what has been built since.

Measured that evening on battery, 14 minutes after boot, the screen at 15%
brightness and only the Claude app and a terminal open, by reading the
battery's `power_now` every two seconds next to the i915 interrupt count
in `/proc/interrupts` (two interrupts per frame shown) and the CPU time of
the app and of tw. The battery's figure lags what happens by about eight
seconds.

- While the app shows its working animation tw shows 300 frames a second
  and the machine draws 16.5 W (16.4 to 18.4). The app uses 0.8 of a CPU
  core for it, tw 0.1 (600 wakeups a second, two per frame), the kernel's
  page-flip workers 0.03, and the Intel GPU is out of its sleep state
  (`rc6_residency_ms`) half the time.
- With nothing changing on screen (5 interrupts a second, 0.03 of a core
  busy in the whole machine) the draw settles at 11.4 W.
- In between the draw follows the frame rate: 15.3 to 16 W at 180 to 270
  frames a second. By that line 60 frames a second would be about 12.4 W.
  An estimate: the panel's 60 Hz mode has never been switched on.

So animating at 300 Hz costs 5 W, a third of the draw, and 11.4 W is the
floor still to explain. The battery holds 54 Wh of the 90 it was designed
for (`energy_full`, `energy_full_design`), which makes 3.3 hours animating
and 4.7 quiet; its charge limit (`charge_control_end_threshold`) is 100.

What was found set, without root. The panel has two modes, 300 Hz
(preferred, 711 MHz pixel clock) and 60 Hz (142 MHz), and says it takes 48
to 300 Hz; tw sets the preferred mode once (`output.c`) and has no way to
change it while running. The CPU is on intel_pstate with
`energy_performance_preference` at `balance_performance` and the platform
profile at `balanced`, on battery too: power-profiles-daemon is running
but nothing tells it to switch, which KDE did. The cores spend 88% of the
time in C10 even while the app animates. Kept awake (`power/control` is
`on`): the Realtek Ethernet controller, the internal NVMe disk behind
Intel's VMD although nothing on it is mounted, the keyboard's USB
controller and the USB system disk. Already as they should be: the NVIDIA
GPU off, keyboard backlight off, Bluetooth blocked, Wi-Fi power saving on,
audio power saving after 10 s. Sleep is `s2idle`; the firmware offers
`deep` as well. Swap is zram only, so the machine cannot hibernate.
Nothing warns or acts when the battery runs out: upower, which does that,
was only ever started by the KDE login screen, and the owner has no bar.
They ran it down to 11% during this very measurement without noticing.

Root is needed to see the rest: how the watts split between the CPU and
everything else (RAPL), how deep the whole package sleeps
(`/sys/kernel/debug/pmc_core`), whether the panel does self refresh
(`i915_edp_psr_status`), and whether the PCIe links of the NVMe disk and
the Ethernet controller save power (`lspci -vv`). powertop and turbostat
are not installed. `tools/power-root.sh` reads all of that in one go; the
owner asked for it ("Yes, write it") and runs it with sudo, on battery and
with the screen still, into `build/power-root.txt`. Its root-only parts
had not run when this was written, so expect to adjust a path or a
pattern after the first report.

Found while writing it, without root. CachyOS switches the sound driver's
power saving with the cable (`/usr/lib/udev/rules.d/20-audio-pm.rules`:
`power_save` is 10 on battery and 0 on mains), which is why that setting
read 10 in the first measurement and 0 later. The laptop's own sound chip
(Realtek ALC285) had all the same not been powered down for one
millisecond in 61 minutes, 20 of them on battery (`power_on_acct` and
`power_off_acct` under `/sys/class/sound/card0/hwC0D0`), with no stream
open at the end; the HDMI codecs had slept nearly all that time. A second
rule, in `/etc/udev/rules.d/62-cachy-t7.rules` and owned by no package,
keeps the USB system disk from being suspended, which is right for a root
disk. Awake on mains, besides those listed above: the Thunderbolt
controller, the USB controller, Wi-Fi, the sound controller and the SPI
controller. The NVIDIA GPU's sound function is still on the bus while the
GPU is off, suspended, and the port above both is in D3cold.

Put to the owner in this order: the panel at 60 Hz while on battery, tw
switching on the power cable's kernel event; the power-saver profile on
battery, off the same event; a warning and a suspend when the battery runs
low, off the same events; one root reading of the floor; the worn battery
and a charge limit of 80%; a test of which sleep mode drains less.
Recommended: the first three as one change.

Asked "Build 60 Hz and the power-saver profile on battery first?", the
owner answered "Yes, build it", and those two are in (`power.c`,
`battery_refresh_hz` and the two commands in `config.h`). What they cost
when in use: a socket that wakes tw two to four times a minute to read a
handful of small files, and 20 kB. What they save is still the estimate above until the
owner has been on battery with the new build: `sh tools/power.sh` then
shows the draw next to the frame rate, to set against 16.5 W animating and
11.4 W still. Asked next whether to build the warning and the suspend on a
low battery, the owner again answered "Yes, build it", and that is in too
(10% and 5%; `battery_warn_percent`, `battery_suspend_percent`).

Measured on that build the next morning (2026-11-15), on battery, at 60 Hz
and on the power-saver profile, with `tools/power.sh`. While the Claude app
animates tw shows 60 frames a second and the machine draws 11.5 to 12.3 W
where it drew 16.5; the app uses 0.32 of a core (0.81 before) and tw 0.03,
with 120 wakeups a second (0.11 and 600). So the two changes save about
4.6 W while something animates, a little more than the estimate.

The owner ran `tools/power-root.sh` 113 seconds after that boot. The
machine was not idle in its window (0.29 of a core busy and 800 TLB
shootdowns a second: the Claude app was starting), so what it says about
sleep is a lower bound. This much stands. The battery read 10.1 W, against
11.4 W for a still screen the day before. The CPU package took 2.76 W of
it, the cores 0.29 W and the GPU nothing, so more than 7 W goes to what is
outside the processor: panel and backlight, the USB system disk, memory,
chipset, radios. `psys` read 0.23 W and means nothing on this machine. The
package was in C2 for 32% of the time and in C3 for 52%, and never deeper,
although the cores were in C10 for 96.6%. (The script first printed those
shares as 0.01% and 0.02% and the owner was told so: it took the figures
of `package_cstate_show` for TSC ticks, and they are microseconds.) The
panel does self refresh (PSR1, in self refresh when read), the display
engine reaches DC5 and never DC6, and frame buffer compression is off
because PSR1 is on (an Intel workaround). Link power saving (ASPM): the
Realtek Ethernet controller's link can do L0s and L1 and has neither on,
and the controller itself stays in D0 with no cable plugged in
(`power/control` is `on`, zero ms suspended); the unused NVMe disk's link
has L1 and L1.2 on; the Thunderbolt port has L1. The firmware handed the
kernel control of the PCIe features (`_OSC`), so the kernel may change
those. The laptop's own sound chip does power down on battery (24 s on,
52 s off in that boot), so the reading of the day before was the mains
setting or a stream held open.

`lspci -vv` wakes the devices that sleep. That first report therefore
listed the port of the NVIDIA GPU and the Thunderbolt controller as awake;
both were in D3cold when read without it. The script now notes who is
awake before it lists anything, says whether the machine was idle and who
was busy, and reads the deepest package sleep the firmware allows (MSR
0xE2 through `/dev/cpu/0/msr`, which this kernel has built in).

Asked of the owner next: the report again in a quiet window, and once more
after `echo auto` into the Ethernet controller's `power/control` (it lasts
until a reboot), to see whether the package then sleeps deeper and what
that is worth in watts.

They ran both, eleven minutes later, 95 seconds apart, the machine idle
both times (0.09 and 0.05 of a core busy). The firmware allows the deepest
package sleep (MSR 0xE2 reads 0x74008008: no limit, locked). With the
Ethernet controller awake: 9.74 W at the battery, 2.51 W in the CPU
package, the package in C2 for 33% of the time and C3 for 53%, nothing
deeper. With it suspended, which took its port down with it: 8.29 W at the
battery, 1.13 W in the package, C2 32%, C3 1.5% and C6 59%, still nothing
deeper. So the controller sitting in D0 with its link's power saving off
held the package at C3, and letting it sleep is worth 1.4 W, a seventh of
the idle draw. That is README step 11, a udev rule for the owner to
install; `power/control` was left at `auto` by the test and lasts until
the next boot.

What holds the package at C6 now is most likely the USB controller. It is
the one part of the chipset that asks for a short wake-up time (`ltr_show`:
XHCI 0.41 ms, the next is Wi-Fi with 20 ms), and it can never sleep: the
system disk hangs off it, and so does the keyboard, whose `power/control`
is `on`. pmc_core's `ltr_ignore` could show whether that is it, but making
the platform ignore a request of the controller the root disk is on is not
something to hand the owner, and nothing short of moving the system to the
internal disk would make it stick. What a deeper sleep could still give is
part of the 1.1 W the package draws now. The 7 W outside the processor are
the larger share and have no counter: panel and backlight, the USB disk,
memory, chipset, Wi-Fi. The display does what it can (PSR1 in self refresh
both times, DC5 entered, DC6 never, which follows from the package not
going past C6). The deepest platform state (`slp_s0_residency_usec`) was
never entered while awake, as expected; whether suspend reaches it is not
known, and with it what a night with the lid closed costs.

Where the draw stands on battery after all of this: 8.3 W with a still
screen (11.4 W the day before) and about 12 W while the Claude app
animates (16.5 W; read before the Ethernet controller was put to sleep,
so probably lower now), on a battery that holds 54 Wh.

Offered a measurement of what the laptop loses while suspended, the owner
declined (2026-11-15): "it's not asleep most of the time I use it anyway
most of the time I just have claude running for a long time. if it's
asleep or locked nothing is getting done anyway so I don't care much about
that". So the case to work on is the Claude app working for hours with the
owner at the machine, and sleep modes are off the list. The journal agrees:
no suspend in the last three boots. Nothing holds the idle suspend off
while the app works (it has only a `delay` inhibitor), so a run left alone
for 15 minutes would be suspended; that has not happened.

Read again for that case with the Ethernet controller asleep: 10.0 to
10.6 W while the app animates at 44 to 52 frames a second, and 9 to 9.5 W
in the stretches where it drew 6 a second with Claude still working. So
what the animation itself costs at 60 Hz is about 1 W, and the draw while
Claude works has gone from 16.5 W to about 10.5 W. What is left in
software is that 1 W (telling the app to reduce motion, or tw sending
frame callbacks less often while nothing is typed). Larger, and not tw's:
the battery's wear (54 of 90 Wh) and the system running from the USB disk.

The owner said yes to trying reduced motion. `gsettings set
org.gnome.desktop.interface enable-animations false` took effect in the
running Claude app within two seconds, and switching it back and forth
showed it was the setting: on, 64 to 78 frames a second and 10.8 to
11.8 W while Claude worked; off, 5 to 6 frames a second and 8.8 W, with
0.05 of a core busy in the whole machine. So the working animation was
costing 2 to 3 W, more than the 1 W estimated from the frame rate alone,
and with it gone the draw while Claude works is nearly that of a still
screen. The setting was left off (README step 12). It is the user's own
desktop setting, in dconf, not something tw sets.

They also asked whether the USB controller holding the package at C6 was
known or a guess. A guess, and they were told so. It rests on one reading
(XHCI is the only part asking for a short wake-up time), and the same
report has a figure that may speak against it: `ltr_show` gives the
aggregated request as 800 ms. The other candidates were never ruled out:
some 500 timer interrupts a second with the Claude app open, and the
display. `tools/power-root.sh` now also prints how soon the package must
be awake again from each level (MSRs 0x60A to 0x60C and 0x633 to 0x635),
to set against what the devices ask for. Since the draw while Claude
works is now close to idle, how deep the package sleeps matters for the
owner's case after all.

The owner ran it a third time (idle, 8.10 W at the battery, 1.10 W in the
package; C2 32%, C3 1%, C6 61%, nothing deeper, as in the second report).
The six registers read zero with the valid bit clear: the firmware never
set them, so there is nothing to set against the devices' requests and
the question stays open. What is known: the firmware allows C10; with the
Ethernet controller asleep the package gets to C6 and, in two idle
reports, not for one microsecond deeper, which looks like a condition
that is never met and not like idle periods that are merely short (about
500 wake-ups a second, 440 of them timers, with the Claude app and a code
editor open). What could tell the candidates apart was not done, and the
owner was told why: making the chipset ignore the USB controller's
request (`ltr_ignore`) with the root disk on that controller; letting the
keyboard's USB device suspend, which input devices are commonly excluded
from because some do not wake; and a report with the Claude app closed,
which is safe but describes a state the owner is never in. At stake is
part of the 1.1 W the package draws when idle.

Where it ended (2026-11-15, on battery, screen at 15%): 8.1 W idle and
8.8 W while Claude works, against 11.4 W and 16.5 W the day before. In
order of size: reduced motion and the 60 Hz mode (the animation), the
Ethernet controller (1.4 W), the power-saver profile.
