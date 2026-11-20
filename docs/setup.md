# Setting up a machine for tw

For CachyOS and Arch: everything tw needs, from packages to the first
login, and what to do when something goes wrong. Making the rest of the
system small (services, kernel settings, replacing KDE's parts) is not
tw's business and is in [system.md](system.md).

1. [Install](#1-install)
2. [What comes with tw](#2-what-comes-with-tw): the NVIDIA GPU, Bluetooth,
   Thunderbolt, power profiles, notifications, sharing a window
3. [Sleep](#3-sleep)
4. [After a system update, and when something is wrong](#4-after-a-system-update-and-when-something-is-wrong)

Figures such as "7 MB in `btop`" were measured on one laptop
([g533zw.md](g533zw.md)) and will differ a little elsewhere.

## 1. Install

### Packages

To build:

```sh
sudo pacman -S meson ninja gcc git pkgconf wayland wayland-protocols libxkbcommon \
    libinput pixman libdrm mesa libglvnd systemd-libs seatd hwdata \
    freetype2 fontconfig noto-fonts
```

The last three are for the notifications (`tw-notify`). Without
`freetype2` and `fontconfig` tw builds and `tw-notify` is left out.

Programs tw starts or works with. None is required to build, but each key
or feature does nothing without its program:

```sh
sudo pacman -S foot fuzzel swaylock xwayland-satellite \
    grim slurp brightnessctl wl-clipboard wireplumber libnotify \
    xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr
```

| Program | What it is for |
| --- | --- |
| foot | terminal (Mod+T); one server process, started with the session, draws every terminal window |
| fuzzel | launcher (Mod+Space, Mod+D) |
| swaylock | lock screen (Mod+Alt+L, lid close, idle). Without it tw does not lock at all |
| xwayland-satellite | X11 programs; started when one runs, stopped when the last has gone |
| grim, slurp | screenshots (Print, Shift+Print) |
| wl-clipboard | `wl-copy` and `wl-paste`, the clipboard from a terminal |
| brightnessctl, wireplumber | brightness, keyboard-light and volume keys |
| libnotify | `notify-send`, for the low-battery warning and the level the volume and brightness keys set. What shows it is `tw-notify`, which comes with tw |
| xdg-desktop-portal, -gtk, -wlr | file dialogs and screen capture (OBS, browsers) |

A few more are named where a check below needs them: `bluez` and
`bluez-utils` (Bluetooth), `vulkan-tools` (the NVIDIA GPU).

### This machine's settings

Two things about a machine are compiled in: its screens (scale, position)
and whether tw may put it to sleep. They are an entry per machine in
[src/machines.h](../src/machines.h), named after the machine's board, and
tw picks the entry as it starts, so one installed tw serves every machine
its disk boots. A machine without an entry gets the last one: every screen
at scale 1, no sleep. That works everywhere, so this step can wait until
after the first login.

```sh
cat /sys/class/dmi/id/board_name    # the entry's .board
```

`tw -d` logs which entry it took (`machine G615LM`, or `machine other`) and
each screen's connector name (`output eDP-1`) for the entry's outputs.
Commit the entry: the next machine then builds from the same tree.

A monitor you carry between machines (or plug in through a dock) goes in
`monitors`, below the machines, by the name it gives itself: plug it in
under `tw -d` and copy the name from the line `output HDMI-A-1 is <make>
<model> <serial>`, leaving out the serial unless two of the same model are
on one desk. It then has its scale on every machine, at every port.

A build directory set up before machines were picked at startup may
remember the old `machine` option, and Meson then refuses to build in it
("Unknown options: machine"). Delete that directory and set it up again.

Three build options decide what else is installed. The defaults suit a
laptop; look at Bluetooth before installing on a desktop.

| Option | Default | Turn it off when |
| --- | --- | --- |
| `-Dbluetooth=false` | on | the machine needs Bluetooth from boot: a Bluetooth keyboard or mouse. Left on, Bluetooth is off until `tw-bluetooth on` |
| `-Dthunderbolt=false` | on | you want to be asked before a Thunderbolt device is let in |
| `-Dnotify=disabled` | on where FreeType and fontconfig are installed | you use another notification service |

Turning one off after an install is safe: the next install removes the
files the earlier one put there for it, and says which.

### Build and install

Meson downloads two pinned sources on the first `setup`, wlroots and
libdisplay-info, and builds both into tw. Everything else comes from the
packages above. If one is missing or older than wlroots needs (Wayland
1.24, wayland-protocols 1.47, libdrm 2.4.129, xkbcommon 1.8, pixman 0.43),
`setup` stops and names it: update the system and run it again. It never
fetches a library of its own.

```sh
meson setup build-release --buildtype=release -Db_lto=true
meson test -C build-release --suite tw
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

Options go on the first line (`-Dbluetooth=false`), or later with
`meson configure build-release -Dbluetooth=false`.

After a change, `meson compile -C build-release` and the last line again.
The install has systemd, udev and polkit read what
it brought, so nothing needs reloading by hand. A running session keeps the old `tw` until you log
out and in again.

Everything goes under `/usr/local`: `tw`, `tw-session`, `tw-notify`,
`tw-gpu`, `tw-power`, `tw-bluetooth` in `bin`, their root helpers in
`libexec`, units and rules under `lib` and `share`. Nothing is written
outside it, so `sudo ninja -C build-release uninstall` takes all of it away
again.

### Log in

Pick "tw" in the login screen's session list, or log in on a text console
and type `tw-session`. The screen is black with a cursor until you open
something: there is no wallpaper and no bar. Mod is the Super key; Mod+T
opens a terminal, Mod+Shift+E quits, and Ctrl+Alt+F3 is a text console if
something goes wrong. The keys are in the [README](../README.md#keys).

To try tw without logging out, run it in a window inside the current
session, where Mod is Alt: `./build-release/tw -s foot`. Do not run
`tw-session` there; it is for the real screen.

## 2. What comes with tw

Installed with it and working without setup. Each does nothing on a machine
without the hardware it is about.

| Part | What it does | Check after installing |
| --- | --- | --- |
| `tw-gpu` | keeps a laptop's NVIDIA GPU off until a program needs it | below |
| `tw-bluetooth` | keeps Bluetooth off until asked for | below |
| Thunderbolt rule | lets a dock in without boltd | below |
| `tw-power` | power-saver profile on battery, balanced on mains | `tw-power` says which is set |
| `tw-notify` | shows notifications, and runs only while one shows | `notify-send Hello there` |
| Keyboard rule | lets a laptop's built-in USB keyboard sleep two seconds after the last key | nothing to do |
| Power key | a short press does nothing; hold it for five seconds, or type `systemctl poweroff`, to power off | a short press leaves everything running |

### The NVIDIA GPU: off until a program needs it

On a laptop with an Intel (or AMD) GPU and an NVIDIA one, tw draws with the
first. The NVIDIA GPU is switched off when you log in, unless a monitor is
plugged into one of its ports at that moment. Off means its driver is
unloaded and the GPU is taken off the PCI bus: it is powered down, the
memory the driver keeps (about 380 MB) is free, and nothing can switch it
back on by accident. Programs that only look at the GPU, such as `btop`,
`fastfetch` and `nvidia-smi`, see no NVIDIA GPU while it is off; without
that they would load the driver again every time they run.

A program gets the GPU in one of two ways:

- From the launcher, automatically, if its desktop entry asks for the
  discrete GPU with `PrefersNonDefaultGPU=true`. Steam and Blender ship
  that line. To give it to another program, copy its entry from
  `/usr/share/applications` to `~/.local/share/applications` and add the
  line.
- From a terminal or a script: `tw-gpu run <program>`. In Steam, a game's
  launch options can be `tw-gpu run %command%`, which keeps Steam itself on
  the first GPU.

The GPU is switched on before the program starts, which takes about two
seconds, and off when the program and everything it started have exited,
also if you close its terminal or press Ctrl-C. If something else still
has the GPU open at that point (`btop` watching it, for example), it goes
off as soon as that program has closed, if that is within ten minutes;
after that `tw-gpu` stops looking, and the next `tw-gpu off` or program
run with it switches the GPU off. NVIDIA's two daemons, `nvidia-powerd`
and `nvidia-persistenced`, do not count: they are stopped with the GPU,
and started again with it if they are enabled.

| Command | What it does |
| --- | --- |
| `tw-gpu run <program>` | runs a program on the NVIDIA GPU |
| `tw-gpu on` | switches it on and leaves it on until `tw-gpu off` |
| `tw-gpu off` | switches it off, or as soon as nothing uses it any more |
| `tw-gpu` | says whether it is on, and whether it is powered |

Check it on the machine, because no test can (`vkcube` is in
`vulkan-tools`):

```sh
tw-gpu run vkcube     # a spinning cube; close it
tw-gpu status         # a few seconds later: "off, powered down"
```

If the status says "off, but its slot is still powered", the GPU did not
power down once it was off the bus, and it uses more battery that way than
before. Run `tw-gpu on` to go back to how it was, and look into it. If
`tw-gpu on` ever fails, it says why or where to look
(`journalctl -b -u tw-gpu@on`); if it cannot bring the GPU back, a
reboot does.

Monitor ports wired to the first GPU work whenever you plug in. Ports wired
to the NVIDIA GPU (on many laptops, HDMI) only work if the monitor is
connected when you log in after a boot. If you have logged out and in
since, the GPU is still switched off and tw cannot see the monitor: run
`tw-gpu on`, then log out and in. `TW_ALL_GPUS=1 tw-session` uses every GPU
for the whole session, at the price of the second one's power and memory;
it also brings back a GPU switched off earlier in this boot.

### Bluetooth: off until you ask for it

From the first boot after installing, Bluetooth is off, and stays off until
you switch it on:

| Command | What it does |
| --- | --- |
| `tw-bluetooth on` | switches Bluetooth on, which takes about two seconds. Then `bluetoothctl` as ever; a device you paired before connects by itself when you turn it on |
| `tw-bluetooth off` | switches it off now, whatever is connected. Also the way to switch it off before the first reboot |
| `tw-bluetooth` | says which it is |

It also goes off by itself, ten minutes after the last device has
disconnected, or after `tw-bluetooth on` if nothing ever connected.

Off means the adapter's driver is not loaded. A file the install puts in
`/usr/local/lib/modprobe.d` keeps it from loading as the machine starts;
`tw-bluetooth on` loads it. Without the driver the kernel has no adapter,
so systemd, which otherwise starts `bluetoothd` whenever there is one, never
does, and `bluetooth.service` stays enabled as it is. What that frees:
`bluetoothd` (7 MB in `btop`) and the Bluetooth kernel modules.

Check it on the machine, because no test can (`bluez`, `bluez-utils`):

```sh
tw-bluetooth off
tw-bluetooth                  # Bluetooth: off
lsmod | grep -c bluetooth     # 0
tw-bluetooth on
bluetoothctl show | grep Powered    # Powered: yes
```

Three things to know.

- A mouse or keyboard that drops its connection to save its own battery,
  and comes back when touched, finds Bluetooth off if it was away for ten
  minutes. For one of those make the time longer (`OnActiveSec` in
  [data/tw-bluetooth-idle.timer](../data/tw-bluetooth-idle.timer)), or
  build with `-Dbluetooth=false`, which leaves Bluetooth as the
  distribution set it up.
- An adapter that is not on USB needs its driver named in
  [data/tw-bluetooth.conf](../data/tw-bluetooth.conf).
- If `tw-bluetooth on` ever fails, `journalctl -b -u tw-bluetooth@on` says
  why, and `sudo touch /run/tw-bluetooth && sudo modprobe btusb` does by
  hand what it does.

### Thunderbolt: a dock without a daemon

A Thunderbolt or USB4 port does not let a device's fast side in by itself:
a dock's network port and the disks on it, or an external GPU, stay off
until something approves the device. `boltd` (package `bolt`) is the
program for that. It is started at every boot, because the port itself
counts as a device, and stays (10 MB in `btop`, awake every eleven seconds)
for the day a dock is plugged in.

On a machine whose kernel shields its memory from such devices, `boltd`
approves every device without asking, and that needs no program. tw
installs the one udev rule the kernel's own guide gives for it. Whether
your machine is one of those:

```sh
cat /sys/bus/thunderbolt/devices/domain0/iommu_dma_protection   # 1: yes
```

If that reads 0, or you want to be asked before a device is let in, keep
`bolt` and build with `-Dthunderbolt=false`. If it reads 1, try the dock
without `boltd` before you remove it:

```sh
sudo systemctl mask --now bolt.service
```

Plug the dock in and try everything on it. Then one of:

```sh
sudo pacman -Rns bolt                    # it all works: boltd can go
sudo systemctl unmask bolt.service       # it does not: back to boltd, plug the dock in again
```

A device that is waiting can also be let in by hand, which shows whether
approval is what it lacks:
`grep . /sys/bus/thunderbolt/devices/*/authorized` (0 is waiting), then
`echo 1 | sudo tee /sys/bus/thunderbolt/devices/0-1/authorized` for that
device. A plain USB-C hub or dock, a charger and a monitor on USB-C are not
Thunderbolt devices: they never needed `boltd` and show nothing here.

### Power profiles, notifications

`tw-power` and `tw-notify` do the jobs of two daemons that would otherwise
stay for the session. With tw installed those can go:

```sh
sudo pacman -Rns power-profiles-daemon
sudo pacman -Rns <notification service>   # mako, dunst, fnott: whichever is installed
```

tw sets a profile only on a machine with a battery: as it starts, and as
the power cable goes in or out. `tw-power performance` (or `balanced`,
`saver`) sets one by hand, on any machine, until the cable next moves or
tw next starts.

`tw-notify`'s entry is in `/usr/local`, which D-Bus reads before `/usr`, so
it is the one used from the next login even while another service is still
installed. To check, after logging in again: `notify-send Hello there`
shows a box in the top right corner that goes after six seconds, and a
second later `pgrep -x tw-notify` prints nothing.

### Sharing a window (OBS, browsers)

Programs that record or share the screen ask xdg-desktop-portal-wlr, which
asks you what to share. Left to itself it asks through `slurp`, which can
pick only a whole screen. To be offered the windows as well, in a fuzzel
menu, create `~/.config/xdg-desktop-portal-wlr/tw`:

```ini
[screencast]
chooser_type=dmenu
chooser_cmd=fuzzel -d -l 10 -p 'Share: '
```

It counts from the next login (the portal reads it as it starts). In OBS
the source is "Screen Capture (PipeWire)". A shared window is recorded even
while it is scrolled out of sight, at scale 1: on a screen at 1.25 it comes
out with 80% of the pixels it shows there, which sharing the whole screen
avoids.

### The screen back on when Claude Code is done

The screen goes off 2 minutes after the last input, and a notification
turns it back on. Claude Code can send one when it finishes and when it
needs an answer, through two hooks in `~/.claude/settings.json` (merged
into what is there; `/hooks` in Claude Code shows them):

```json
"hooks": {
  "Stop": [{"hooks": [{"type": "command", "command": "grep -qsx On /sys/class/drm/card*-*/dpms || notify-send -a 'Claude Code' 'Claude Code' 'Done, waiting for you' 2>/dev/null || true"}]}],
  "Notification": [{"hooks": [{"type": "command", "command": "grep -qsx On /sys/class/drm/card*-*/dpms || notify-send -a 'Claude Code' 'Claude Code' 'Needs your input' 2>/dev/null || true"}]}]
}
```

Each sends a notification only while every screen is off (no connector's
`dpms` reads `On`), so a reply watched as it comes needs none.

## 3. Sleep

tw can suspend the machine after a time without input and when the battery
runs low. Both are off in a new entry of `src/machines.h`, and stay off
until the machine has been seen to wake: the G533ZW once did not, and had
to be switched off and on again ([g533zw.md](g533zw.md#sleep)).

Try it by hand first, with nothing unsaved open:

```sh
systemctl suspend
```

Press a key or the power button. If the machine comes back with its screen
and its network, give its entry in `src/machines.h` a time and a charge
(`suspend_after_seconds = 900`, `battery_suspend_percent = 5`), build and
install. A long job run as `systemd-inhibit --what=sleep <command>` holds
the idle suspend off; a refused attempt is tried again one period later, so
the machine sleeps up to one period after the job ends. Closing the lid
suspends at once whatever holds it off (systemd's default), unless a monitor
is plugged in; tw locks the session as the lid closes.

Claude Code holds nothing off by itself on Linux (it does on macOS), so a
long run left alone would be suspended in the middle. Three more hooks in
`~/.claude/settings.json` hold a sleep inhibitor from the prompt to the end
of the answer; it also goes when that Claude Code exits, however it exits:

```json
"UserPromptSubmit": [{"hooks": [{"type": "command", "command": "f=\"$XDG_RUNTIME_DIR/claude-awake.$PPID\"; [ -e \"$f\" ] || setsid -f sh -c 'echo $$ >\"$1\"; exec systemd-inhibit --what=sleep --who=\"Claude Code\" --why=\"Working\" waitpid \"$2\"' - \"$f\" \"$PPID\" </dev/null >/dev/null 2>&1; true"}]}],
"SessionEnd": [{"hooks": [{"type": "command", "command": "f=\"$XDG_RUNTIME_DIR/claude-awake.$PPID\"; [ -e \"$f\" ] && kill -- -\"$(cat \"$f\")\" 2>/dev/null; rm -f \"$f\"; true"}]}]
```

and the `SessionEnd` command once more as a second hook under `Stop`. An
answer stopped with Esc runs no `Stop` hook, so the inhibitor then stays
until the next answer ends or Claude Code exits. `systemd-inhibit --list`
shows it. Costs: two sleeping processes per working session, no wakeups
(`waitpid` waits on a pidfd).

If it does not come back, leave both at 0 and take sleep away from the two
things outside tw that still lead there: closing the lid (systemd does
that by itself) and `systemctl suspend` typed by hand.

```sh
sudo systemctl mask sleep.target suspend.target
```

Closing the lid then only locks the session, and with a monitor plugged
in only turns the laptop's own screen off. On battery the laptop runs
until it is empty: the warning at 10% is all there is, so plug in when it
shows. `sudo systemctl unmask sleep.target suspend.target` gives sleep
back.

## 4. After a system update, and when something is wrong

**tw does not start after a system update.** tw is built against the
system's libraries, and an update can replace one with a version of another
name. The two that change with every release, wlroots and libdisplay-info,
are built into tw for that reason; the rest (Wayland, libinput, xkbcommon,
Mesa, libdrm, pixman, udev, libseat) have kept their names for years. A
CachyOS that moves to a new wlroots changes nothing for tw: it never uses
the system's. If
one ever does change, `tw-session` says so and the loader names the
library. Log in on a text console and build tw again from clean, in its
source directory:

```sh
meson setup --wipe build-release
meson compile -C build-release
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

`--wipe` keeps the build's options and needs no network: the two sources
are already in `subprojects/`.

**Moving wlroots or libdisplay-info to a new version.** Each is pinned to
one commit in its `subprojects/*.wrap`, with the release's tag in the
comment above. A new wlroots release changes its API, so moving that pin
is a change of its own, with tw's code changed to match. To move one, find
the commit the release's tag names, put it in the wrap file as `revision`
along with the tag in the comment, and fetch it. For wlroots, also copy the
minimum versions its `meson.build` asks of the system libraries into the
list at the top of tw's `meson.build`:

```sh
git ls-remote https://gitlab.freedesktop.org/wlroots/wlroots.git 'refs/tags/<tag>^{}'
meson subprojects update --reset wlroots        # or delete subprojects/wlroots
meson setup --wipe build-release
```

`--reset` throws away anything changed by hand in that checkout and puts
back the wrap's patch, if it has one. Until a checkout matches its wrap
file, `meson setup` warns that the subproject "may be out of date"; the
same command silences it.

**Something else is wrong.**

| Symptom | Look at |
| --- | --- |
| tw exits or will not start | run `tw-session` from a text console: what it prints stays on screen. `tw -d` logs everything |
| a key does nothing | its program is not installed (the table in part 1); `tw -d` logs "not installed" |
| the session is locked and no lock screen comes | any key starts the locker again. If none comes, swaylock is failing: what it says is on the console tw was started from. To get back in, go to another console (Ctrl+Alt+F3), log in and run `WAYLAND_DISPLAY=wayland-1 swaylock -C /dev/null` (the socket tw logged at start, `ls $XDG_RUNTIME_DIR`), then go back and type your password: any working locker can take over a lock that has none |
| Xwayland keeps running | [Keep the shell from starting Xwayland](system.md#keep-the-shell-from-starting-xwayland) |
| the NVIDIA GPU will not go off | `tw-gpu`, then `journalctl -b -u tw-gpu@off` names who has it open |
| `tw-gpu` says the running kernel's modules are gone, or `tw-bluetooth` that its driver "is not on the disk" | a kernel update removed the running kernel's modules; reboot. Until then the GPU or Bluetooth is left as it is rather than switched off for good |
| `tw-gpu` says no NVIDIA driver is installed | the machine has an NVIDIA GPU but no NVIDIA driver for the running kernel. Install one, or leave it: `tw-gpu off` still takes the GPU off the bus, and `tw-gpu run` runs programs on the first GPU |
| a program does not stay signed in | `secret-tool lookup` as in [the keyring step](system.md#the-keyring-oo7); `journalctl --user -u oo7-daemon` |
| where the memory goes | [g533zw.md](g533zw.md#memory) on how to read `/proc/meminfo` and `btop` |
