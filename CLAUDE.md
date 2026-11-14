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
```

Always pass `--suite tw`; without it Meson also runs the vendored
subprojects' own tests.

`-d` turns on debug logging; tw logs a `map <app_id> <w>x<h>` line per window.

## Code map

| File | Role |
| --- | --- |
| `src/config.h` | All configuration, compile-time: gaps, colours, column widths, key bindings |
| `src/layout.c` | Scrolling-strip maths. Pure functions, no wlroots, unit tested |
| `src/window.c` | xdg-shell windows, popups, decorations, focus, `strip_arrange()` |
| `src/output.c` | Outputs and the frame handler |
| `src/input.c` | Keyboard, bindings, pointer, seat |
| `src/main.c` | Startup, teardown, `spawn()` |
| `tests/client.c` | Minimal Wayland client that prints the state the compositor gives it |
| `tests/smoke.sh` | End-to-end behaviour on the headless backend |
| `tests/budget.sh` | Idle-wakeup and memory budgets (release build only) |

The strip is one list of windows, left to right, one window per column, shown
on the first output. `strip_arrange()` is the only place that positions
windows; call it after anything that changes the list, the focus, a column
width or the output.

## Rules

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

**Idle means zero wakeups.** No timers, no polling, no periodic work, no
helper threads. Anything time-based must be driven by output frame events and
must stop when it has nothing left to do. The budget test enforces 0.

**Do not send what has not changed.** A redundant configure wakes a client and
a redundant scene update causes a repaint. Cache the last value sent and
compare, as `window_set_size()` and `window_update_clip()` do.

**Off-screen windows cost nothing.** Columns outside the output have their
scene node disabled (no frame callbacks) and are sent the xdg `suspended`
state. Keep both when changing the layout code.

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

## Known noise

With a GPU renderer, LeakSanitizer reports small leaks at exit from inside the
GPU driver (seen with the NVIDIA driver and the libdbus it loads). They are not
ours. The tests use the software renderer, where the report must be empty.
