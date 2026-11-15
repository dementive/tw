# tw

A scrolling-tiling Wayland compositor. Windows sit side by side on a strip that
scrolls, as in niri. It is built on wlroots, and is configured by editing
[src/config.h](src/config.h) and rebuilding.

## Setting up a new machine

Written for Arch and CachyOS. Everything here was needed on the first install;
add to it when something new turns up.

### 1. Packages

To build:

```sh
sudo pacman -S meson ninja gcc pkgconf wayland wayland-protocols libxkbcommon \
    libinput pixman libdrm mesa libglvnd systemd-libs libdisplay-info hwdata \
    freetype2 fontconfig noto-fonts
```

The last three are for the notifications: their text is drawn from the
system's sans-serif font, at build time, so nothing font-related is needed
to run. Without them tw builds and `tw-notify` is left out.

Programs tw starts or works with. None is required to build, but each key or
feature does nothing without its program:

```sh
sudo pacman -S foot fuzzel swaylock wl-clip-persist xwayland-satellite \
    grim slurp brightnessctl wl-clipboard wireplumber power-profiles-daemon \
    libnotify iwd iw oo7 libsecret bluez bluez-utils \
    xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr thunar
```

| Program | What it is for |
| --- | --- |
| foot | terminal (Mod+T) |
| fuzzel | launcher (Mod+Space, Mod+D) |
| swaylock | lock screen (Mod+Alt+L, lid close, idle) |
| wl-clip-persist | keeps what you copied when its program exits, started with the session |
| xwayland-satellite | X11 programs; started when one runs, stopped when the last has gone |
| grim, slurp | screenshots (Print, Shift+Print) |
| brightnessctl, wireplumber | brightness and volume keys |
| power-profiles-daemon | the power-saver CPU profile while on battery |
| libnotify | `notify-send`, for the low-battery warning. What shows it is `tw-notify`, which comes with tw |
| iwd, iw | Wi-Fi without NetworkManager, see step 14 |
| oo7, libsecret | the keyring, see steps 4 and 15 |
| bluez, bluez-utils | Bluetooth (`bluetoothctl`), which is off until `tw-bluetooth on`, see step 17 |
| xdg-desktop-portal, -gtk, -wlr | file dialogs and screen capture (OBS, browsers) |
| thunar | file manager. Its settings daemon (`xfconfd`, 9 MB in `btop`, 0.6 MB its own) stays after the first use until you log out |

`vulkan-tools` is worth having too: step 6 uses its `vkcube` to check the
NVIDIA GPU.

### 2. Build and install

Meson downloads the pinned wlroots on the first `setup`.

```sh
meson setup build-release --buildtype=release -Db_lto=true
meson test -C build-release --suite tw
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

Run the last line again after every change. A running session keeps the old
binary until you log out and in again.

### 3. Log in

Log out and pick "tw" in the login screen's session list, or log in on a
text console and type `tw-session` (step 10 does away with the login screen
altogether). The screen is black
with a cursor until you open something: there is no wallpaper and no bar.
Mod is the Super key. Mod+Shift+E quits, and Ctrl+Alt+F3 is a text console if
something goes wrong.

### 4. Keyring

Programs such as the Claude app and VS Code need a keyring to stay signed
in. Here that is oo7: a small service that your login password unlocks as
you log in. Step 15 sets it up; do it before relying on a console login
(step 10). `tw-session` only tells those programs to use the standard
keyring service, whichever program provides it.

To check: `sh tools/keyring.sh` should say that the keyring is oo7-daemon
and that it is open.

### 5. Keep the shell from starting Xwayland

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

If the `grep` prints a `source /usr/share/cachyos-fish-config/cachyos-config.fish`
line, that file loads the plugin on its second line. Replace the `source` line
with the parts of that file you want to keep, leaving out the `done.fish` line.

Then close every terminal that was already open.

To find some other program doing this, look for X11 tools in your shell's
startup files: `grep -rn 'xprop\|xdotool\|xset\|xrdb' ~/.config/fish`.

### 6. The NVIDIA GPU: off until a program needs it

On a laptop with an Intel and an NVIDIA GPU, tw draws with the Intel one. The
NVIDIA GPU is switched off when you log in, unless a monitor is plugged into
one of its ports at that moment. Off means its driver is unloaded and the GPU
is taken off the PCI bus: it is powered down, the memory the driver keeps
(about 380 MB) is free, and nothing can switch it back on by accident.
Programs that only look at the GPU, such as `btop`, `fastfetch` and
`nvidia-smi`, see no NVIDIA GPU while it is off. Without that last part they
would load the driver again every time they run. Nothing has to be set up
for any of this.

A program gets the GPU in one of two ways:

- From the launcher, automatically, if its desktop entry asks for the
  discrete GPU with `PrefersNonDefaultGPU=true`. Steam and Blender ship that
  line. To give it to another program, copy its entry from
  `/usr/share/applications` to `~/.local/share/applications` and add the line.
- From a terminal or a script: `tw-gpu run <program>`. In Steam, a game's
  launch options can be `tw-gpu run %command%`, which keeps Steam itself on
  the Intel GPU.

The GPU is switched on before the program starts, which takes about two
seconds, and off when the program and everything it started have exited,
also if you close its terminal or press Ctrl-C. If something else still has
the GPU open at that point (`btop` watching it, for example), it goes off as
soon as that program has closed.

| Command | What it does |
| --- | --- |
| `tw-gpu run <program>` | runs a program on the NVIDIA GPU |
| `tw-gpu on` | switches it on and leaves it on until `tw-gpu off` |
| `tw-gpu off` | switches it off, or as soon as nothing uses it any more |
| `tw-gpu status` | says whether it is on, and whether it is powered |

After the first install, make the system read the new files:

```sh
sudo systemctl daemon-reload
sudo systemctl restart polkit
sudo udevadm control --reload
```

Then check it on the machine, because no test can:

```sh
tw-gpu run vkcube     # a spinning cube; close it
tw-gpu status         # a few seconds later: "off, powered down"
```

On the ROG Strix this is what happens. If on another machine the status says
"off, but its slot is still powered", the GPU did not power down once it was
off the bus, and it uses more battery that way than before. Run `tw-gpu on`
to go back to how it was, and look into it. If `tw-gpu on` ever fails to
bring the GPU back, a reboot does.

The HDMI port belongs to the NVIDIA GPU. A monitor on it works if it is
plugged in when you log in after a boot. If you have logged out and in since,
the GPU is still switched off and tw cannot see the monitor: run `tw-gpu on`,
then log out and in.

### 7. Services tw has no use for

Two services that CachyOS enables can go:

```sh
sudo systemctl disable --now ananicy-cpp.service
sudo systemctl disable --now switcheroo-control.service
```

| Service | What it is | Why it can go |
| --- | --- | --- |
| ananicy-cpp | CachyOS's daemon that adjusts process priorities by a rule list | about 7 MB, and with only a couple of programs running there is little for it to sort out |
| switcheroo-control | tells GNOME and KDE which GPU is the discrete one | `tw-gpu` does that job here |

Leave `avahi-daemon` alone if you use KDE Connect, which finds devices
through it.

### 8. The kernel's memory reserves

Of what `btop` and `free` call "used" right after login, about 285 MB is
free memory that the kernel holds back. Two settings shrink that to about
140 MB and take 170 MB off the figure, the per-CPU page caches that follow
it included:

```sh
printf 'vm.min_free_kbytes = 16384\nvm.lowmem_reserve_ratio = 1024 1024 32 0 0\n' |
    sudo install -Dm644 /dev/stdin /etc/sysctl.d/90-memory.conf
sudo sysctl --system
```

| Setting | What it is | What changing it costs |
| --- | --- | --- |
| `vm.min_free_kbytes` | the memory the kernel always keeps free. With huge pages on it picks 66 MB; 16 MB is what it picks without them on a 16 GB machine | huge pages stay on, but once the file cache has filled the RAM a newly started program more often gets normal pages first and huge ones a little later |
| `vm.lowmem_reserve_ratio` | how much of the first 4 GB is kept for devices that can only use that part: 58 MB by default, 14 MB with this | nothing found |

On a machine with a different amount of RAM, take the square root of
16 times the RAM in kB for the first value (22600 for 32 GB).

To check: `sh tools/mem.sh` prints the figure and what it is made of; the
"held back" line is the one these settings change. To see whether huge
pages suffer, look at `grep thp_fault_fallback /proc/vmstat` after a few
days of use: it counts the times a program wanted a huge page and got
normal ones. It was 0 before the change.

### 9. Two module settings in the boot image

Both save memory that is set aside at boot and never used here:

```sh
printf 'blacklist xe\noptions nvme io_queue_depth=128\n' | sudo tee /etc/modprobe.d/memory.conf
sudo limine-mkinitcpio
```

| Line | What it does | Saves |
| --- | --- | --- |
| `blacklist xe` | the kernel loads two drivers for the Intel GPU, `i915` and the newer `xe`, and only `i915` drives it on this generation; this keeps `xe` and its helper modules out | about 5 MB |
| `options nvme io_queue_depth=128` | the SSD driver makes room for 1,023 requests under way in each of its queues, one queue per CPU thread; 128 each is still far more than a laptop uses | about 9 MB |

Both modules are loaded from the boot image, so the file has to go into it:
`limine-mkinitcpio` is CachyOS's command to rebuild it with the Limine
boot loader; elsewhere it is `sudo mkinitcpio -P`. After a reboot,
`lsmod | grep '^xe '` prints nothing and
`cat /sys/module/nvme/parameters/io_queue_depth` prints 128. On a machine
whose Intel GPU is driven by `xe`
(`readlink /sys/class/drm/card*/device/driver` says so), leave the first
line out.

### 10. Log in on the text console instead of a login manager

A login manager starts a whole compositor to ask for a password and then
stays in memory for the session. The kernel's text consoles do the same job
and leave nothing behind. Do it in this order: each step is undone with one
command, and the login manager stays as the way back until the last one.

First let the console login unlock the keyring, as the login manager did:
that is step 15, which also has you test a login on a second console.

Then try the whole thing once: log out, log in on a text console, and type
`tw-session`. A program that keeps a login, such as the Claude app, should
still have it.

Then switch the login manager off (here it was KDE's, `plasmalogin`; `sddm`
and `gdm` are others):

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
(fish: 13 MB in `btop`, 2.5 MB of them its own), and when tw exits, or
crashes behind the lock screen, the console is back at the login prompt and
not at a shell. The price: quitting tw logs you out. Not tried on this
machine yet; plain `tw-session` is always there.

### 11. Let the wired network chip sleep

With no cable plugged in, the Ethernet controller still stays fully powered,
and on this laptop that alone keeps the processor from sleeping deeper than
its third level. Letting the controller sleep took the draw of the idle
laptop from 9.7 W to 8.3 W:

```sh
echo 'ACTION=="add", SUBSYSTEM=="pci", ATTR{class}=="0x020000", TEST=="power/control", ATTR{power/control}="auto"' | sudo tee /etc/udev/rules.d/90-ethernet-pm.rules
```

It takes effect at the next boot. To have it now as well:

```sh
echo auto | sudo tee /sys/bus/pci/devices/0000:2c:00.0/power/control
```

(`lspci -D | grep -i ethernet` gives the address on another machine.) The
controller wakes when a cable is plugged in. Should a cable ever go
unnoticed, delete the file and reboot.

### 12. Tell applications to keep still

Programs that honour the desktop's "reduce motion" setting stop animating
for decoration. The Claude app's working animation alone had the screen
redrawn 64 times a second for as long as Claude worked, which cost about
2 W; with the setting it is redrawn 5 times a second:

```sh
gsettings set org.gnome.desktop.interface enable-animations false
```

It takes effect at once, in GTK programs and in Chromium and Electron ones,
and it is kept across logins. Scrolling in those programs may jump where it
used to glide. To undo it:
`gsettings reset org.gnome.desktop.interface enable-animations`.

### 13. Notifications: remove the other service

Notifications are shown by `tw-notify`, which is installed with tw. The
session's D-Bus starts it when a program sends one and it exits when the last
one has closed, so it is in memory only while there is something to read.
Its entry is in `/usr/local`, which D-Bus reads before `/usr`, so it is
used from the next login even while fnott (or KDE's service) is installed.
fnott can go:

```sh
sudo pacman -Rns fnott
```

To check, after logging in again:

```sh
notify-send 'Hello' 'from tw-notify'
```

A box appears in the top right corner and goes after six seconds; a second
later `pgrep -x tw-notify` prints nothing.

### 14. Wi-Fi through iwd instead of NetworkManager

NetworkManager and the `wpa_supplicant` it starts are two programs of
42 MB in `btop` (5.7 MB of their own), and NetworkManager fetches a page
every five minutes to see whether the internet is there. iwd does Wi-Fi by
itself. Try it before deciding:

```sh
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
with one small file does it).

### 15. The keyring: oo7

oo7 keeps what programs store in the keyring, in 8 MB (1.2 MB of its own),
and your login password unlocks it. On a machine that came with KDE it
takes the place of the KDE wallet (`ksecretd`), which was the largest
helper in a tw session: 53 MB in `btop`, 8 MB of its own, and 25 MB of Qt
and KDE libraries that nothing else used. It takes the wallet's entries
over by itself.

```sh
sudo sh tools/keyring.sh oo7
```

That changes `/etc/pam.d/login`: three lines for oo7 come in, and the KDE
wallet's go out if they are there. The old file is kept as
`/etc/pam.d/login.before-oo7`. The lock screen checks your password through
this same file, so test it at once, with your session still open:

1. Press Ctrl+Alt+F3, log in, type `exit`, and come back with Ctrl+Alt+F1.
   If the login is refused, `sudo sh tools/keyring.sh back` puts the file
   back. Do not log out or reboot before this login has worked.
2. Switch the running session over, which also checks that oo7 got your
   password from that login and can store a secret:

   ```sh
   sh tools/keyring.sh start
   ```

3. Lock the screen (Mod+Alt+L) and unlock it.
4. Quit the Claude app and open it again. If it asks you to sign in, do: it
   then keeps that in oo7.

`sh tools/keyring.sh` says which keyring is running and whether it is open.
To go back: `sudo sh tools/keyring.sh back`, then log out and in. That
only helps while the keyring you had before is still installed.

### 16. Without KDE

With steps 10, 14 and 15 done, nothing of KDE runs in a tw session.
Removing it frees 1.1 GB of disk and no memory. Reboot once first and see
that the login, the Wi-Fi and your sign-ins are all there: with KDE and
NetworkManager gone, `tools/iwd.sh back` and `tools/keyring.sh back` have
nothing to go back to.

```sh
sudo pacman -D --asexplicit bolt vulkan-tools mpv ttf-hack ttf-fira-sans
sudo pacman -Rns $(pacman -Qgq plasma) ark dolphin ffmpegthumbs filelight gwenview kate kcalc kdegraphics-thumbnailers kdialog kio-admin kio-extras konsole kwalletmanager partitionmanager signon-kwallet-extension haruna kde-gtk-config cachyos-kde-settings phonon-qt6-vlc kwallet-pam breeze-gtk cachyos-emerald-kde-theme-git cachyos-nord-kde-theme-git networkmanager networkmanager-openvpn wpa_supplicant
```

The first line keeps five packages that came in with KDE and are of use
without it: `bolt` (lets Thunderbolt devices in), `vulkan-tools` (step 6),
`mpv`, and two fonts. The second shows what it will remove, about 200
packages, and asks before it does.

| Goes | In its place |
| --- | --- |
| Dolphin | Thunar (`sudo pacman -S thunar`) |
| Konsole, Kate | foot, your editor |
| Gwenview | nothing installed; `imv` is a small one |
| Spectacle | Print and Shift+Print (grim) |
| Haruna | mpv |
| Ark, KCalc, Filelight, the partition manager | nothing |
| NetworkManager, wpa_supplicant, OpenVPN | iwd (step 14); no VPN |

KDE Connect stays, with the KDE libraries it needs; add
`kdeconnect modemmanager modemmanager-qt` to the second line to remove it
too. `heaptrack`, if installed, keeps a few more of those libraries.

Left behind, for you to delete: NetworkManager's copies of your Wi-Fi
passwords (`sudo rm -r /etc/NetworkManager`), and the old wallet
(`rm -r ~/.local/share/kwalletd ~/.local/share/kwalletd.before-oo7`).
Programs drawn with GTK were told to use KDE's mouse pointer, which is
gone: `gsettings set org.gnome.desktop.interface cursor-theme Adwaita`
gives them the one tw itself shows.

### 17. Bluetooth: off until you ask for it

Installing tw is all the setup there is; if this is the first install with
`tw-bluetooth` in it, make the system read the new files as in step 6
(`daemon-reload`, restart polkit, reload udev). From the next boot on
Bluetooth is off, and stays off until you switch it on:

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
so systemd, which starts `bluetoothd` whenever there is one (whether its
radio is blocked or not, which is why it used to run all day), never does.
`bluetooth.service` stays enabled as it is; there is nothing to disable.
What that frees on the ROG Strix: `bluetoothd` (7 MB in `btop`, 0.6 MB its
own) and 1.6 MB of kernel modules. The radio was asleep already.

Check it on the machine, because no test can:

```sh
tw-bluetooth off
tw-bluetooth                  # Bluetooth: off
lsmod | grep -c bluetooth     # 0
tw-bluetooth on
bluetoothctl show | grep Powered    # Powered: yes
```

Three things to know. A mouse or keyboard that drops its connection to save
its own battery, and comes back when touched, finds Bluetooth off if it was
away for ten minutes: for one of those make the time longer (`OnActiveSec`
in [data/tw-bluetooth-idle.timer](data/tw-bluetooth-idle.timer)), or build
with `meson configure build-release -Dbluetooth=false`, which leaves
Bluetooth as the distribution set it up. An adapter that is not on USB
needs its driver named in [data/tw-bluetooth.conf](data/tw-bluetooth.conf).
And if `tw-bluetooth on` ever fails, `journalctl -b -u tw-bluetooth@on`
says why, and `sudo modprobe btusb` does by hand what it does.

## Good to know

- Monitor ports wired to the laptop's first GPU work whenever you plug in.
  Ports wired to the second GPU only work if the monitor is connected when you
  log in; see step 6. On the ROG Strix the Intel GPU has two DisplayPort
  outputs (most likely the USB-C ports) and the NVIDIA GPU has HDMI and one
  DisplayPort.
- Screen readers and other accessibility aids find no programs to talk to:
  the session tells toolkits to leave the accessibility bus alone, which
  saves two background programs. Log in with `TW_A11Y=1` set to keep it.
- The laptop suspends after 15 minutes without input, also on mains power. Run
  a long job as `systemd-inhibit --what=sleep <command>` to hold that off.
- On battery every screen runs at 60 Hz and the CPU is set to its
  power-saver profile; plugging the power cable in brings back the full
  refresh rate and the balanced profile. The screen may blink once as it
  changes. Both are settings in [src/config.h](src/config.h):
  `battery_refresh_hz` (0 keeps the full rate) and the two commands below it.
  To use another profile until the cable next moves, run
  `powerprofilesctl set performance` (or `balanced`, `power-saver`).
- A notification is a title and up to four lines of text, in the top right
  corner, above a fullscreen window too. Clicking one closes it; a left
  click also tells its program, which can then bring its window forward.
  One marked urgent has a red border and stays
  until clicked. There are no icons, buttons or sounds, and only Latin
  letters and common punctuation can be shown: anything else, an emoji for
  one, appears as a small box. Size, colours and how long one stays are in
  [notify/config.h](notify/config.h); the font is picked when tw is built
  (`meson configure build-release -Dnotify_font='DejaVu Sans'`).
- There is no battery indicator. When the charge is down to 10% a
  notification says so, and it stays until you click it. At 5% the laptop
  locks and suspends, and it does so again for each further percent if you
  wake it without plugging in. It cannot hibernate (the swap is in RAM), so a
  suspended laptop still runs out in the end, only much later. The two
  thresholds are `battery_warn_percent` and `battery_suspend_percent` in
  [src/config.h](src/config.h); 0 turns either off.
- Keys, timeouts, monitor scale, autostart programs and the default terminal
  are all in [src/config.h](src/config.h).
