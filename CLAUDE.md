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
| `src/layout.c` | Strip maths: column widths, stacking, view scrolling, the scroll spring. Pure functions, no wlroots, unit tested |
| `src/window.c` | xdg-shell windows, columns, floating dialogs, popups, decorations, workspaces, focus, focus requests, `strip_arrange()`, scroll animation and swipes |
| `src/layer.c` | Layer shell: launchers, notifications, wallpapers, bars |
| `src/idle.c` | Screen blanking and suspend after inactivity, idle inhibitors, noticing a wake-up |
| `src/lock.c` | Session lock for `swaylock`; locking by itself (`lock_now()`) and starting the locker |
| `src/x11.c` | X11 display sockets; starts xwayland-satellite when the first X11 client connects, and stops it when none is left |
| `src/power.c` | Running on battery: learns from udev when the power cable goes in or out, slows the outputs down and runs the power-profile command |
| `src/output.c` | Outputs, which workspace each one shows, which mode each one runs in, scene layers, the frame handler |
| `src/input.c` | Keyboards and pointers (real and virtual), bindings, pointer lock and confinement, touchpad settings and gestures, lid switch, seat |
| `src/main.c` | Startup, autostart, teardown, `spawn()`, `program_find()` |
| `data/tw-session` | Session start script: picks the GPUs, switches an unused NVIDIA GPU off, sets up the keyring, keeps the accessibility bus off |
| `data/tw-gpu` | Switches the NVIDIA GPU on for the programs that want it and off after them |
| `data/tw-gpu-root`, `tw-gpu@.service.in`, `tw-gpu.rules`, `tw-gpu-udev.rules` | What lets `tw-gpu` load and unload the driver: root helper, system unit, polkit rule, udev rule |
| `data/` (rest) | The login-screen entry, the portal configuration |
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
| `tests/gpu.sh` | `tw-gpu`, its root helper, and `tw-session` (the GPU choice, what it passes on to services), against a made-up `/sys` and stand-ins for `systemctl` and `modprobe` |
| `tests/budget.sh` | Idle-wakeup and memory budgets (release build only) |
| `tests/lib.sh` | Shared by the shell tests: starts tw headless with a private runtime directory and PATH, and no power supplies |
| `tools/power.sh` | Battery draw every two seconds next to frames shown and CPU in use. For telling what an animation or a refresh rate costs |
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
every 35 seconds or so with nothing changed. A machine without a battery opens no
monitor and runs no command.

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

Keeping the accessibility bus off is tested as far as the session script
goes: what it exports and what it hands to D-Bus and systemd. At a real
login the portal then left the bus alone, as intended. A Chromium or
Electron program still asks for the bus's address as it starts, which
starts the launcher but not the registry daemon.

The keyring set-up in `data/tw-session` also only runs at a real login. It
does two things, both seen to work at a real login: it passes the
session environment to the KDE wallet daemon that the login screen left
waiting (after which that daemon owns `org.freedesktop.secrets`), and it sets
`GNOME_DESKTOP_SESSION_ID`, without which Chromium and Electron programs use
no keyring at all (their `safeStorage` reports `basic_text`).

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
in `power.c` is untested; `tests/power.sh` covers what tw does with the
state it finds as it starts. Seen on the owner's laptop (2026-11-14), in a
headless tw whose supplies were made up (`TW_SYS`) but whose announcements
were real: the battery and the charger each announced a change about
every 35 seconds while charging, tw noticed the made-up charger going offline at
the next announcement (33 s) and coming back (35 s), and ran `set
power-saver` and `set balanced` once each. With the monitor open tw holds
20 kB more of its own. `powerprofilesctl set power-saver` needs no
password there and sets the CPUs' `energy_performance_preference` to
`power` and the platform profile to `quiet`; the daemon behind it was
already running in a session where tw never called it. Not seen yet: the
panel changing mode (the first time the cable is pulled after an install),
whether the screen blinks as it does, what the machine draws at 60 Hz, and
whether a USB-C charger shows up as a supply that is online.

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
libraries and no longer shares them. So that budget went from 2500 to
2650, and the allocated figure got a budget of its own, 850, which is the
one that means something.

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

fnott is no longer in `autostart[]`: its package ships a D-Bus service
file, so the bus starts it when a program sends the first notification
(tried on a private bus with a headless tw: started by `notify-send`,
drew on the output, and served the second notification too). KDE's
`org.kde.plasma.Notifications.service` claims the same name and loses:
dbus-broker logs it as a duplicate at every login. The owner called this a
"hack" that does not fix what they see ("i don't like fnott. why is it so
much memory"), which is fair as far as it goes: once a notification has
come, the daemon stays, with the 1.7 MB.

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
are not installed.

Put to the owner in this order: the panel at 60 Hz while on battery, tw
switching on the power cable's kernel event; the power-saver profile on
battery, off the same event; a warning and a suspend when the battery runs
low, off the same events; one root reading of the floor; the worn battery
and a charge limit of 80%; a test of which sleep mode drains less.
Recommended: the first three as one change.

Asked "Build 60 Hz and the power-saver profile on battery first?", the
owner answered "Yes, build it", and those two are in (`power.c`,
`battery_refresh_hz` and the two commands in `config.h`). What they cost
when in use: a socket that wakes tw about four times a minute to read six
small files, and 20 kB. What they save is still the estimate above until the
owner has been on battery with the new build: `sh tools/power.sh` then
shows the draw next to the frame rate, to set against 16.5 W animating and
11.4 W still. The warning and suspend on a low battery are not built; they
were left for the owner to decide.
