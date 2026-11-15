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
    libinput pixman libdrm mesa libglvnd systemd-libs libdisplay-info hwdata
```

Programs tw starts or works with. None is required to build, but each key or
feature does nothing without its program:

```sh
sudo pacman -S foot fuzzel swaylock fnott wl-clip-persist xwayland-satellite \
    grim slurp brightnessctl wl-clipboard wireplumber power-profiles-daemon \
    libnotify \
    xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr socat
```

| Program | What it is for |
| --- | --- |
| foot | terminal (Mod+T) |
| fuzzel | launcher (Mod+Space, Mod+D) |
| swaylock | lock screen (Mod+Alt+L, lid close, idle) |
| fnott | notifications; D-Bus starts it when the first one arrives |
| wl-clip-persist | keeps what you copied when its program exits, started with the session |
| xwayland-satellite | X11 programs; started when one runs, stopped when the last has gone |
| grim, slurp | screenshots (Print, Shift+Print) |
| brightnessctl, wireplumber | brightness and volume keys |
| power-profiles-daemon | the power-saver CPU profile while on battery |
| libnotify | `notify-send`, for the low-battery warning |
| xdg-desktop-portal, -gtk, -wlr | file dialogs and screen capture (OBS, browsers) |
| socat | hands the session to the KDE wallet, see step 4 |

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

Programs such as the Claude app and VS Code need a keyring to stay signed in.
`tw-session` sets this up by itself when the login screen unlocks the KDE
wallet at login, which the Plasma login manager does when `kwallet` and
`kwallet-pam` are installed. That is the only arrangement that has been tried.
Without KDE's wallet, another keyring service such as gnome-keyring should
work, since programs are pointed at the standard keyring service and not at
KDE's.

To check: `busctl --user list | grep secrets` should show a line for
`org.freedesktop.secrets`.

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

First let the console unlock the keyring, as the login manager did:

```sh
sudo cp /etc/pam.d/login /etc/pam.d/login.orig
printf '%s\n' '-auth       optional     pam_kwallet5.so' \
    '-session    optional     pam_kwallet5.so auto_start force_run' |
    sudo tee -a /etc/pam.d/login
```

The lock screen checks your password through this same file, so test it
before anything else, with your session still open: press Ctrl+Alt+F3, log
in, type `exit`, and come back (Ctrl+Alt+F2, or F1). If the login is
refused, `sudo cp /etc/pam.d/login.orig /etc/pam.d/login` puts the file
back.

Then try the whole thing once: log out, log in on a text console, and type
`tw-session`. A program that keeps a login, such as the Claude app, should
still have it.

Then switch the login manager off:

```sh
sudo systemctl disable --now plasmalogin
```

From the next boot there is a text prompt: your name, your password, then
`tw-session`. KDE starts from there with
`/usr/lib/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland`,
the command its login-screen entry runs (not tried from a console).
`sudo systemctl enable --now plasmalogin` brings the login screen back.

Log out of a console when you are done with it. One left logged in is a
shell anyone at the keyboard reaches with Ctrl+Alt+F3, lock screen or not.

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
- There is no battery indicator. When the charge is down to 10% a
  notification says so, and it stays until you click it. At 5% the laptop
  locks and suspends, and it does so again for each further percent if you
  wake it without plugging in. It cannot hibernate (the swap is in RAM), so a
  suspended laptop still runs out in the end, only much later. The two
  thresholds are `battery_warn_percent` and `battery_suspend_percent` in
  [src/config.h](src/config.h); 0 turns either off.
- Keys, timeouts, monitor scale, autostart programs and the default terminal
  are all in [src/config.h](src/config.h).
