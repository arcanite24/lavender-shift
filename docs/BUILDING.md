# Building from a checkout (Windows)

Players don't need this page: the launcher in the
[latest release](https://github.com/arcanite24/lavender-shift/releases/latest)
does all of it. This is for working on Lavender Shift itself.

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

## Releases and codegen

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
