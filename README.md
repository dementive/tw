# tw

A scrolling-tiling Wayland compositor. Windows sit side by side on a strip
that scrolls, as in niri; the code is small, as in dwl. It is built on
wlroots and configured by editing [src/config.h](src/config.h) and
rebuilding.

What it is built for, in this order: no work when idle, low memory, low
latency, small code. An idle tw does not wake up at all, a window nobody
can see is told to stop drawing, and everything that is not the
compositor's job (bar, launcher, lock screen, X11) is another program that
runs only while it is needed.

## Install

On CachyOS or Arch, with the packages listed in
[docs/setup.md](docs/setup.md#packages):

```sh
meson setup build-release --buildtype=release -Db_lto=true
meson test -C build-release --suite tw
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

Then pick "tw" at the login screen, or log in on a text console and type
`tw-session`. To try it in a window first: `./build-release/tw -s foot`
(Mod is Alt there).

[docs/setup.md](docs/setup.md) is the whole of a new machine: packages,
the machine's own settings, what to check after installing, and the
optional steps that make the system around tw small.

## Keys

Mod is the Super key. I, J, K, L are up, left, down, right, and the arrow
keys do the same.

| Keys | What they do |
| --- | --- |
| Mod+T | terminal |
| Mod+Space, Mod+D | launcher |
| Mod+Q | close the window |
| Mod+J / Mod+L | focus the column on the left / right |
| Mod+I / Mod+K | focus the window above / below in the column |
| Mod+Ctrl+direction | move the focused column (left, right) or window (up, down) |
| Mod+Shift+direction | go to the monitor on that side; with none there, the previous (up, left) or next (down, right) workspace |
| Mod+Shift+Ctrl+direction | the same, taking the focused column along |
| Mod+Home / Mod+End | focus the first / last column; with Ctrl, move the column there |
| Mod+1 … Mod+9 | show that workspace; with Ctrl, take the focused column there |
| Mod+Page Up / Page Down | previous / next workspace; with Ctrl, take the column along |
| Mod+R | cycle the column's width: a third, half, two thirds |
| Mod+F | column at full width |
| Mod+Shift+F | fullscreen |
| Mod+[ / Mod+] | a lone window joins the column on that side; a window sharing a column leaves it |
| Mod+, / Mod+. | pull the window on the right into this column / put this window into a column of its own |
| Mod+Alt+L | lock |
| Mod+Shift+P | screen off until the next input |
| Print / Shift+Print | screenshot of the screen / of a region |
| Mod+Shift+E | quit |
| Ctrl+Alt+F1 … F12 | text consoles |

A three-finger swipe sideways drags the strip; up or down switches
workspace. The volume and brightness keys work.

## Configuring

Everything is compile-time: edit, `meson compile -C build-release`, install.

| What | Where |
| --- | --- |
| Keys, gaps, column widths, timeouts, the terminal and other programs | [src/config.h](src/config.h) |
| Screens (scale, position) and whether the machine may sleep | [src/machines/](src/machines/), one file per machine; the build picks the one named after the machine's board |
| Notifications: size, colours, how long they stay | [notify/config.h](notify/config.h); the font with `-Dnotify_font='DejaVu Sans'` |
| Whether Bluetooth is kept off, and the Thunderbolt rule | build options `-Dbluetooth`, `-Dthunderbolt` |
| Programs to start with the session, without a rebuild | `~/.config/tw/autostart`, an executable script |

`tw -h` lists its options: `-d` logs what it does, `-b`, `-z` and `-x`
override the screen-off, suspend and X11 timeouts.

## What tw does by itself

- **Screen and lock.** The screen goes off after 5 minutes without input
  and the session locks; closing the lid locks too. A window that inhibits
  idling (a playing video) holds the screen on while it is visible.
- **Sleep.** Off until the machine has been seen to wake from one; see
  [setup.md](docs/setup.md#5-sleep).
- **On battery** every screen runs at 60 Hz and the machine is set to its
  power-saver profile; plugging in brings back the full rate and the
  balanced profile. The screen may blink once as it changes. There is no
  battery indicator: at 10% a notification says so, and stays until
  clicked.
- **The clipboard** keeps what you copied after the program you copied it
  from has closed, up to 64 MB. A program that empties the clipboard on
  purpose, as a password manager does, is obeyed.
- **X11 programs** work. Xwayland is started when one runs and stopped half
  a minute after the last has gone.
- **Notifications** appear top right, above fullscreen windows too: a title
  and up to four lines. A click closes one; a left click also tells its
  program, which can bring its window forward. An urgent one has a red
  border and stays until clicked. No icons, buttons or sounds, and only
  Latin letters and common punctuation: anything else shows as a small box.
- **Screen readers** find no programs to talk to: the session keeps the
  accessibility bus off, which saves three background programs. Log in
  with `TW_A11Y=1` set to keep it.
- **Several monitors** each show one workspace. Unplugging one only hides
  its workspace; no window is moved.

Commands that come with it:

| Command | What it does |
| --- | --- |
| `tw-gpu run <program>` | runs a program on a laptop's NVIDIA GPU, which is otherwise off; `tw-gpu status`, `on`, `off` |
| `tw-bluetooth on` | switches Bluetooth on, which is otherwise off; `tw-bluetooth off`, and alone to say which it is |
| `tw-power saver`, `balanced`, `performance` | sets the power profile until the power cable next moves; alone, says which is set |

## After a system update

wlroots and the one library under it whose name changes from release to
release are built into tw, so an update should not stop it starting. The
rest of what it links has kept its names for years. If it ever does not
start after an update, `tw-session` says so; build it again from clean:

```sh
meson setup --wipe build-release
meson compile -C build-release
sudo meson install -C build-release --no-rebuild --skip-subprojects
```

## More

| | |
| --- | --- |
| [docs/setup.md](docs/setup.md) | a new machine, start to finish, and what to do when something is wrong |
| [docs/design.md](docs/design.md) | how tw works, for changing it |
| [docs/testing.md](docs/testing.md) | what the tests cover, what they cannot, what has been seen on real hardware |
| [docs/g533zw.md](docs/g533zw.md) | the laptop tw was written on: memory and battery measurements, decisions |
| [CLAUDE.md](CLAUDE.md) | the rules a change has to keep |
