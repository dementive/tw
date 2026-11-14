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
./build/tw -d -s alacritty

# Install the binary and the login-screen entry (tw, tw-session, tw.desktop).
# --skip-subprojects keeps wlroots' headers and library out of /usr/local.
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

Always pass `--suite tw`; without it Meson also runs the vendored
subprojects' own tests.

`-d` turns on debug logging; tw logs a `map <app_id> <w>x<h>` line per window.
`-b <seconds>` overrides the screen-blanking timeout from `config.h`.

Never run `data/tw-session` from inside a desktop session to try it out: it
publishes its `WAYLAND_DISPLAY` to the user's D-Bus and systemd environment.

## Code map

| File | Role |
| --- | --- |
| `src/config.h` | All configuration, compile-time: gaps, colours, column widths, key bindings |
| `src/layout.c` | Strip maths: column widths, stacking, view scrolling, the scroll spring. Pure functions, no wlroots, unit tested |
| `src/window.c` | xdg-shell windows, columns, floating dialogs, popups, decorations, workspaces, focus, `strip_arrange()`, scroll animation and swipes |
| `src/layer.c` | Layer shell: launchers, notifications, wallpapers, bars |
| `src/idle.c` | Screen blanking after inactivity, idle inhibitors |
| `src/lock.c` | Session lock, for `swaylock` |
| `src/output.c` | Outputs, scene layers, the frame handler |
| `src/input.c` | Keyboards (real and virtual), bindings, pointer, touchpad settings and gestures, seat |
| `src/main.c` | Startup, teardown, `spawn()` |
| `data/` | `tw-session` (session start script) and the login-screen entry |
| `protocols/` | Protocol XML that wayland-protocols does not ship, copied from the pinned wlroots |
| `tests/client.c` | Minimal Wayland client (window, layer surface, locker, ...) that prints what the compositor tells it |
| `tests/keys.c` | Presses key combinations through the virtual-keyboard protocol |
| `tests/smoke.sh` | End-to-end behaviour on the headless backend |
| `tests/columns.sh` | Windows sharing a column, floating dialogs |
| `tests/idle.sh` | Screen blanking and inhibitors, with a two-second timeout |
| `tests/budget.sh` | Idle-wakeup and memory budgets (release build only) |

There are `TW_WORKSPACES` workspaces, each a strip: a list of columns, left to
right. A column (`struct tw_column`) is a stack of windows, top to bottom,
sharing the column's width and splitting its height; it is never empty and is
freed when its last window leaves (`column_take()`). A fullscreen window is
always alone in its column. Dialogs (windows with a parent or a fixed size)
are not in the strip: they sit in the workspace's `floating` list, centred, at
the size their client chose.

Only the active workspace is on screen, on the first output. `strip_arrange()`
decides where the view should be (`view_x`) and places everything; call it
after anything that changes a strip, the focus, a column width, the active
workspace or the output. Nothing else positions windows.

The view glides to `view_x` on a spring. `view_pos` is where it is drawn now;
`strip_animate()` advances it once per output frame and stops asking for
frames when it arrives. A three-finger swipe moves `view_pos` directly and
`strip_swipe_end()` picks the column edge to settle on. A window is drawn
where `view_pos` puts it, but told to draw (not `suspended`) as soon as it is
visible at either `view_pos` or `view_x`.

Each workspace has `focused` (tiled or floating; gets the keyboard) and `tiled`
(the strip's focus, which the view scrolls to). They differ only while a
dialog is focused.

Focus has two parts, both derived by `focus_update()`: the active window (the
focused window of the active workspace, drawn with the focused border), and
the keyboard, which goes to the lock screen if locked, else to a layer surface
that asked for exclusive focus, else to the active window. Change
`workspace->focused`, `server->active`, `server->keyboard_layer` or the lock
state, then call `focus_update()`; do not send focus events anywhere else.

A window is on screen only if its column is in view, its workspace is active,
the outputs are on (`!server->blanked`) and the session is unlocked. In every
other case `strip_arrange()` disables its scene node and suspends it.

Bindings follow niri's defaults. A new binding gets a `press` line and an
expectation in `tests/smoke.sh`.

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
timestamp, the timer sorts it out when it fires), and must stop when it has
nothing left to do. The budget test enforces 0.

**Do not send what has not changed.** A redundant configure wakes a client and
a redundant scene update causes a repaint. Cache the last value sent and
compare, as `window_set_size()` and `window_update_clip()` do.

**Off-screen windows cost nothing.** Windows nobody can see have their scene
node disabled (no frame callbacks) and are sent the xdg `suspended` state.
Keep both when changing the layout code.

**A locked session stays locked.** While `server->locked`, no binding runs
(only VT switching), only lock surfaces get input, and windows stay hidden. If
the lock client dies the session remains locked until a new one unlocks it.
Any new input path or binding mechanism must respect this, with a test.

**Stay out of the pixel business.** Solid-colour borders only; no blur,
shadows, rounded corners or text rendering in the compositor. They need custom
shaders or a font stack, and they defeat direct scanout.

**Keep everything else out of process.** X11 apps go through
xwayland-satellite, bars and launchers are layer-shell clients, configuration
is `config.h`. No in-process Xwayland, scripting runtime or config parser.

**A behaviour change comes with a test.** Layout maths goes in
`tests/layout_test.c`; anything a client can observe goes in `tests/smoke.sh`,
extending `tests/client.c` if needed. Run the dev-build tests before calling
something done: a sanitizer report or a leak fails the smoke test.

**Budgets ratchet down.** They are the two numbers at the bottom of
`meson.build`. Raising one needs a stated reason.

## Not covered by tests

The headless backend has no touchpad and no way to inject gestures, so the
gesture handlers in `input.c` and the swipe functions in `window.c`
(`strip_swipe_*`, `workspace_snap()`) are untested glue. Keep them thin, and
put anything with logic in `layout.c` where it can be unit tested.

## Known noise

With a GPU renderer, LeakSanitizer reports small leaks at exit from inside the
GPU driver (seen with the NVIDIA driver and the libdbus it loads). They are not
ours. The tests use the software renderer, where the report must be empty.
