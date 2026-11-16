# A lean CachyOS around tw

None of this is needed to run tw; [setup.md](setup.md) is. These are the
steps that made the rest of one laptop small and quiet, for any machine
that is to run tw and little else. Part 1 is a set of independent steps:
take the ones that fit. Part 2 replaces the parts of KDE that stay in
memory, in order, each tested before the next one takes away the way back.

1. [Services, the shell, the kernel](#1-services-the-shell-the-kernel)
2. [Replacing KDE's parts](#2-replacing-kdes-parts): keyring, login, Wi-Fi,
   then KDE itself

Figures such as "7 MB in `btop`" were measured on that laptop
([g533zw.md](g533zw.md)) and will differ a little elsewhere.

## 1. Services, the shell, the kernel

Each step stands on its own and says what it saves.

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
free memory that the kernel holds back and nothing uses: 285 MB on a 16 GB
machine with huge pages on ([g533zw.md](g533zw.md#memory) says how to read
it off `/proc/meminfo`). Two settings shrink that to about 140 MB and took
170 MB off the figure:

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

## 2. Replacing KDE's parts

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

The login unlocks it through `/etc/pam.d/login`, the file a console login
reads and the lock screen checks your password with. A mistake there locks
you out of both, so keep a copy and test before you log out:

```sh
sudo pacman -S oo7 libsecret
sudo cp -p /etc/pam.d/login /etc/pam.d/login.before-oo7
sudoedit /etc/pam.d/login
```

Delete any line with `pam_kwallet5.so`, and add these three at the end:

```
-auth       optional     pam_oo7.so
-session    optional     pam_oo7.so auto_start
-password   optional     pam_oo7.so
```

Then have every login start oo7, and test, with your session still open:

```sh
sudo systemctl --global enable oo7-daemon.service
```

1. Switch to a text console (Ctrl+Alt+F3), log in, type `exit`, and switch
   back. If the login is refused, put the file back with
   `sudo cp -p /etc/pam.d/login.before-oo7 /etc/pam.d/login`. Do not log
   out or reboot before this login has worked.
2. Log out and in again. Then `secret-tool store --label=test tw test`,
   type anything, and `secret-tool lookup tw test` prints it back: oo7 is
   open. `secret-tool clear tw test` removes it.
3. Lock the screen (Mod+Alt+L) and unlock it.
4. A program that keeps a sign-in may ask for it once more; it then keeps
   it in oo7.

To go back: put the old file back as in step 1,
`sudo systemctl --global disable oo7-daemon.service`, and log out and in.

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
see whether the internet is there. iwd does Wi-Fi by itself, gets its own
addresses, and hands DNS to systemd-resolved, which CachyOS runs already
(`systemctl is-active systemd-resolved` says `active`).

Do this from a terminal you can stay at: the network is away until iwd has
joined. iwd does not read NetworkManager's networks, so have the Wi-Fi
password at hand.

```sh
sudo pacman -S iwd iw
printf '[General]\nEnableNetworkConfiguration=true\n' | sudo install -Dm644 /dev/stdin /etc/iwd/main.conf
sudo systemctl disable --now NetworkManager
sudo systemctl enable --now iwd
iwctl station wlan0 scan
iwctl station wlan0 connect <network>
```

`iwctl station list` names the card if it is not `wlan0`. Once joined,
iwd remembers the network and joins it by itself from then on. To go
back:

```sh
sudo systemctl disable --now iwd
sudo iw phy phy0 interface add wlan0 type managed
sudo systemctl enable --now NetworkManager
```

The middle line is needed because iwd replaces the card's network
interface with its own, which goes when iwd stops.

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
NetworkManager gone, there is nothing to go back to.

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
[Thunderbolt](setup.md#thunderbolt-a-dock-without-a-daemon) first if you use a
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
