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
| `bus` | `notify/bus.c`: messages written and read back, cut short, damaged; nothing may be read past its end |
| `smoke` | everything a client can observe: sizes, focus, every key binding, workspaces, layer surfaces, fullscreen, the lock, command-line options, a clean exit; no window told a binding's key is held |
| `columns` | windows sharing a column, floating dialogs |
| `pointer` | pointer focus, click to focus, pointer lock, confinement; a window opening under a resting pointer gets it; while locked, a click without moving reaches no window, and an open menu gives the keyboard up to the lock screen |
| `outputs` | two monitors: a workspace on each, focus and columns between them |
| `idle` | screen blanking and inhibitors with a two-second timeout; locking on idle; suspend (a stand-in command) |
| `x11` | the X11 sockets, starting the helper on demand and stopping it when unused, with a stand-in helper |
| `clipboard` | a copy outlives its program; an emptied clipboard stays empty; long copies, slow and vanishing readers, the size limit, programs that die or do not answer while being read |
| `power` | what tw makes of the power supplies it finds as it starts, and the command it runs |
| `notify` | `tw-notify` on a private session bus and a headless tw: started on demand, expiry, clicks, leaving |
| `gpu` | `tw-gpu`, its root helper, and `tw-session`: the GPU choice (a second GPU that is a 3D controller too), what it passes on to services, a tw that cannot start; a GPU left on when nouveau drives it, when tw has every GPU open, and after a kernel update |
| `profile` | `tw-power` and its root helper: the three profiles, machines that lack a setting, the performance governor, nothing written twice, refusals |
| `bluetooth` | `tw-bluetooth` and its root helper: off at boot, on, off by itself and by hand, a program that still has Bluetooth open, every way switching on can fail, staying on after a kernel update |
| `thunderbolt` | that the Thunderbolt rule is still the kernel guide's and udev accepts it |
| `install` | `data/post-install.sh`: what it reloads after a real install, and that a staged or rootless one reloads nothing |
| `budget` | release build only: idle wakeups and memory, see below |

Several of these were checked by breaking the code under test one way at a
time and seeing a test fail each time: the clipboard (27 ways), `tw-notify`
(18), `tw-power` (21), `tw-bluetooth` (38), the accessibility condition and
its part of `tw-session` (5), the Thunderbolt rule (4). When doing that again: a breakage run that is
interrupted leaves a stale program in the scratch copy, and every test then
fails or hangs for no reason. Rebuild it from clean.

## The budgets

The three numbers at the bottom of `meson.build`, checked on the release
build with three clients mapped and X11 support waiting for its first
client:

| Budget | Now | Measured |
| --- | --- | --- |
| idle wakeups in 3 s | 0 | 0 |
| own memory (PSS minus pixel buffers) | 2800 kB | 2445 to 2713 |
| of which allocated by tw | 850 kB | 800 to 812 |

They ratchet down; raising one needs a stated reason.

**The memory figure swings; the allocated one does not.** PSS divides each
shared library page among the processes that map it, so "own memory" moves
by 100 to 200 kB with what else is running: it rose each time a program
that shared libraries with tw was removed from the machine (a login
manager's compositor, the KDE wallet, NetworkManager), with nothing in tw
changed. A second tw from the same build running during the test shares
the binary too, and the figure then reads about 1960. That is why this
budget has been raised three times (2500, 2650, 2800) and says little.

So when the budget test fails: run it again and look at the line "of which
allocated by tw" before looking for a regression. To tell what a change
costs, compare that line, and for the rest build the previous commit too
and run both budget tests back to back.

**At a real login tw holds 15 to 20 MB of its own, not 0.8.** The
difference is the GPU driver (Mesa), which the tests' software renderer
never loads: 14 MB the moment the GLES2 renderer exists on Intel, in two
driver instances, because wlroots opens one for the allocator and one for
the renderer. Turning off Mesa's shader cache makes it larger, not smaller.
tw also holds the screen's buffers, three at the panel's size (8 MB each at
1920x1080), which show up as shared memory (`Shmem`, `Unevictable`) and not
in any process. A client's window buffers are counted there too, once,
however many processes have them open.

## Known noise

- With a GPU renderer, LeakSanitizer reports small leaks at exit from
  inside the GPU driver (seen with the NVIDIA driver and the libdbus it
  loads). They are not tw's. The tests use the software renderer, where the
  report must be empty.
- A program that only asks X11 a question starts Xwayland, which tw stops
  again half a minute later. Two are known: fish's `done` plugin, which
  runs `xprop` each time a shell starts when `DISPLAY` is set
  ([setup.md](setup.md) says how to remove it), and the Claude desktop app,
  which connects to X11 for a moment as it starts. To find another one,
  open a terminal in a private headless tw run with `-d`, giving it the
  environment of a terminal from the real session, and look for "started
  xwayland-satellite" in its log. Xwayland still running with no X11 window
  open means a program without a window is connected, or one left and no
  window has closed since.

## What no test reaches

The headless backend sets the limits.

- **Touchpad gestures.** It has no touchpad and no way to inject gestures,
  so the gesture handlers in `input.c` and the swipe functions in
  `window.c` (`strip_swipe_*`, `workspace_snap()`) are untested glue. Keep
  them thin, and put anything with logic in `layout.c`.
- **Windows that unmap and map again, and popups of windows with a
  client-drawn shadow.** The test client does neither, so that a remapped
  window is not left `suspended` or fullscreen, and where a menu is kept
  on screen for a GTK window with a shadow, are only reasoned from
  wlroots' source.
- **A workspace changing during a swipe**, and the cursor theme taken from
  `XCURSOR_THEME` and `XCURSOR_SIZE`: untested.
- **The lid switch.** The handler in `input.c` is untested; what it calls
  (`lock_now()`) is tested through locking on idle.
- **Unplugging an output.** Its outputs cannot be unplugged, so
  `handle_destroy()` in `output.c` (the remaining output taking over the
  active workspace) is untested.
- **Suspend and wake-up.** The machine cannot be suspended or the clock set
  from a test, so the wake-up watch in `idle.c` is untested, and the
  suspend command is a stand-in.
- **Refresh rates.** Its outputs have no modes, so of the battery mode only
  the choice is tested (`tw_refresh_saves()`), not the loop over an
  output's modes in `output_mode()`, nor `outputs_update_mode()`, nor the
  mode half of `output_set_power()`.
- **A power supply changing.** Only the kernel can announce one, so
  `handle_change()` in `power.c` is untested, and with it everything about
  a low battery but the decision (`tw_battery_look()`). `tests/power.sh`
  covers what tw does with the state it finds as it starts.
- **A GPU appearing** while tw runs with `WLR_DRM_DEVICES` unset (tw
  started without `tw-session`) makes wlroots open it
  (`backend/drm/monitor.c`); that path has never been run.
- **The window side of the clipboard.** `tests/clipboard.sh` copies and
  pastes through the data-control protocol, as clipboard tools do, and not
  the way windows do it (`wl_data_device`, which wants a window that has
  the keyboard). The two meet inside wlroots, before tw's code.
- **What a notification looks like.** `tests/notify.sh` reads what
  `tw-notify` says it does (`-d`) and what `dbus-monitor` sees on the bus,
  not the screen; the text layout is tested only for not crashing. The
  test's bus is dbus-daemon, not dbus-broker.
- **Real hardware behind the scripts.** `tests/gpu.sh`, `profile.sh`,
  `bluetooth.sh` and `thunderbolt.sh` cannot show that a real GPU, kernel,
  adapter or dock does what the made-up machine does, nor that the polkit
  rules let the units be started. `tests/x11.sh` proves tw's half of the
  hand-over, not that the real helper works with tw.

## Seen on a real machine

All on the G533ZW, CachyOS, November 2026.

### The session

- Login from a login screen and from a text console (`tw-session` typed
  after logging in), keyring unlocked in both; a boot with no login manager
  at all.
- Chromium and Electron programs keep their login with
  `GNOME_DESKTOP_SESSION_ID` set.
- The accessibility bus stays off: at login the portal leaves it alone, and
  when the Claude app asks for it systemd logs "skipped, unmet condition
  check ConditionEnvironment=!NO_AT_BRIDGE", the app comes up as usual and
  no launcher is running afterwards.
- Lock and unlock with swaylock through the edited `/etc/pam.d/login`.

- `sudo meson install` ending with its reloads: systemd reloaded, polkit
  restarted and the user's systemd reloaded within a third of a second,
  with nothing said on the terminal.

- The libdisplay-info built into tw gives the same make and model for the
  panel's EDID as the system's library does (a small program linked
  against each, reading `/sys/class/drm/card1-eDP-1/edid`).

Not seen yet: `exec tw-session`; the rebuild hint in `tw-session`; a login
on the build that has libdisplay-info built in. wlroots calls it as the DRM
backend starts, which no test does.

### X11

With xwayland-satellite 0.8.3 and Xwayland 24.1: after `xdpyinfo` both were
gone when the idle time ran out, Xwayland along with the helper, and the
next `xdpyinfo` started them again. `xprop -spy` kept them up, through a
window closing too, and they went after it had left and the next window
closed. The same from inside `bwrap --unshare-net`, whose connection is
indeed missing from `/proc/net/unix`. `vkcube --wsi xcb` kept them up while
its window was open. In the real session: the Claude app started, the
helper two seconds later, and 43 seconds after that neither it nor Xwayland
was running.

### The clipboard

In a headless tw, with real programs: a word selected in foot with the
mouse and copied with Ctrl+Shift+C was read (five formats, kept once: 8
bytes), was still there after foot was killed, and was pasted into a second
foot, so the window side works in both directions. `wl-copy --foreground`
kept running (under `wl-clip-persist` it lost the clipboard at once). A PNG
came back byte for byte. `wl-copy --clear` left the clipboard empty.

Measured there: tw holds 28 kB more with a line of text copied, 20.4 MB
with a text of 20 MB that `wl-copy` offers in five formats
(`wl-clip-persist` held 98 MB for it) and is back down after the next,
small copy. In five seconds of offering a copy with nothing happening tw
did not wake once.

The rule for a program that dies while being read came out of running the
test over and over. A first version believed the kernel closes a dying
program's connection before its pipe; it is the other way round, and a
program made to die in the second of its three formats left the cut-short
one on offer in 11 of 300 tries. With the rule as it is now: none in 600.

Not seen yet: the installed build in the daily session; a copy from a
Chromium, GTK or Qt program, which offer more formats than foot does; a
copy made through xwayland-satellite, and tw stopping that helper while it
owns the clipboard.

### On battery

- The battery and the charger each announce a change every 35 to 110
  seconds with nothing changed; tw noticed a made-up charger going offline
  at the next announcement and ran each command once.
- A made-up battery running down, with real announcements: warned once at
  9%, suspended at 5%, did nothing more while the charge stayed at 5%,
  suspended again at 4%, did nothing at 3% on the charger, warned again but
  did not suspend when the charger went at 3%, and suspended at 2%.
- At a real login on battery the panel came up at 60 Hz from its first
  modeset and the profile was power-saver.
- At a real login on mains tw ran `tw-power balanced`: the unit ran for
  86 ms with nobody asked for a password, so the polkit rule works.

Not seen yet: the panel changing mode while tw runs (the cable pulled or
plugged in) and whether the screen blinks as it does; whether a USB-C
charger shows up as a supply that is online; the root helper writing to the
real `/sys` (at that login it most likely found all three settings as it
wanted them); a real suspend from a low battery; a wake-up being told from
the clocks.

### Notifications

Notifications from `notify-send` (GLib), `busctl` (systemd) and `dbus-send`
(libdbus), so three other implementations agree with `bus.c`. Wrapping, the
urgent border; one picture drawn per change at 1.25. From sending a
notification to the answer takes 9 ms when `tw-notify` has to be started
and 5 ms when it runs. (Measured with the font baked into the program,
before it used fontconfig.)

With fontconfig, in a headless tw: Latin, Greek, Cyrillic, Japanese and
symbols drawn, each from the first font fontconfig has for it (Noto Sans,
DejaVu Sans, Noto Sans CJK). About 2.9 MB of its own and 6.2 MB PSS with
those three fonts open.

Installed, under dbus-broker: the broker logged the other service's entry
as the duplicate, so tw's entry in `/usr/local` won, and systemd started
`tw-notify` as a transient unit for the first notification. Each start is a
unit with a name of its own, so leaving and being started again a moment
later do not collide.

Not seen yet: the fontconfig build on a real screen, at scale 1.25; a
notification from the Claude app, its window coming forward after a click,
and a monitor going away while the list is on it.

### The NVIDIA GPU

RTX 3070 Ti, driver 620.70.

- The polkit rule; loading the driver (2.4 s) and unloading it (0.5 s); the
  GPU powered down afterwards (`power_state` D3cold, also for the port
  above it).
- Vulkan on Wayland, and OpenGL and Vulkan on X11, drawing with the NVIDIA
  GPU into a tw that draws with the Intel one.
- `tw-gpu launch` with a real desktop entry, from fuzzel in the real
  session; the switch-off after the program's process group was hung up on.
- Off the bus: `nvidia-modprobe`, `nvidia-smi`, `fastfetch` and `btop` are
  all refused ("NVRM: No NVIDIA GPU found") and leave it off; scanning the
  port brings it back, driver loaded, in 2.4 s; ten switch-ons in a row
  after the slot had been cold for 2 to 60 seconds.
- A GPU that another program has open is left alone, named in the journal
  ("in use by ..."), and goes off within seconds of that program exiting.
- At a real login: refused while the login screen held the GPU, off the
  bus three seconds after the login screen stopped. A login while the GPU
  is off: it stayed off.

Real runs found four faults the made-up machine had not, all now in the
test: among them the switch-off at the first real login, which came before
the login screen let go of the GPU; a driver loaded again by `btop` two
seconds after it had been unloaded (the sign: `nvidia` and `nvidia_uvm`
loaded, `nvidia_drm` not), which is why the GPU leaves the bus; and udev's
early load colliding with the helper's.

Not seen yet: a window on the real screen drawn by the NVIDIA GPU, a
monitor on the NVIDIA GPU, and suspend while it is off.

### Bluetooth

- A boot with the blacklist: no adapter, no Bluetooth module in the kernel,
  no `bluetoothd`, and the adapter's USB device asleep with `power/control`
  at `auto`, which only the udev rule can have set.
- `off`: the service stopped, the bus asked for it twice within 18 ms and
  systemd skipped both, the core out of the kernel 46 ms after the stop;
  0.13 s in all.
- `on`: 1.25 s. The core and `btusb` loaded, systemd started `bluetoothd`
  for the new adapter by itself, the radio came up unblocked and powered,
  the timer was set, and WirePlumber registered its endpoints with the new
  daemon. A WirePlumber started while Bluetooth is off logs one warning and
  takes the daemon up when it appears.
- A program that calls the Bluetooth service while it is off (Chromium
  does, as it starts) is refused by the condition.

Not seen yet: Bluetooth going off by itself; a device connecting, and a
connection ending and starting the timer over (the rule matches
`DEVTYPE=link`, a name taken from the kernel module's strings). No device
is paired on that machine.

### Thunderbolt

Nothing. The rule has never met a device: udev accepts it, and that is all.
The kernel's guide leaves open whether a dock's USB ports and display
outputs work before the device is approved. [setup.md](setup.md) has the
dock tried with boltd masked before the package goes.
