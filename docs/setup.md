# Setting up a machine

For CachyOS and Arch. Part 1 is all tw needs. The rest is what makes the
machine around it small and quiet, each step on its own: do the ones that
fit the machine and skip the rest. Add to this file whenever a change
needs a new program or a step done by hand.

1. [Install](#1-install)
2. [What comes with tw](#2-what-comes-with-tw): the NVIDIA GPU, Bluetooth,
   Thunderbolt, power profiles, notifications
3. [A leaner system](#3-a-leaner-system): services, the shell, kernel
   settings
4. [Replacing KDE's parts](#4-replacing-kdes-parts): keyring, login, Wi-Fi
5. [Sleep](#5-sleep)
6. [After a system update, and when something is wrong](#6-after-a-system-update-and-when-something-is-wrong)

Figures such as "7 MB in `btop`" were measured on one laptop
([g533zw.md](g533zw.md)) and will differ a little elsewhere.

## 1. Install

### Packages

To build:

```sh
sudo pacman -S meson ninja gcc pkgconf wayland wayland-protocols libxkbcommon \
    libinput pixman libdrm mesa libglvnd systemd-libs libdisplay-info hwdata \
    freetype2 fontconfig noto-fonts
```

The last three are for the notifications: their text is drawn from the
system's sans-serif font, at build time, so nothing font-related is needed
to run. Without them tw builds and `tw-notify` is left out.

Programs tw starts or works with. None is required to build, but each key
or feature does nothing without its program:

```sh
sudo pacman -S foot fuzzel swaylock xwayland-satellite \
    grim slurp brightnessctl wl-clipboard wireplumber libnotify \
    xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr
```

| Program | What it is for |
| --- | --- |
| foot | terminal (Mod+T) |
| fuzzel | launcher (Mod+Space, Mod+D) |
| swaylock | lock screen (Mod+Alt+L, lid close, idle). Without it tw does not lock at all |
| xwayland-satellite | X11 programs; started when one runs, stopped when the last has gone |
| grim, slurp | screenshots (Print, Shift+Print) |
| wl-clipboard | `wl-copy` and `wl-paste`, the clipboard from a terminal |
| brightnessctl, wireplumber | brightness and volume keys |
| libnotify | `notify-send`, for the low-battery warning. What shows it is `tw-notify`, which comes with tw |
| xdg-desktop-portal, -gtk, -wlr | file dialogs and screen capture (OBS, browsers) |

Further packages are named where a later step needs them: `oo7` and
`libsecret` (the keyring), `iwd` and `iw` (Wi-Fi), `bluez` and
`bluez-utils` (Bluetooth), `vulkan-tools` (checking the NVIDIA GPU), a file
manager such as `thunar`.

### This machine's settings

Two things about a machine are compiled in: its screens (scale, position)
and whether tw may put it to sleep. They live in one small file per
machine, `src/machines/<name>.h`, and the build picks the file named after
the machine's board. With no such file it uses
[default.h](../src/machines/default.h): every screen at scale 1, no sleep.
That works everywhere, so this step can wait until after the first login.

```sh
tr 'A-Z' 'a-z' </sys/class/dmi/id/board_name | tr -c 'a-z0-9\n' _    # the name
cp src/machines/default.h src/machines/<name>.h                      # then edit
```

`tw -d` logs each screen's connector name (`output eDP-1`) for the output
rules. The build says which file it took ("machine: src/machines/...").
`-Dmachine=<name>` names one outright. Commit the file: the next machine
then builds from the same tree.

Three build options decide what else is installed. The defaults suit a
laptop; look at Bluetooth before installing on a desktop.

| Option | Default | Turn it off when |
| --- | --- | --- |
| `-Dbluetooth=false` | on | the machine needs Bluetooth from boot: a Bluetooth keyboard or mouse. Left on, Bluetooth is off until `tw-bluetooth on` |
| `-Dthunderbolt=false` | on | you want to be asked before a Thunderbolt device is let in |
| `-Dnotify=disabled` | on where FreeType is installed | you use another notification service |

### Build and install

Meson downloads the pinned wlroots on the first `setup`.

```sh
meson setup build-release --buildtype=release -Db_lto=true
meson test -C build-release --suite tw
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

Options go on the first line (`-Dbluetooth=false`), or later with
`meson configure build-release -Dbluetooth=false`.

After a change, `meson compile -C build-release` and the last line again.
The install has systemd, udev and polkit read what it brought, so nothing
needs reloading by hand. A running session keeps the old `tw` until you log
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
off as soon as that program has closed.

| Command | What it does |
| --- | --- |
| `tw-gpu run <program>` | runs a program on the NVIDIA GPU |
| `tw-gpu on` | switches it on and leaves it on until `tw-gpu off` |
| `tw-gpu off` | switches it off, or as soon as nothing uses it any more |
| `tw-gpu status` | says whether it is on, and whether it is powered |

Check it on the machine, because no test can (`vkcube` is in
`vulkan-tools`):

```sh
tw-gpu run vkcube     # a spinning cube; close it
tw-gpu status         # a few seconds later: "off, powered down"
```

If the status says "off, but its slot is still powered", the GPU did not
power down once it was off the bus, and it uses more battery that way than
before. Run `tw-gpu on` to go back to how it was, and look into it. If
`tw-gpu on` ever fails to bring the GPU back, a reboot does.

Monitor ports wired to the first GPU work whenever you plug in. Ports wired
to the NVIDIA GPU (on many laptops, HDMI) only work if the monitor is
connected when you log in after a boot. If you have logged out and in
since, the GPU is still switched off and tw cannot see the monitor: run
`tw-gpu on`, then log out and in. `TW_ALL_GPUS=1 tw-session` uses every GPU
for the whole session, at the price of the second one's power and memory.

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
sudo pacman -Rns fnott          # or whichever notification service is installed
```

tw sets a profile only on a machine with a battery, as the power cable
goes in or out. `tw-power performance` (or `balanced`, `saver`) sets one by
hand, on any machine, until the cable next moves.

`tw-notify`'s entry is in `/usr/local`, which D-Bus reads before `/usr`, so
it is the one used from the next login even while another service is still
installed. To check, after logging in again: `notify-send Hello there`
shows a box in the top right corner that goes after six seconds, and a
second later `pgrep -x tw-notify` prints nothing.

## 3. A leaner system

None of this is needed. Each step says what it saves.

### Services and packages tw has no use for

```sh
sudo systemctl disable --now ananicy-cpp.service
sudo systemctl disable --now switcheroo-control.service
sudo systemctl disable --now systemd-userdbd.socket systemd-userdbd.service
sudo pacman -Rns alsa-plugins wl-clip-persist
```

| What | What it is | Why it can go |
| --- | --- | --- |
| ananicy-cpp | CachyOS's daemon that adjusts process priorities by a rule list | about 7 MB, and with only a couple of programs running there is little for it to sort out |
| switcheroo-control | tells GNOME and KDE which GPU is the discrete one | `tw-gpu` does that job |
| systemd-userdbd | looks users up for systemd-homed and for user records kept as JSON files | neither is in use; it is four processes, 25 MB in `btop`, and users are found without it |
| alsa-plugins | extras for programs that talk to ALSA directly | one of them, an AC-3 encoder, is loaded by WirePlumber at every login and brings FFmpeg along, 79 libraries in all: WirePlumber is 25 MB in `btop` without it instead of 37. What goes with it is AC-3 to an S/PDIF receiver, and ALSA's own bridges to JACK, OSS and PulseAudio |
| wl-clip-persist | kept what you had copied when its program exited | tw does that itself |

Leave `avahi-daemon` alone if you use KDE Connect, which finds devices
through it.

### Keep the shell from starting Xwayland

tw starts Xwayland when a program talks to X11, and stops it again half a
minute after the last such program has gone. While it runs it takes about
25 MB. A program that merely asks X11 a question is enough to start it, and
CachyOS's fish setup ships one: the `done` plugin runs `xprop` every time a
shell starts and around every command, so Xwayland would start and stop all
day. Its notifications do not work under Wayland anyway, so remove it.

To check: open a terminal, wait a minute, then run `pgrep -x Xwayland`. If
that prints a number although you have no X11 program open, something keeps
waking it.

The plugin can be loaded from two places:

```sh
# Your own copy, if there is one:
rm ~/.config/fish/conf.d/done.fish

# CachyOS's copy. Its stock config loads it; see whether yours pulls that in:
grep -n cachyos-config ~/.config/fish/config.fish
```

If the `grep` prints a
`source /usr/share/cachyos-fish-config/cachyos-config.fish` line, that file
loads the plugin on its second line. Replace the `source` line with the
parts of that file you want to keep, leaving out the `done.fish` line. Then
close every terminal that was already open.

To find some other program doing this, look for X11 tools in your shell's
startup files: `grep -rn 'xprop\|xdotool\|xset\|xrdb' ~/.config/fish`.

### The kernel's memory reserves

Of what `btop` and `free` call "used" right after login, a good part is
free memory that the kernel holds back (`sh tools/mem.sh` shows it as
"held back": 285 MB on a 16 GB machine with huge pages on). Two settings
shrink that to about 140 MB and took 170 MB off the figure:

```sh
printf 'vm.min_free_kbytes = 16384\nvm.lowmem_reserve_ratio = 1024 1024 32 0 0\n' |
    sudo install -Dm644 /dev/stdin /etc/sysctl.d/90-memory.conf
sudo sysctl --system
```

| Setting | What it is | What changing it costs |
| --- | --- | --- |
| `vm.min_free_kbytes` | the memory the kernel always keeps free. With huge pages on it picks 66 MB; 16 MB is what it picks without them on a 16 GB machine | huge pages stay on, but once the file cache has filled the RAM a newly started program more often gets normal pages first and huge ones a little later |
| `vm.lowmem_reserve_ratio` | how much of the first 4 GB is kept for devices that can only use that part: 58 MB by default, 14 MB with this | nothing found |

On a machine with a different amount of RAM, take the square root of 16
times the RAM in kB for the first value (22600 for 32 GB).

To see whether huge pages suffer, look at
`grep thp_fault_fallback /proc/vmstat` after a few days of use: it counts
the times a program wanted a huge page and got normal ones.

### Two module settings in the boot image

Both save memory that is set aside at boot and never used. Check that each
applies to the machine first.

```sh
printf 'blacklist xe\noptions nvme io_queue_depth=128\n' | sudo tee /etc/modprobe.d/memory.conf
sudo limine-mkinitcpio
```

| Line | What it does | Saves | Leave it out when |
| --- | --- | --- | --- |
| `blacklist xe` | the kernel loads two drivers for an Intel GPU, `i915` and the newer `xe`, and only one drives it; this keeps `xe` and its helper modules out | about 5 MB | the GPU is driven by `xe` (`readlink /sys/class/drm/card*/device/driver` says so), or the machine has no Intel GPU |
| `options nvme io_queue_depth=128` | the SSD driver makes room for 1,023 requests under way in each of its queues, one queue per CPU thread; 128 each is still far more than a desktop uses | about 9 MB | the machine has no NVMe disk |

Both modules are loaded from the boot image, so the file has to go into
it: `limine-mkinitcpio` is CachyOS's command to rebuild it with the Limine
boot loader; elsewhere it is `sudo mkinitcpio -P`. After a reboot,
`lsmod | grep '^xe '` prints nothing and
`cat /sys/module/nvme/parameters/io_queue_depth` prints 128.

### Let the wired network chip sleep

On a laptop, a wired network controller with no cable in can stay fully
powered and keep the processor from sleeping deeply. On the G533ZW letting
it sleep took the draw of the idle laptop from 9.7 W to 8.3 W. To see
whether yours may sleep (`on` means it never does; no output, no such
controller):

```sh
for d in /sys/bus/pci/devices/*; do [ "$(cat $d/class)" = 0x020000 ] && cat $d/power/control; done
```

The rule, for any Ethernet controller on PCI:

```sh
echo 'ACTION=="add", SUBSYSTEM=="pci", ATTR{class}=="0x020000", TEST=="power/control", ATTR{power/control}="auto"' | sudo tee /etc/udev/rules.d/90-ethernet-pm.rules
```

It takes effect at the next boot. The controller wakes when a cable is
plugged in. Should a cable ever go unnoticed, delete the file and reboot.

### Tell applications to keep still

Programs that honour the desktop's "reduce motion" setting stop animating
for decoration. The Claude app's working animation alone had the screen
redrawn 64 times a second for as long as Claude worked, which cost 2 to
3 W on battery; with the setting it is redrawn 5 times a second:

```sh
gsettings set org.gnome.desktop.interface enable-animations false
```

It takes effect at once, in GTK programs and in Chromium and Electron ones,
and it is kept across logins. Scrolling in those programs may jump where it
used to glide. To undo it:
`gsettings reset org.gnome.desktop.interface enable-animations`.

## 4. Replacing KDE's parts

For a machine that came with KDE Plasma (or another desktop) and is to run
tw only. Three of the desktop's parts stay in memory in a tw session, or
start one that does: the keyring, the login manager, NetworkManager. Do
the steps in this order. Each can be undone, and each is tested before the
next one takes away the way back.

### The keyring: oo7

Programs such as the Claude app and VS Code need a keyring to stay signed
in. oo7 is a small one (8 MB in `btop`, 1.2 MB of its own) that your login
password unlocks as you log in. On a machine that came with KDE it takes
the place of the KDE wallet (`ksecretd`, 53 MB in `btop`), and takes the
wallet's entries over by itself.

```sh
sudo pacman -S oo7 libsecret
sudo sh tools/keyring.sh oo7
```

That changes `/etc/pam.d/login`: three lines for oo7 come in, and the KDE
wallet's go out if they are there. The old file is kept as
`/etc/pam.d/login.before-oo7`. The lock screen checks your password
through this same file, so test it at once, with your session still open:

1. Switch to a text console (Ctrl+Alt+F3), log in, type `exit`, and switch
   back. If the login is refused, `sudo sh tools/keyring.sh back` puts the
   file back. Do not log out or reboot before this login has worked.
2. Switch the running session over, which also checks that oo7 got your
   password from that login and can store a secret:

   ```sh
   sh tools/keyring.sh start
   ```

3. Lock the screen (Mod+Alt+L) and unlock it.
4. Quit a program that keeps a sign-in and open it again. If it asks you to
   sign in, do: it then keeps that in oo7.

`sh tools/keyring.sh` says which keyring is running and whether it is
open. To go back: `sudo sh tools/keyring.sh back`, then log out and in.
That only helps while the keyring you had before is still installed.

### Log in on the text console instead of a login manager

A login manager starts a whole compositor to ask for a password and then
stays in memory for the session. The kernel's text consoles do the same
job and leave nothing behind.

First let the console login unlock the keyring: that is the step above,
which also had you test a login on a second console.

Then try the whole thing once: log out, log in on a text console, and type
`tw-session`. A program that keeps a login should still have it.

Then switch the login manager off (`plasmalogin` is KDE's; `sddm` and `gdm`
are others):

```sh
sudo systemctl disable --now plasmalogin
```

From the next boot there is a text prompt: your name, your password, then
`tw-session`. `sudo systemctl enable --now plasmalogin` brings the login
screen back for as long as it is installed.

Log out of a console when you are done with it. One left logged in is a
shell anyone at the keyboard reaches with Ctrl+Alt+F3, lock screen or not.

Typed as `exec tw-session`, tw takes the login shell's place instead of
running under it. The shell then is not left waiting for the whole session
(fish: 13 MB in `btop`), and when tw exits, or crashes behind the lock
screen, the console is back at the login prompt and not at a shell. The
price: quitting tw logs you out. Not tried yet; plain `tw-session` is
always there.

### Wi-Fi through iwd instead of NetworkManager

NetworkManager and the `wpa_supplicant` it starts are two programs of
42 MB in `btop`, and NetworkManager fetches a page every five minutes to
see whether the internet is there. iwd does Wi-Fi by itself. Try it before
deciding:

```sh
sudo pacman -S iwd iw
sudo sh tools/iwd.sh try
```

That copies the Wi-Fi networks NetworkManager knows to iwd, passwords
included, stops NetworkManager, starts iwd, and waits up to a minute for
the internet to be reachable. The network is away for some seconds. If it
does not come back through iwd, or you interrupt the script, it starts
NetworkManager again by itself. Either way nothing lasts past a reboot:
NetworkManager is still what starts at boot.

If it says the internet is reachable through iwd, use the machine for a
while, then make it permanent, or go back:

```sh
sudo sh tools/iwd.sh keep
sudo sh tools/iwd.sh back
```

`sh tools/iwd.sh` says which of the two is running. With iwd, Wi-Fi is
handled with `iwctl` where it was `nmcli`:

| Command | What it does |
| --- | --- |
| `iwctl station wlan0 show` | the network you are on |
| `iwctl station wlan0 scan`, then `iwctl station wlan0 get-networks` | what is in range |
| `iwctl station wlan0 connect <name>` | joins a network; asks for its password the first time |
| `iwctl known-networks list` | the networks it remembers |
| `iwctl known-networks <name> forget` | forgets one |

iwd does nothing for the wired port, VPNs or mobile broadband. A cable
plugged in is ignored until something is set up for it (systemd-networkd
with one small file does it). On a machine that lives on a cable, keep
NetworkManager or set that up first.

### Removing KDE

With the three steps above done, nothing of KDE runs in a tw session.
Removing it frees about 1.1 GB of disk and no memory. Reboot once first and
see that the login, the Wi-Fi and your sign-ins are all there: with KDE and
NetworkManager gone, `tools/iwd.sh back` and `tools/keyring.sh back` have
nothing to go back to.

```sh
sudo pacman -D --asexplicit vulkan-tools mpv ttf-hack ttf-fira-sans
sudo pacman -Rns $(pacman -Qgq plasma) ark dolphin ffmpegthumbs filelight gwenview kate kcalc kdegraphics-thumbnailers kdialog kio-admin kio-extras konsole kwalletmanager partitionmanager signon-kwallet-extension haruna kde-gtk-config cachyos-kde-settings phonon-qt6-vlc kwallet-pam breeze-gtk cachyos-emerald-kde-theme-git cachyos-nord-kde-theme-git networkmanager networkmanager-openvpn wpa_supplicant
```

The package list is what one CachyOS KDE install had; pacman names any
that are not installed, and those come off the line. The first line keeps
four packages that came in with KDE and are of use without it:
`vulkan-tools`, `mpv`, and two fonts. The second shows what it will remove,
about 200 packages, and asks before it does. `bolt`, the daemon that lets
Thunderbolt devices in, goes with them: read
[Thunderbolt](#thunderbolt-a-dock-without-a-daemon) first if you use a
Thunderbolt dock.

| Goes | In its place |
| --- | --- |
| Dolphin | Thunar (`sudo pacman -S thunar`). Its settings daemon (`xfconfd`, 9 MB in `btop`) stays after the first use until you log out |
| Konsole, Kate | foot, your editor |
| Gwenview | nothing installed; `imv` is a small one |
| Spectacle | Print and Shift+Print (grim) |
| Haruna | mpv |
| Ark, KCalc, Filelight, the partition manager | nothing |
| NetworkManager, wpa_supplicant, OpenVPN | iwd; no VPN |

KDE Connect stays, with the KDE libraries it needs; add
`kdeconnect modemmanager modemmanager-qt` to the second line to remove it
too. `heaptrack`, if installed, keeps a few more of those libraries.

Left behind, for you to delete: NetworkManager's copies of your Wi-Fi
passwords (`sudo rm -r /etc/NetworkManager`), and the old wallet
(`rm -r ~/.local/share/kwalletd ~/.local/share/kwalletd.before-oo7`).
Programs drawn with GTK were told to use KDE's mouse pointer, which is
gone: `gsettings set org.gnome.desktop.interface cursor-theme Adwaita`
gives them the one tw itself shows.

## 5. Sleep

tw can suspend the machine after a time without input and when the battery
runs low. Both are off in `src/machines/default.h`, and stay off until the
machine has been seen to wake: one laptop did not, and had to be switched
off and on again ([g533zw.md](g533zw.md#sleep)).

Try it by hand first, with nothing unsaved open:

```sh
systemctl suspend
```

Press a key or the power button. If the machine comes back with its screen
and its network, give its file in `src/machines/` a time and a charge
(`suspend_after_seconds = 900`, `battery_suspend_percent = 5`), build and
install. A long job run as `systemd-inhibit --what=sleep <command>` holds
the idle suspend off.

If it does not come back, leave both at 0 and take sleep away from the two
things outside tw that still lead there: closing the lid (systemd does
that by itself) and `systemctl suspend` typed by hand.

```sh
sudo systemctl mask sleep.target suspend.target
```

Closing the lid then only locks the session. On battery the laptop runs
until it is empty: the warning at 10% is all there is, so plug in when it
shows. `sudo systemctl unmask sleep.target suspend.target` gives sleep
back.

## 6. After a system update, and when something is wrong

**tw does not start after a system update.** tw is built against the
system's libraries, and an update can replace one with a version of another
name (`libdisplay-info` does that about once a year). `tw-session` then
says so and the loader names the library. Log in on a text console and
build tw again from clean, in its source directory:

```sh
meson setup --wipe build-release
meson compile -C build-release
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

`--wipe` keeps the build's options. wlroots itself is pinned in the source
tree and built into tw, so an update never changes it under tw.

**Something else is wrong.**

| Symptom | Look at |
| --- | --- |
| tw exits or will not start | run `tw-session` from a text console: what it prints stays on screen. `tw -d` logs everything |
| a key does nothing | its program is not installed (the table in part 1); `tw -d` logs "not installed" |
| the session is locked and no lock screen comes | any key starts the locker again. If none comes, swaylock is failing: what it says is on the console tw was started from |
| Xwayland keeps running | [Keep the shell from starting Xwayland](#keep-the-shell-from-starting-xwayland) |
| the NVIDIA GPU will not go off | `tw-gpu status`, then `journalctl -b -u tw-gpu@off` names who has it open |
| a program does not stay signed in | `sh tools/keyring.sh` says whether a keyring is running and open |
| where the memory goes | `sh tools/mem.sh`, and [g533zw.md](g533zw.md#memory) on how to read it |
| what the battery draw is made of | `sh tools/power.sh` on battery; `sudo sh tools/power-root.sh` for the idle draw |
