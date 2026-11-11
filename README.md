# Lavender Shift

Forza Horizon 2 (Xbox 360) recompiled to run natively, on
[ShiftGlue](https://github.com/arcanite24/shiftglue-sdk), the SDK Pinyon
Shift (Forza Horizon 1) uses. One game version: the USA disc with title
update v2 applied. Performance comes first; see
[docs/APPROACH_RESEARCH.md](docs/APPROACH_RESEARCH.md) for the plan.

You need your own disc image and title update. Nothing from the game is in
this repository.

## Playing (Windows)

You need Windows 10 2004 or newer (x64), a CPU with AVX2 (Intel Haswell,
AMD Zen or newer), a Vulkan GPU, 30 GB free, your own Forza Horizon 2 USA
disc image and the title update v2 package.

1. Extract the release ZIP into a folder with a short path, for example
   `C:\Games\LavenderShift`.
2. Start `LavenderShiftLauncher.exe`, choose the disc image and the title
   update, and press **Set up**. The launcher downloads the pinned build
   tools and the prebuilt recompiler and runtime, extracts your files,
   translates the game's code and compiles it on your PC. Tick the
   Microsoft license box to let setup fetch Microsoft's C++ libraries
   directly (no administrator prompt); unticked, it installs the Visual
   Studio Build Tools if the PC has none. The first setup takes 15
   minutes to an hour, depending on the PC; it continues where it stopped
   if interrupted.
3. Press **Play**. Settings are in the game's F6 menu.

Saves and settings live in `user` beside the launcher. To update, extract
a newer release into the same folder and run setup again: it rebuilds what
changed and keeps your extracted files and saves.

## Building from a checkout (Windows)

`tools/setup-release.ps1` is what the launcher runs, and works the same in
a checkout:

```powershell
.\tools\setup-release.ps1 -IsoPath '<FH2 USA disc>.iso' -UpdatePath '<FH2 title update v2>.zip'
.\tools\launch.ps1
```

It provisions the tools pinned in `config/release-toolchain.json` into
`.local/toolchain` (`tools/provision-toolchain.ps1`), prepares ShiftGlue
(`tools/prepare-rexglue.ps1`), extracts the disc and update
(`tools/lavender_setup.py`, which accepts only the supported files),
generates the translation and builds (`tools/build.ps1`). Each step skips
work already done, so after code changes `.\tools\build.ps1` alone is
enough. A failure writes `.local/logs/setup-error.json`. Never delete
`.local` recursively if it holds junctions.

`tools/package-release.ps1` builds the release ZIP (the launcher plus a
`git archive` of HEAD) and refuses files that look like game data.

`config/analysis` holds the codegen facts: `default.toml` is carried from
Pinyon Shift's FH1 analysis, and `thunks-*.toml` lists function-pointer
targets that FH2's `.pdata` leaves out (`tools/find_missing_thunks.py`).
An `M3_TRACE indirect.unregistered` stop at runtime means one is still
missing.

`thirdparty/shiftglue-sdk` is a submodule on ShiftGlue `main`. On Windows,
libmspack's `cabextract/mspack` files are git symlinks that check out as
text; copy the real files from `thirdparty/libmspack/mspack` over them.
