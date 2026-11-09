# Lavender Shift

Forza Horizon 2 (Xbox 360) recompiled to run natively, on
[ShiftGlue](https://github.com/arcanite24/shiftglue-sdk), the SDK Pinyon
Shift (Forza Horizon 1) uses. One game version: the USA disc with title
update v2 applied. Performance comes first; see
[docs/APPROACH_RESEARCH.md](docs/APPROACH_RESEARCH.md) for the plan.

You need your own disc image and title update. Nothing from the game is in
this repository.

## Stage 0 reference build (Windows)

This is the measuring baseline: FH2 on today's ShiftGlue with its existing
PM4 path.

1. Link the toolchain. `.local/toolchain` is a junction to Pinyon Shift's
   pinned toolchain (LLVM 20.1.8, CMake 3.31.10, Ninja 1.13.2). The Visual
   Studio Build Tools provide the Windows SDK and CRT. Never delete `.local`
   recursively: it follows the junction.
2. Lay out the game:

   ```powershell
   $py = '.local/toolchain/python-3.13.15/python.exe'
   & $py -I tools/lavender_setup.py disc   '<FH2 USA disc>.iso'
   & $py -I tools/lavender_setup.py update '<FH2 title update v2>.zip'
   & $py -I tools/lavender_setup.py codegen
   ```

3. Build and run:

   ```powershell
   .\tools\build.ps1
   .\tools\launch.ps1
   ```

`config/analysis` holds the codegen facts: `default.toml` is carried from
Pinyon Shift's FH1 analysis, and `thunks-*.toml` lists function-pointer
targets that FH2's `.pdata` leaves out (`tools/find_missing_thunks.py`).
An `M3_TRACE indirect.unregistered` stop at runtime means one is still
missing.

`thirdparty/shiftglue-sdk` is a submodule on ShiftGlue `main`. On Windows,
libmspack's `cabextract/mspack` files are git symlinks that check out as
text; copy the real files from `thirdparty/libmspack/mspack` over them.
