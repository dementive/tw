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

## Good to know

- Monitor ports wired to the laptop's first GPU work whenever you plug in.
  Ports wired to the second GPU only work if the monitor is connected when you
  log in. On the ROG Strix the Intel GPU has two DisplayPort outputs (most
  likely the USB-C ports) and the NVIDIA GPU has HDMI and one DisplayPort.
- The NVIDIA driver keeps about 380 MB of memory for as long as it is loaded,
  even while its GPU is asleep and nothing uses it. It belongs to no program,
  so it only shows in the total. Measured by unloading the driver:
  `sudo systemctl stop nvidia-powerd`, then
  `sudo modprobe -r nvidia_drm nvidia_uvm nvidia_modeset nvidia`; a reboot
  brings it back.
- The laptop suspends after 15 minutes without input, also on mains power. Run
  a long job as `systemd-inhibit --what=sleep <command>` to hold that off.
- Keys, timeouts, monitor scale, autostart programs and the default terminal
  are all in [src/config.h](src/config.h).
