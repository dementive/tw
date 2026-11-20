# tw

A scrolling-tiling Wayland compositor in C on wlroots, in the spirit of niri's
layout and dwl's size. Priorities, in order: no work when idle, low memory,
low latency, small code.

This file is what a change has to know and keep. The rest is in `docs/`:

| File | Read it when |
| --- | --- |
| [docs/design.md](docs/design.md) | changing a part: how each one works and why |
| [docs/testing.md](docs/testing.md) | judging what a test proves: coverage, what no test reaches, what has been seen on real hardware and what has not |
| [docs/setup.md](docs/setup.md) | a change needs a new program or a step done by hand: the owner's guide to installing tw on a machine |
| [docs/system.md](docs/system.md) | the owner asks about slimming the system around tw: services, kernel settings, replacing KDE's parts. Not tw; optional |
| [docs/g533zw.md](docs/g533zw.md) | the owner asks about memory, battery or sleep on their laptop: measurements, what was tried, what they decided |
| [docs/g615lm.md](docs/g615lm.md) | the same for the second laptop the SSD boots (ROG Strix G16): its hardware, its GPUs and monitor ports, the G533ZW's settings checked there |

## Build, test, run

If `meson` is not installed, `uvx meson` is a drop-in replacement.

```sh
# Dev build: ASan + UBSan. Use this for all day-to-day work.
meson setup build -Db_sanitize=address,undefined
meson test -C build --suite tw

# Optimized build: static wlroots + LTO. Adds the budget test.
meson setup build-release --buildtype=release -Db_lto=true
meson test -C build-release --suite tw

# The owner's way to try it: a window inside their session (Mod is Alt
# there). You run it headless instead; see "Working with the owner".
./build/tw -d -s foot

# Install (the owner runs this themself, never you).
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

Always pass `--suite tw`; without it Meson also runs the vendored
subprojects' own tests. `--skip-subprojects` keeps wlroots' headers and
library out of `/usr/local`.

`-d` turns on debug logging; tw logs a `map <app_id> <w>x<h>` line per
window and an `output <name>` line per monitor. `-b <seconds>`, `-l <seconds>`
and `-z <seconds>` override the screen-blanking, locking and suspend
timeouts, and `-x <seconds>` how long after the last X11 program the X
server is stopped.

Never run `data/tw-session` from inside a desktop session to try it out: it
publishes its `WAYLAND_DISPLAY` to the user's D-Bus and systemd environment.
The one safe way is `TW_SESSION_PRINT=1 sh data/tw-session`, which prints
what it would do and starts nothing.

## Code map

| File | Role |
| --- | --- |
| `src/config.h` | All configuration, compile-time: layout, input, timeouts, programs, key bindings |
| `src/machines.h` | What depends on the machine: its outputs, and whether tw may suspend it. An entry per machine; tw takes the one named after the board as it starts (`machine_pick()`), else the last |
| `src/tw.h` | The server struct and every function shared between files, grouped by file in the order `main()` starts them, as below |
| `src/main.c` | Startup, options, teardown, `spawn()`, `program_find()` |
| `src/output.c` | Outputs, which workspace each shows, which mode each runs in, scene layers, the frame handler and its cap on battery, a new renderer after a GPU reset |
| `src/layer.c` | Layer shell: launchers, notifications, wallpapers, bars, and their menus |
| `src/window.c` | One window: its xdg-shell client, scene nodes, size and state sent, menus (popups), title-bar mode, focus requests (xdg-activation) |
| `src/strip.c` | Where windows go: workspaces, columns, floating dialogs, fullscreen, focus, `strip_arrange()`, scroll animation and swipes, and the actions bound to keys |
| `src/input.c` | Keyboards and pointers (real and virtual), bindings, pointer lock and confinement, drag and drop, moving windows with Mod and the mouse, touchpad settings and gestures, lid switch, seat |
| `src/clipboard.c` | Keeps what was copied for when its program exits |
| `src/idle.c` | Screen blanking, locking and suspend after inactivity, idle inhibitors, noticing a wake-up, `suspend_now()` |
| `src/lock.c` | Session lock for swaylock; locking by itself (`lock_now()`) and starting the locker |
| `src/power.c` | On battery: learns from udev when the power cable moves, slows the outputs, runs the power-profile command, warns and suspends on a low battery |
| `src/x11.c` | X11 display sockets; starts xwayland-satellite on the first X11 client, stops it when none is left |
| `src/layout.c` | Strip maths: column widths, stacking, scrolling, the spring; which refresh rate on battery; what a falling charge and the lid call for, and which screen is the laptop's own. Pure functions, no wlroots, unit tested |
| `notify/` | `tw-notify`, a second program: `notify.c` (the list, its window, the bus conversation, text through fontconfig and FreeType), `bus.c` (D-Bus messages with no library; pure, unit tested), `config.h` |
| `data/tw-session` | Session start script: picks the GPUs, points programs at the keyring, keeps the accessibility bus off, says what to do when tw cannot start |
| `data/tw-gpu`, `tw-power`, `tw-bluetooth` | The three commands that need root. Each comes with a root helper (`*-root`), a system unit (`*@.service.in`) and a polkit rule (`*.rules`) |
| `data/` (rest) | udev rules, the Bluetooth blacklist, timer and service drop-in, the accessibility drop-in, the logind drop-in for the power key, the Thunderbolt rule, the login-screen entry, the portal configuration, the D-Bus entry that starts `tw-notify`, and `post-install.sh`, which `meson install` runs to have the system read all of it |
| `protocols/` | Protocol XML that wayland-protocols does not ship, copied from the pinned wlroots |
| `tests/` | `lib.sh` starts tw headless with a private runtime directory and PATH; `client.c`, `keys.c` and `clip.c` are the test programs; one `.sh` per area. [docs/testing.md](docs/testing.md) lists them |

How it hangs together, in one paragraph; [docs/design.md](docs/design.md)
has the rest. Each workspace is a strip of columns, each output shows one
workspace, and `strip_arrange()` is the only thing that positions windows:
call it after anything that changes a strip, the focus, a width or an
output. `focus_update()` is the only thing that sends focus events: change
`workspace->focused`, `server->active`, `server->keyboard_layer` or the
lock state, then call it. X11, the NVIDIA GPU, Bluetooth, power profiles
and notifications are all "off until needed" and, apart from `x11.c` and
the commands `power.c` runs, not in tw's C code at all.

## Rules

**The owner's standing instruction: ultimate memory saving, ultimate
performance, ultimate battery life.** Every feature is judged on those three
before anything else. Concretely: a feature that is not in use costs nothing
(no wakeups, no allocation that grows, no helper process), and a feature in
use says what it costs. Prefer the design that does less. Measure with the
budget test, not by guessing.

**wlroots is pinned to 0.20.x and its API changes every release.** Do not
write wlroots calls from memory. The exact source is in
`subprojects/wlroots`: read `include/wlr/` for signatures and
`tinywl/tinywl.c` for usage. Bumping the pin is its own change.
libdisplay-info, which wlroots needs, is pinned and built in the same way
(`subprojects/libdisplay-info.wrap`): its library changes name with every
release, and a system update would otherwise leave tw unable to start.

**Every `wl_signal_add` needs a `wl_list_remove` before the listener's
memory is freed or the signal's owner is destroyed.** wlroots asserts on
leftover listeners, and a listener that outlives its struct is a
use-after-free. When one object's listeners live inside another object's
struct (the decoration listeners in `tw_window`), detach them in both
destroy paths. Startup failures after listeners are attached must go through
the teardown in `main()`.

**Idle means zero wakeups.** No polling, no periodic work, no helper
threads. Anything time-based must be driven by output frame events, or be a
one-shot timer that is not re-armed per input event (see `idle.c`: input
records a timestamp, the one timer sorts out blanking and suspending when it
fires), and must stop when it has nothing left to do. Waiting for something
that may never happen has to be free: the wake-up watch in `idle.c` is a
timer that never expires and that the kernel cancels when the clock jumps.
The budget test enforces 0. The one wakeup tw cannot refuse is the kernel's:
on a machine with a battery, batteries and chargers announce themselves
every minute or so with nothing changed, and `power.c` has to look
([docs/design.md](docs/design.md#on-battery)). The tests run without a
battery, so the budget does not see it; keep that handler cheap.

**Do not send what has not changed.** A redundant configure wakes a client
and a redundant scene update causes a repaint. Cache the last value sent and
compare, as `window_place()` and `window_show()` do.

**Off-screen windows cost nothing.** Windows nobody can see have their scene
node disabled (no frame callbacks) and are sent the xdg `suspended` state.
Keep both when changing the layout code.

**A locked session stays locked.** While `server->locked`, no binding runs
(only VT switching), only lock surfaces get input, and windows stay hidden.
If the lock client dies the session remains locked until a new one unlocks
it. Any new input path or binding mechanism must respect this, with a test.
tw never locks by itself unless the locker is installed
(`program_installed()`): a lock nobody can open is worse than none.

**Nothing sends a machine to sleep that has not been seen to wake.**
`suspend_after_seconds` and `battery_suspend_percent` are 0 in the last
entry of `src/machines.h` and stay 0 in a machine's own entry until its
owner has suspended it by hand and seen it come back. The G533ZW did not.

**Stay out of the pixel business.** tw draws nothing around windows: no
borders, blur, shadows, rounded corners or text. They need custom shaders
or a font stack, and they defeat direct scanout.

**Keep everything else out of process.** X11 apps go through
xwayland-satellite, bars and launchers are layer-shell clients,
configuration is `config.h`. No in-process Xwayland, scripting runtime or
config parser. A helper is started when it is first needed, never at
startup, and tw checks that it is installed instead of running it to find
out. The one job taken in is keeping the clipboard (`clipboard.c`): a helper
for it cannot be started when needed, so it ran all session.

**A behaviour change comes with a test.** Layout maths goes in
`tests/layout_test.c`; anything a client can observe goes in
`tests/smoke.sh`, extending `tests/client.c` if needed; a new binding gets a
`press` line and an expectation there. Run the dev-build tests before
calling something done: a sanitizer report or a leak fails the smoke test.
In the tests tw's PATH is a private, empty directory, so it never starts a
program installed on the machine; a test that needs a locker or an X11
helper puts a stand-in there with `fake_program` (see `tests/lib.sh`).

**Budgets ratchet down.** They are the three numbers at the bottom of
`meson.build`. Raising one needs a stated reason. The memory figure swings
with what else runs on the machine and the allocated one does not: when the
budget test fails, run it again and compare "of which allocated by tw"
before looking for a regression ([docs/testing.md](docs/testing.md#the-budgets)).

**Installed paths do not change in passing.** `meson install` does not
remove what an earlier version put there, so a renamed unit, rule or
drop-in leaves its old self behind on every machine that has tw. Renaming
one is its own change, with the cleanup said in `docs/setup.md`.

**The docs are part of a change.** A new program or manual step goes in
`docs/setup.md`, and one outside tw that only makes the system smaller in
`docs/system.md`. How a part works goes in `docs/design.md`, not here.
Something seen on a real machine for the first time, or found to be
untestable, goes in `docs/testing.md`. A measurement or a decision about
one of the owner's laptops goes in that laptop's file, `docs/g533zw.md` or
`docs/g615lm.md`. Keep this file to what every change needs.

## Working with the owner

They are the only user, on CachyOS, and move tw between their own
machines. What they have decided, so that it is not put to them again:

- No bar, no window borders or other focus markers, no night light, no
  tray, no floating-window controls. Configuration stays compile-time.
- No notification daemon but `tw-notify`; no clipboard helper; no
  autostart of the session from the shell (they type `tw-session`).
- The screen goes off after 30 s without input and locks nothing; a
  notification turns it back on (Claude Code sends one when it is done);
  the session locks after 15 minutes. On battery, frames are capped at 2 a
  second after 5 s without input, and never for videos, games or anything
  fullscreen.
- Fixes at the root: a workaround that shadows or papers over the cause was
  rejected as "hacky".
- Ordinary means over clever ones: `tw-notify` loads its font through
  fontconfig like any program; a font drawn into the binary at build time
  was taken out. Measuring and migration scripts are not kept in the tree
  once their job is done.
- On their laptop: sleep after 30 idle minutes (not while Claude Code
  works) and on lid close. Huge pages stay on, and the list of
  kernel settings they took and declined is closed. See
  [docs/g533zw.md](docs/g533zw.md#decisions).

When they ask where the machine's memory goes, account for the whole
machine before looking at tw: [docs/g533zw.md](docs/g533zw.md#memory) has
how to split `/proc/meminfo`, how `btop`'s figures mislead, and everything
already tried. Most of what `btop` calls "used" at startup is the kernel,
and no program is behind it.

Never suspend the machine, run `sudo`, `pacman` or `meson install`, or type
their password: those steps are theirs. Give the exact command instead, and
say when a reinstall or a new package is needed. To run tw by hand, use the
headless backend in a private `XDG_RUNTIME_DIR` under `build/`, with `-z 0
-b 0`, and wrap anything that talks to D-Bus in `dbus-run-session`.
