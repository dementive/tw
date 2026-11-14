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
override the screen-blanking and suspend timeouts from `config.h`.

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
| `src/x11.c` | X11 display sockets; starts xwayland-satellite when the first X11 client connects |
| `src/output.c` | Outputs, which workspace each one shows, scene layers, the frame handler |
| `src/input.c` | Keyboards and pointers (real and virtual), bindings, pointer lock and confinement, touchpad settings and gestures, lid switch, seat |
| `src/main.c` | Startup, autostart, teardown, `spawn()`, `program_find()` |
| `data/tw-session` | Session start script: picks the GPUs, switches an unused NVIDIA GPU off, sets up the keyring |
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
| `tests/x11.sh` | X11 sockets and starting the helper on demand, with a stand-in helper |
| `tests/gpu.sh` | `tw-gpu`, its root helper and the GPU choice in `tw-session`, against a made-up `/sys` and stand-ins for `systemctl` and `modprobe` |
| `tests/budget.sh` | Idle-wakeup and memory budgets (release build only) |
| `tests/lib.sh` | Shared by the shell tests: starts tw headless with a private runtime directory and PATH |

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

**Budgets ratchet down.** They are the two numbers at the bottom of
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
seconds, were refused and it stayed off. Still not seen: a login while the
GPU is off, and suspend while it is off.

A GPU appearing while tw runs with `WLR_DRM_DEVICES` unset (tw started
without `tw-session`) makes wlroots open it (`backend/drm/monitor.c`); that
path has never been run.

The keyring set-up in `data/tw-session` also only runs at a real login. It
does two things, both seen to work at a real login: it passes the
session environment to the KDE wallet daemon that the login screen left
waiting (after which that daemon owns `org.freedesktop.secrets`), and it sets
`GNOME_DESKTOP_SESSION_ID`, without which Chromium and Electron programs use
no keyring at all (their `safeStorage` reports `basic_text`).

`tests/x11.sh` uses a stand-in for xwayland-satellite. It proves tw's half of
the hand-over (display, arguments, sockets, restart, cleanup), not that the
real helper works with tw.

## Known noise

With a GPU renderer, LeakSanitizer reports small leaks at exit from inside the
GPU driver (seen with the NVIDIA driver and the libdbus it loads). They are not
ours. The tests use the software renderer, where the report must be empty.

Xwayland running although no X11 window is open means something only asked
X11 a question, which is enough to start it. Two culprits are known. A shell
plugin: fish's `done` runs `xprop` each time a shell starts when `DISPLAY` is
set (README.md says how to remove it). And the Claude desktop app, which
connects to X11 for a moment as it starts (seen with the plugin gone: the
helper started in the same second as the app's GPU process, and no X11
client was left afterwards). To find one, open a terminal in a
private headless tw run with `-d`, giving it the environment of a terminal
from the real session, and look for "started xwayland-satellite" in its log.

The memory figure of the budget test moves by 100 to 200 kB with what else is
running on the machine, because shared library pages are divided among the
processes that map them. To tell what a change costs, compare the line
"of which allocated by tw", which does not move, and for the rest build the
previous commit too and run both budget tests back to back. After a reboot
on 2026-11-14 the figure read 2320 to 2484 kB over 17 runs, against a budget
of 2500, and the test failed once in a full run with nothing in tw changed:
when it fails, run it again and look at the allocated figure before looking
for a regression.

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
machine before looking at tw: `/proc/meminfo`, `smaps_rollup` per process,
and `drm-total-*` in `/proc/<pid>/fdinfo/*` for GPU buffers. The first time
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
terminal showing the figure.

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

The session helpers (the portals, PipeWire, the accessibility bus) all
start at login although tw starts none of them: the KDE wallet daemon
registers with the portal as it starts, and the portal starts the rest.
