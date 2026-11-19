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
the machine's own settings, and what to check after installing.
[docs/system.md](docs/system.md) has the optional steps that make the
system around tw small.

## Keys

Mod is the Super key. I, J, K, L are up, left, down, right, and the arrow
keys do the same.

| Keys | What they do |
| --- | --- |
| Mod+T | terminal |
| Mod+Space, Mod+D | launcher |
| Mod+Q | close the window |
| Mod+J / Mod+L | focus the column on the left / right; past the end of the strip, the monitor on that side |
| Mod+I / Mod+K | focus the window above / below in the column |
| Mod+Ctrl+direction | move the focused column (left, right) or window (up, down); a column past the end of the strip goes on to the monitor on that side |
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
| Mod+left-drag | pick up a window and let it go on another monitor, or beside another column |
| Ctrl+Alt+F1 … F12 | text consoles |

A three-finger swipe sideways drags the strip; up or down switches
workspace. The volume and brightness keys work.

## Configuring

Everything is compile-time: edit, `meson compile -C build-release`, install.

| What | Where |
| --- | --- |
| Keys, gaps, column widths, timeouts, the terminal and other programs | [src/config.h](src/config.h) |
| Screens (scale, position) and whether the machine may sleep | [src/machines.h](src/machines.h), an entry per machine; tw picks the one named after the machine's board as it starts |
| Notifications: font, size, colours, how long they stay | [notify/config.h](notify/config.h) |
| Whether Bluetooth is kept off, and the Thunderbolt rule | build options `-Dbluetooth`, `-Dthunderbolt` |
| Programs to start with the session, without a rebuild | `~/.config/tw/autostart`, an executable script |

`tw -h` lists its options: `-d` logs what it does, `-b`, `-z` and `-x`
override the screen-off, suspend and X11 timeouts.

## What tw does by itself

- **Screen and lock.** The screen goes off after 5 minutes without input
  and the session locks; closing the lid locks too. A window that inhibits
  idling (a playing video) holds the screen on while it is visible.
- **Sleep.** Off until the machine has been seen to wake from one; see
  [setup.md](docs/setup.md#3-sleep).
- **On battery** every screen runs at 60 Hz and the machine is set to its
  power-saver profile; plugging in brings back the full rate and the
  balanced profile. The screen may blink once as it changes. There is no
  battery indicator: at 10% a notification says so, and stays until
  clicked.
- **The clipboard** keeps what you copied after the program you copied it
  from has closed, up to 64 MB. A program that empties the clipboard on
  purpose, as a password manager does, is obeyed.
- **X11 programs** work. Xwayland is started when one runs and stopped
  about half a minute after the last X11 window closes.
- **Notifications** appear top right, above fullscreen windows too: a title
  and up to four lines. A click closes one; a left click also tells its
  program, which can bring its window forward. An urgent one has a red
  border and stays until clicked. No icons, buttons, sounds or emoji.
- **Screen readers** find no programs to talk to: the session keeps the
  accessibility bus off, which saves three background processes. Log in
  with `TW_A11Y=1` set to keep it.
- **Several monitors** each show one workspace. Unplugging one hides its
  workspace, or moves it to another monitor if it was the one in use; no
  window is moved.

Commands that come with it:

| Command | What it does |
| --- | --- |
| `tw-gpu run <program>` | runs a program on a laptop's NVIDIA GPU, which is otherwise off; `tw-gpu status`, `on`, `off` |
| `tw-bluetooth on` | switches Bluetooth on, which is otherwise off; `tw-bluetooth off`, and alone to say which it is |
| `tw-power saver`, `balanced`, `performance` | sets the power profile until the power cable next moves or tw next starts; alone, says which is set |

## After a system update

wlroots and libdisplay-info, the two libraries under tw whose names change
from release to release, are built into it, so an update should not stop
it starting. If it ever does, `tw-session` says so, and
[setup.md](docs/setup.md#4-after-a-system-update-and-when-something-is-wrong)
has the rebuild.

## More

| | |
| --- | --- |
| [docs/setup.md](docs/setup.md) | a new machine, start to finish, and what to do when something is wrong |
| [docs/system.md](docs/system.md) | optional: a small CachyOS around tw, and replacing KDE's keyring, login and Wi-Fi |
| [docs/design.md](docs/design.md) | how tw works, for changing it |
| [docs/testing.md](docs/testing.md) | what the tests cover, what they cannot, what has been seen on real hardware |
| [docs/g533zw.md](docs/g533zw.md) | the laptop tw was written on: memory and battery measurements, decisions |
| [CLAUDE.md](CLAUDE.md) | the rules a change has to keep |

## License

MIT; see [LICENSE](LICENSE). The protocol files in `protocols/` keep the
licenses written at their top.
