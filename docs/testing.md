# What is tested, and what has only been seen

Three kinds of evidence stand behind tw, and this file keeps them apart:
what the tests check on every run, what no test can reach, and what has
been seen to work on a real machine (so far one: an ASUS ROG Strix G533ZW,
see [g533zw.md](g533zw.md)). Add to the last part whenever something is
seen for the first time, and take things off the "not seen yet" lists.

## Running the tests

```sh
meson test -C build --suite tw           # dev build: ASan + UBSan
meson test -C build-release --suite tw   # optimized build: adds the budget test
```

Always pass `--suite tw`; without it Meson also runs the vendored
subprojects' own tests. In the dev build a sanitizer report or a leak fails
the test that caused it.

The compositor tests start tw on the headless backend with the software
renderer, in a private runtime directory, with a PATH that holds nothing but
the test's own stand-ins (`fake_program` in `tests/lib.sh`) and with no
power supplies. So they need no GPU and no seat, start no program installed
on the machine, and cannot touch the session they are run from. The script
tests run each script against a made-up machine: a directory standing in for
`/sys`, and stand-ins for `systemctl`, `modprobe` and the like.

| Test | What it covers |
| --- | --- |
| `layout` | `src/layout.c`: column widths, stacking, scrolling, the spring, the refresh-rate choice, the low-battery decision |
| `bus` | `notify/bus.c`: messages written and read back, cut short, damaged; nothing may be read past its end; big-endian ones passed over by their length; what a broken header still says; `Notify` bodies read to the byte by the same `bus_notify_read()` tw-notify uses |
| `smoke` | everything a client can observe: no file tw was handed open reaching a program it starts; sizes, focus, every key binding (the level keys with stand-ins for `wpctl`, `brightnessctl` and `notify-send`), workspaces, layer surfaces (and no configure to one that nothing changed for), fullscreen, the lock, command-line options, a clean exit; no window told a binding's key is held; windows offered fullscreen and nothing else; focus requests with a made-up token or one naming no surface refused |
| `columns` | windows sharing a column, floating dialogs; a dialog that maps again drawn in front; a game that goes fullscreen without the focus leaving it where it was, and getting it when the dialog in front closes; a game of a fixed size going fullscreen in the strip and floating again after, and one that asks to start fullscreen and changes its mind before its first frame; a tiled window's menu drawn above a dialog; a hidden window that unmaps and maps again as a dialog told it is neither suspended nor tiled from its first configure; a dialog's client-drawn shadow not cropped (the pointer reaches it) |
| `pointer` | pointer focus, click to focus, pointer lock, confinement, which waits until the pointer is inside its region; a window opening under a resting pointer gets it, and so does one the strip glides under it, its motion closed by a frame; the window under the pointer loses it while the screens are off and gets it back; a lock screen appearing under a resting pointer gets it, and a click without moving reaches it and no window; the window under the pointer gets it back as the session unlocks, with nothing in between; and an open menu gives the keyboard up to the lock screen; a menu its client moves off the screen is kept on it; drag and drop between two windows, and a drag carried to an empty workspace landing nowhere, the keyboard following the focus once it is over; a button held as the session locks keeps nothing on the window behind the curtain; a held button keeps the pointer on its window past the edge, and the release reaches it; the menu of a bar on the bottom layer drawn over the window below the bar, and getting the pointer there |
| `outputs` | two monitors: a workspace on each, focus and columns between them, also by Mod and Mod+Ctrl past the end of the strip and back, and across an empty strip; Mod and the left button carrying a window to an empty part of the other monitor and beside a column there, nothing moving when let go on itself, the window never hearing that button, a plain drag still reaching it, and nothing moving while locked; a window on one monitor told nothing while workspaces, a bar and fullscreen change on the other; a bar keeping its room on its own monitor only, from the frame it maps, and giving it back as it goes |
| `idle` | screen blanking and inhibitors with a two-second timeout; locking on idle; suspend (a stand-in command); a layer surface's inhibitor holding nothing off behind the lock; an inhibitor letting go putting the next suspend attempt a period away, however many failed before; an abandoned lock with no locker installed: stays locked, the missing locker is not run, and another lock client takes over and unlocks |
| `x11` | the X11 sockets, starting the helper on demand and stopping it when unused, a helper that dies, fails or is uninstalled, taking over the display a killed tw left behind, with a stand-in helper |
| `clipboard` | a copy outlives its program; an emptied clipboard stays empty; a copy marked secret is not kept; long copies, slow and vanishing readers, the size limit, programs that die or do not answer while being read; a window copying with an input serial older than tw's offer |
| `power` | what tw makes of the power supplies it finds as it starts, and the command it runs; which entry of `src/machines.h` it takes, by the board name, and for an unknown board or none |
| `notify` | `tw-notify` on a private session bus and a headless tw: started on demand, expiry, clicks, the cursor set over the list, leaving; another program replacing and closing a notification by its id, as `notify-send -r` does, or by its tag, as the level keys do; an urgent one kept when the list overflows; ids that fit libnotify's signed int; tags too long to keep whole kept apart; notifications closed when tw goes |
| `gpu` | `tw-gpu`, its root helper, and `tw-session`: the GPU choice (by the built-in panel, also when the NVIDIA GPU is the boot VGA device beside an integrated GPU that is not VGA-class; a second GPU that is a 3D controller; no GPU with `boot_vga`, by vendor), `TW_ALL_GPUS=1` bringing back a GPU switched off earlier, what it passes on to services and takes back from them, at the start and once tw has exited (also when a signal ends the session), a tw that cannot start; one switch-off waiting however many programs run beside one that stays; a GPU left on when nouveau drives it, when tw has every GPU open (and `off` saying so; outside a tw session no GPUs named does not count), and after a kernel update; a GPU that does not come back stays known; the wait for a GPU kept open giving up; a failed unit told apart from a GPU in use, and no driver installed from a kernel update; NVIDIA's two daemons not counted as use; the GPU's sound function let sleep; a module the helper has no name for that holds the driver; `on` and `off` in the root helper one at a time (`flock`); desktop entries in subdirectories and by `DESKTOP_ENTRY_PATH` |
| `profile` | `tw-power` and its root helper: the three profiles, machines that lack a setting, the performance governor, and kept where a CPU has no preference to set, nothing written twice, refusals, two at once waiting for each other (`flock`) |
| `bluetooth` | `tw-bluetooth` and its root helper: off at boot, on, off by itself and by hand, a program that still has Bluetooth open, the timer running out during an `on`, every way switching on can fail, staying on after a kernel update, an adapter whose driver is not btusb coming back on, a driver the file names for another machine not keeping Bluetooth on, the helper one at a time (`flock`). Of the udev rules only that udev accepts them, not that the sleep rule finds an adapter |
| `thunderbolt` | that the Thunderbolt rule is still the kernel guide's and udev accepts it |
| `install` | `data/post-install.sh`: what it reloads after a real install, that a staged or rootless one reloads nothing, and that it removes what an option now off left, and nothing outside the prefix |
| `budget` | release build only: idle wakeups and memory, see below |

The clipboard, `tw-notify`, `tw-power`, `tw-bluetooth`, the
accessibility part of `tw-session` and the Thunderbolt rule were each
checked by breaking the code one way at a time and seeing a test fail
every time; so were the newer focus, lock, inhibitor, menu and fullscreen
tests.

A check that a test repeats must be a function or a command that reads
anew each time: `wait_until "..." test "$(count ...)" -gt 1` reads once,
before `wait_until` starts, and passes or times out on that one reading.

## The budgets

The three numbers at the bottom of `meson.build`, checked on the release
build with three clients mapped and X11 support waiting for its first
client:

| Budget | Now | Measured |
| --- | --- | --- |
| idle wakeups in 3 s | 0 | 0 |
| own memory (PSS minus pixel buffers) | 2800 kB | 2445 to 2713 on the G533ZW; 3191 to 3440 on the G615LM (2026-11-19): the machine's doing, since the commits before and after that day's changes read the same back to back |
| of which allocated by tw | 850 kB | 800 to 812 |

They ratchet down; raising one needs a stated reason.

**The memory figure swings; the allocated one does not.** PSS divides each
shared library page among the processes that map it, so "own memory" moves
by 100 to 200 kB with what else is running: it rose each time a program
that shared libraries with tw was removed from the machine (a login
manager's compositor, the KDE wallet, NetworkManager), with nothing in tw
changed. A second tw from the same build running during the test shares
the binary too, and the figure then reads about 1960. So this budget says
little on its own.

So when the budget test fails: run it again and look at the line "of which
allocated by tw" before looking for a regression. To tell what a change
costs, compare that line, and for the rest build the previous commit too
and run both budget tests back to back. Keep both builds on the same disk:
a build under `/tmp` (tmpfs) reads about 800 kB lower, because its binary
is shared memory there, which the figure leaves out with the pixel
buffers.

**The budget is not what tw holds at a real login.** The tests' software
renderer never loads the GPU driver, which is most of tw's memory on a real
screen, and the screen's buffers are shared memory that no process is
charged for. [g533zw.md](g533zw.md#what-is-left-and-what-was-not-done) has
the figures from a real login.

## Known noise

With a GPU renderer, LeakSanitizer reports small leaks at exit from inside
the GPU driver (seen with the NVIDIA driver and the libdbus it loads).
They are not tw's. The tests use the software renderer, where the report
must be empty.

## What no test reaches

- **Freeing Mesa's shader compiler.** The tests draw with the software
  renderer, for which `renderer_release_compiler()` does nothing. It was
  checked by hand: the release build headless with `WLR_RENDERER=gles2`,
  an empty `PATH` and a private runtime directory, `RssAnon` read after 3 s,
  three runs each: 14.5 MB before, 8.8 after, drawing unchanged.
- **Touchpad gestures.** The headless backend has no touchpad and no way
  to inject gestures,
  so the gesture handlers in `input.c` and the swipe functions in
  `strip.c` (`strip_swipe_*`, `workspace_snap()`, and with it a dialog
  keeping the keyboard through a swipe) are untested glue. Keep them thin,
  and put anything with logic in `layout.c`.
- **Popups of windows with a client-drawn shadow, and popups that outlive
  what they hang from.** The test client does neither, so two things rest
  only on reading wlroots' source: where a menu is kept on screen for a GTK
  window with a shadow, and that a menu's surface left behind by a closed
  window, or given a new menu, points at no freed or stale scene tree.
- **A workspace changing during a swipe**, a swipe the session locks
  during, and the cursor theme taken from `XCURSOR_THEME` and
  `XCURSOR_SIZE`: untested.
- **Physical keyboards and nesting.** The tests type through virtual
  keyboards, which bring their own keymap, and run headless, so the shared
  keymap of real keyboards, the fall back to xkbcommon's default layout when
  the `XKB_DEFAULT_*` one does not compile (`keymap_compile()` in
  `input.c`), and Mod being Alt in a window (with Mod+Alt bindings left
  out) are untested.
- **An animated cursor on a dark screen.** The `pointer` test checks that
  the window under the pointer loses it as the screens go off, which takes
  away its cursor; no test client sets an animated cursor, so the wlroots
  timer that steps one is not seen by the budget test.
- **A GPU reset.** The software renderer is never lost, so
  `handle_renderer_lost()` in `output.c` has never run, nor the frame it
  asks of each output (which follows what sway does), nor
  `surfaces_redraw()`, which ends the repaint loop a picture without a
  texture starts. Nor can a headless output
  be one that is not for a desktop (`non_desktop`).
- **The lid switch.** A lid cannot be emulated, and no headless output is
  a laptop's own screen, so the handler in `input.c` and `outputs_lid()`
  in `output.c` are untested: the laptop's own screen leaving the layout
  and coming back, its workspace and layer surfaces, blanking passing it
  over, and the lock not waiting for it or putting its lock surface on
  another screen. What decides (`tw_lid_look()`, `tw_connector_builtin()`)
  is unit tested in `layout_test.c`, and `lock_now()` through locking on
  idle. Closing the lid with a monitor plugged in has not been tried on
  the G533ZW, nor has a lid opened while on another VT (the switch coming
  back with no event, `new_switch()`).
- **Plugging and unplugging an output.** Headless outputs are all there
  from the start and cannot be unplugged, so `handle_destroy()` in
  `output.c` (the remaining output taking over the active workspace), a
  monitor plugged in while the screens are off (and two of them each
  taking a workspace of their own), and `tw-notify` showing its list again
  on a monitor plugged in after its own went away are untested. Every test
  exits through `output_finish()`, but with no output there that could
  bring another back as it goes.
- **Suspend and wake-up.** The machine cannot be suspended or the clock set
  from a test, so the wake-up watch in `idle.c` (and its telling a wake-up
  from the time being set) is untested, and the suspend command is a
  stand-in. That `locked` waits for the curtain to be presented is seen
  only in that it still arrives: the headless backend presents every frame
  at once.
- **Switching VTs.** The headless backend has no session, so the idle clock
  stopping while the user is on another VT, and coming back turning the
  screens on, are untested (`handle_session_active()` in `idle.c`).
- **An output refusing to come on.** Headless outputs never refuse, so
  input trying one again (`outputs_power()` in `idle.c`) is untested, and
  so is `output_commit()` falling back from a refused mode to the first
  mode the monitor takes.
- **Refresh rates.** Its outputs have no modes, so of the battery mode only
  the choice is tested (`tw_refresh_saves()`), not the loop over an
  output's modes in `output_mode()`, nor `outputs_update_mode()`, nor the
  mode half of `output_set_power()`.
- **A power supply changing.** Only the kernel can announce one, so
  `handle_change()` in `power.c` is untested (with it, that a mouse's
  battery announcing itself is passed over, and that a profile command
  started while another runs waits for it and is the one for the state at
  that moment), and with it everything about a low battery but the
  decision (`tw_battery_look()`). `tests/power.sh` covers what tw does with
  the state it finds as it starts.
- **Losing the seat.** The headless backend has no session, so tw
  exiting when seatd or logind goes away (`handle_session_destroy()` in
  `main.c`) is untested; it rests on reading wlroots'
  `backend/session/session.c`.
- **A GPU appearing** while tw runs with `WLR_DRM_DEVICES` unset (tw
  started without `tw-session`) makes wlroots open it
  (`backend/drm/monitor.c`); that path has never been run.
- **The window side of the clipboard.** `tests/clipboard.sh` copies and
  pastes through the data-control protocol, as clipboard tools do, and not
  the way windows do it (`wl_data_device`, which wants a window that has
  the keyboard). The two meet inside wlroots, before tw's code. The one
  window copy it makes (`client.c --copy-late`) checks the serial tw's own
  offer carries.
- **What a notification looks like.** `tests/notify.sh` reads what
  `tw-notify` says it does (`-d`) and what `dbus-monitor` sees on the bus,
  not the screen; the text layout is tested only for not crashing. The
  test's bus is dbus-daemon, not dbus-broker. Not reached there: a monitor
  unplugged under the list (the headless outputs stay), a message whose
  header tw-notify cannot read (dbus-daemon checks every header first;
  `bus_test.c` covers the reading), a click while the list is changing,
  and a `Ping` that names no interface (neither busctl nor gdbus sends one).
- **Real hardware behind the scripts.** `tests/gpu.sh`, `profile.sh`,
  `bluetooth.sh` and `thunderbolt.sh` cannot show that a real GPU, kernel,
  adapter or dock does what the made-up machine does, nor that the polkit
  rules let the units be started. `tests/x11.sh` proves tw's half of the
  hand-over, not that the real helper works with tw.

## Seen on a real machine

What has been seen to work outside the tests, and what has not been seen
yet, on CachyOS in November 2026. Everything was seen on the G533ZW unless
it names the G615LM. Take a line out of the last column once it has been
seen; the figures that go with these are in [g533zw.md](g533zw.md) and
[g615lm.md](g615lm.md).

| Area | Seen | Not seen yet |
| --- | --- | --- |
| Compositor | | windows moved between real monitors, by Mod+Ctrl past the end of the strip and with Mod and the mouse; drag and drop between real programs; a GTK 4 or Firefox menu that moves; the shared keymap with the laptop's hotkeys; the keyboard-light keys reaching tw (the G533ZW's keyboard announces them), and the kernel not also stepping the light |
| Session | login from a login screen and from a text console, keyring unlocked in both; on the G615LM after a reboot, its own entry of `src/machines.h` picked (its panel at 1.25, the screenshot 2048x1280 logical); a boot with no login manager; Chromium and Electron keep their logins (`GNOME_DESKTOP_SESSION_ID`); the accessibility bus stays off (systemd logs the unmet `ConditionEnvironment=!NO_AT_BRIDGE`); lock and unlock through the edited `/etc/pam.d/login`; `meson install` reloading systemd, polkit and the user's systemd; the built-in libdisplay-info giving the same make and model as the system's for the panel's EDID, read from `/sys` | `exec tw-session`; the rebuild hint in `tw-session`; Plasma after a logout from tw starting without tw's variables in systemd (`systemctl --user show-environment`); a login on the build with libdisplay-info built in (wlroots reads the EDID as the DRM backend starts); on the real screen, a click or an open menu while locked; the G533ZW's entry picked at a real login on it |
| X11 | with xwayland-satellite 0.8.3 and Xwayland 24.1: both stopped after `xdpyinfo` once the idle time ran out and started again on the next; kept up by `xprop -spy`, by a sandboxed client (`bwrap --unshare-net`), by an open `vkcube --wsi xcb`; in the real session, gone 43 s after the Claude app's start-up connection | |
| Clipboard | in a headless tw with real programs: a copy from foot outliving foot and pasted into another foot; `wl-copy --foreground`; a PNG byte for byte; `wl-copy --clear`; no wakeup while holding a copy | the installed build in daily use; copies from Chromium, GTK or Qt; a copy through xwayland-satellite, and tw stopping that helper while it owns the clipboard |
| Battery | announcements of an unchanged battery and charger every 35 to 110 s, acted on only when something changed; a made-up battery running down with real announcements: one warning, suspends at 5%, 4% and 2% and none on the charger; at a login on battery, the panel at 60 Hz from its first modeset and power-saver; at a login on mains, `tw-power balanced` without a password prompt | the panel changing mode while tw runs, and whether the screen blinks; a USB-C charger as a supply; the root helper writing the real `/sys`; a real suspend from a low battery; a wake-up told from the clocks |
| Notifications | `notify-send`, `busctl` and `dbus-send` all agree with `bus.c`; wrapping, the urgent border, one picture per change; under dbus-broker, tw's entry in `/usr/local` winning and each start a transient unit of its own; with fontconfig, in a headless tw: Latin, Greek, Cyrillic, Japanese and symbols, each from the first font that has them | the fontconfig build on a real screen at 1.25; a notification from the Claude app and its window coming forward after a click; a monitor going away under the list |
| NVIDIA GPU | RTX 3070 Ti, driver 620.70: the polkit rule; load and unload; the GPU in D3cold off the bus; Vulkan and OpenGL from it into a tw drawing with Intel, on Wayland and X11; `tw-gpu launch` from fuzzel; off the bus, `nvidia-smi`, `fastfetch` and `btop` refused and the GPU left off; ten switch-ons in a row; a GPU in use left alone and off once let go; at login, waiting for the login screen to let go; on the G615LM (RTX 5060, same driver), a monitor on it, plugged in at login, through a USB-C hub, with tw drawing on Intel | a window on the real screen drawn by the GPU; suspend while it is off; a machine whose GPU is a 3D controller; a machine whose first GPU has no `boot_vga`, or one whose NVIDIA GPU has it beside the panel's GPU; the guard after a kernel update, and the message with no driver installed; `nvidia-persistenced` stopped and started with the GPU; a GPU whose slot stayed powered for want of its sound function sleeping |
| Bluetooth | a boot with the blacklist: no adapter, no module, no `bluetoothd`, the USB device asleep; `off` in 0.13 s with the service's restarts refused; `on` in 1.25 s with the service, the radio, the timer and WirePlumber all following; a program calling the service while off refused; on the G615LM (BE200, `8087:0036`), the sleep rule after a boot: the adapter asleep with `control` at `auto` and no driver | going off by itself; a device connecting, and a connection ending and starting the timer over (no device is paired on that machine); an adapter not on USB; a Wi-Fi and Bluetooth chip that does not call itself Bluetooth as a whole device (both laptops' adapters do, so the rule's match by interface is not what was tested) |
| Thunderbolt | udev accepts the rule; nothing more | any device at all; whether a dock's USB ports and display outputs work before it is approved |

Nothing that needs a suspend can be seen on the G533ZW: it did not wake
from one, and sleep stays off there ([g533zw.md](g533zw.md#decisions)).
Those items wait for another machine; the G615LM has not been suspended
yet ([g615lm.md](g615lm.md#sleep)).

The real runs found faults the made-up machines had not, and each is in a
test now: the GPU switched off at the first login before the login screen
had let go of it; `btop` loading the driver again two seconds after it was
unloaded, which is why the GPU leaves the bus; udev's own driver load
colliding with the helper's; and, in the clipboard, the kernel closing a
dying program's pipe before its connection to tw, the other way round from
what the first version assumed.
