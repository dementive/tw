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
    grim slurp brightnessctl wl-clipboard wireplumber \
    xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr socat
```

| Program | What it is for |
| --- | --- |
| foot | terminal (Mod+T) |
| fuzzel | launcher (Mod+Space, Mod+D) |
| swaylock | lock screen (Mod+Alt+L, lid close, idle) |
| fnott | notifications, started with the session |
| wl-clip-persist | keeps what you copied when its program exits, started with the session |
| xwayland-satellite | X11 programs; started only when one runs |
| grim, slurp | screenshots (Print, Shift+Print) |
| brightnessctl, wireplumber | brightness and volume keys |
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

Log out and pick "tw" in the login screen's session list. The screen is black
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

tw starts Xwayland only when a program talks to X11, and Xwayland then stays
in memory (about 30 MB) for the rest of the session. A program that merely
asks X11 a question is enough, and CachyOS's fish setup ships one: the `done`
plugin runs `xprop` every time a shell starts and around every command. Its
notifications do not work under Wayland anyway, so remove it.

To check: open a terminal, then run `pgrep -x Xwayland`. If that prints a
number although you have no X11 program open, something woke it.

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

Then close every terminal that was already open, and free the memory with
`pkill xwayland-satellite`. tw starts it again when an X11 program needs it.

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

## Good to know

- Monitor ports wired to the laptop's first GPU work whenever you plug in.
  Ports wired to the second GPU only work if the monitor is connected when you
  log in; see step 6. On the ROG Strix the Intel GPU has two DisplayPort
  outputs (most likely the USB-C ports) and the NVIDIA GPU has HDMI and one
  DisplayPort.
- The laptop suspends after 15 minutes without input, also on mains power. Run
  a long job as `systemd-inhibit --what=sleep <command>` to hold that off.
- Keys, timeouts, monitor scale, autostart programs and the default terminal
  are all in [src/config.h](src/config.h).
