# macOS (Apple silicon)

Lavender Shift builds and runs natively on Apple silicon Macs, set up from
a terminal. There is no Mac launcher yet. The game's Vulkan renderer runs
on Metal through MoltenVK, which ShiftGlue builds in. As on Windows, you
bring your own disc image and title update, and the game is translated and
compiled on your Mac.

## Requirements

- An Apple silicon Mac with macOS 13.3 or newer. Intel Macs are not
  supported.
- Your FH2 USA disc image and title update v2 (see the
  [README](../README.md#what-you-need)).
- About 30 GB free: 7.2 GB of game files, ShiftGlue's source, about 350 MB
  of translated code, and the build.
- Apple's Command Line Tools (`xcode-select --install`), which bring
  `clang`, `git` and Python 3.9.
- CMake and Ninja: `brew install cmake ninja`.

## From a release

Download `LavenderShift-<version>-macos-arm64.tar.gz` from the
[latest release](https://github.com/arcanite24/lavender-shift/releases/latest),
then in Terminal:

```bash
cd ~/Downloads
tar -xzf LavenderShift-<version>-macos-arm64.tar.gz
cd LavenderShift-<version>-macos-arm64
python3 tools/lavender_mac.py setup --iso ~/Downloads/<FH2 USA disc>.iso \
    --update ~/Downloads/<title update v2>.zip
python3 tools/lavender_mac.py run
```

`setup` checks the tools, fetches ShiftGlue at the revision the release
pins (`config/release-toolchain.json`), extracts the disc and update (only
the supported files are accepted), builds the translator, translates the
game, compiles it and prepares the pipelines. The first setup takes about
half an hour on an M4 Pro, most of it compiling. It keeps the Mac awake
(`caffeinate`), and each step logs to `.local/logs`. After that, `run` is
all you need. Saves and settings live in `.local/user` in that folder.

To update, extract the newer release over the same folder (`tar -xzf` into
the folder's parent) and run `setup` again without `--iso` and `--update`:
it reuses the extracted files and rebuilds only what changed.

## From a checkout

```bash
git clone --recursive https://github.com/arcanite24/lavender-shift.git
cd lavender-shift
python3 tools/lavender_mac.py setup --iso <disc>.iso --update <title update>.zip
```

In a checkout, `setup` uses the submodule instead of fetching ShiftGlue.
The steps also run on their own: `tools/lavender_setup.py disc`, `update`
and `codegen`, then `lavender_mac.py doctor` and `build`. `build` makes the
translator (`rexglue`, in the submodule), translates the game into
`.local/generated` and compiles it with the `mac-arm64-release` preset. A
tree translated on another machine is translated again, because the
generator records absolute paths. After code changes, `build --step game`
is enough.

`run` prepares the ahead-of-time pipelines from your files, as
`tools/launch.ps1` does on Windows, then starts
`out/build/mac-arm64-release/lavender_shift`. Game flags go after `--`.
Started on its own, the game finds `.local/game` and `.local/user` from its
own folder, the same as on Windows. Only start one copy at a time: `run`
checks, but the game itself does not lock its user folder on macOS.

## Keys

The F-keys work, but on a Mac keyboard they are media keys unless you hold
`fn`. Each one also has a Mac shortcut:

| Action | Keys |
| --- | --- |
| Settings | F6, Cmd+, |
| Photo | F8, Cmd+Shift+P |
| Hide the HUD | F2, Cmd+Shift+H |
| Fullscreen | F11, Ctrl+Cmd+F |
| Quit | Cmd+Q |

## Controllers

The build places SDL_GameControllerDB (`thirdparty/sdl-gamecontrollerdb`,
plus Lavender's own lines in `config/gamecontrollerdb-extra.txt`) beside the
game as `gamecontrollerdb.txt`, and the game loads it from there. Pads SDL
knows by itself (Xbox Wireless, DualShock 4, DualSense, Switch Pro, 8BitDo
pads in their HID modes or on their dongles) work without it.

Pads that SDL opens only as a plain joystick need the database. One example
is the 8BitDo Ultimate 2C Wired in its HID mode (USB `2dc8:301d`): without
the database it has no layout, and with it it is a full gamepad
(checked on the test Mac).

A wired pad in Xbox 360 (XInput) mode does not work on a Mac: it is not a
HID device and macOS has no driver for it (Steam brings its own). The
Ultimate 2C Wired can come up in that mode (`2dc8:310a`, nothing in the
game's log); switch it to its other mode as 8BitDo's manual describes, or
use a pad over Bluetooth or a wireless dongle.

## Where things live

```text
.local/game/       the extracted disc, update and the modules codegen reads
.local/generated/  the translated code
.local/user/       saves, settings (config), logs, crash reports, photos
.local/logs/       build logs
```

## What is different on a Mac

- **Physical memory.** Host pages are 16 KB on Apple silicon, so ShiftGlue
  maps the guest's physical view (0xE0000000 and up) 4 KB further on. Code
  in `src/` that reads guest memory directly goes through
  `rex::memory::detail::PhysicalHostOffset`, or `GuestPtr`, as the
  generated code does.
- **Display rate.** DISPLAY reads the primary display's refresh rate from
  SDL (`src/frame_rate.cpp`).
- **First-run defaults.** Apple GPUs are integrated, so a new settings file
  starts at LOW-SPEC 60, as on any integrated GPU. A Mac-specific rule waits
  for a free-roam measurement.
- **Checks over ssh.** macOS does not let an ssh session capture the
  screen or send key presses. `LAVENDER_SHIFT_PHOTO_AT=20,50` makes the game
  save photos to `.local/user/photos` at those seconds after start, through
  its own photo path, so a remote check needs no permission changes.

## Status

| System | Status |
| --- | --- |
| Mac mini, M4 Pro, macOS 26.3 (2026-11-18) | Builds with the Command Line Tools' clang and Python 3.9.6. A clean game build took 13 minutes (shared CPU). It boots to the title screen and holds 60 fps there at LOW-SPEC 60 (120 Hz display detected). The pipeline step works. Driving and free roam: not checked yet. |
| Other Apple silicon Macs | Not tested |

The disc and update extraction (`lavender_setup.py disc` and `update`) have
not been run on a Mac yet. The test Mac used files extracted on Windows,
without their catalogs. `lavender_setup.py codegen` and `status` ran there
on Python 3.9.6.
