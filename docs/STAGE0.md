# Stage 0: reference build results

The reference build is Forza Horizon 2 (USA disc, title update v2 applied
while loading) recompiled on ShiftGlue `101ad468` with its existing PM4 path
(`rexgpu-fh1`, Vulkan). It is the baseline every later stage is measured
against; see [APPROACH_RESEARCH.md](APPROACH_RESEARCH.md#stage-0-measure-before-building) for
the plan and the probes.

## Status (2026-11-09)

| Platform | State |
| --- | --- |
| Windows (multivac) | Boots to the title, first-run video setup, the intro movie and in-engine cutscene, and the opening drive ("Drive to the Festival"): driving, HUD, crowds, motion blur and depth of field render. No race reached yet. |
| Odin 2 Portal | Not started |
| Steam Deck | Not started |

## What the reference build needed

Each item is in this repository or the ShiftGlue submodule:

- **Function boundaries for indirect-call targets.** FH2's `.pdata` leaves
  out short stubs that are only reached through pointers:
  - static-initialiser stubs in the CRT init tables (the first boot stopped
    on `832453C8`);
  - `this`-adjusting thunks (the second boot stopped on `831B1760`, a vtable
    slot).

  `config/analysis/thunks-*.toml` lists them: 155 in `default.xex`, 15 in
  XMediaFacade, 79 in SpeechFacade. `tools/find_missing_thunks.py` finds
  them. It runs Pinyon Shift's finder with two fixes:
  - a branch onto a `.pdata` function counts as a tail call;
  - import thunks count as registered, because an entry for one breaks the
    facade link.
- **GPU plugin.** The app selects `rexgpu-fh1` in `OnPreSetup`. Without it
  the runtime starts with no GPU emulation.
- **Vulkan backend.** On Windows the SDK defaults `REXGLUE_USE_VULKAN` to
  off, so the preset turns it on. SPIRV-Tools and glslang need Python at
  configure time, so `tools/build.ps1` pins the toolchain's interpreter.
- **`XamXStudioRequest`.** FH2 tries XStudio first and falls back to the
  older call (`PsCamDeviceRequest`, now reached) when it gets a failure
  HRESULT. ShiftGlue gained the cvar `xam_xstudio_request_unsupported`
  (default off, which keeps FH1 unchanged), and `tools/launch.ps1` turns it
  on. fh2-recompiled found the garage hang this avoids.
- **Perf CSV.** The build defines `REXGLUE_ENABLE_PERF_COUNTERS`, and the app
  opens `--perf_log_csv` itself, as Pinyon's app does.

## First measurements

These come from the opening drive, standing at the start line. Machine:
multivac, a Ryzen 7 5800X and an RTX 4080 driving a 3840x2160 window, with
the pipeline cache warm. Each window is 57-59 s of a run, and each figure is
from one run.

| Guest cadence | Guest frames/s | Process CPU | Draws per guest frame | Render passes/s |
| --- | --- | --- | --- | --- |
| Default: guest vblank 120 Hz | 60 | 2.39 cores | ~3,200 | 32,300 |
| `--pinyon_shift_fh1_render_fps_limit=30`: vblank 60 Hz, as on the 360 | 30 | 1.97 cores | ~3,600 | (not read) |

- **Guest frame rate.** ShiftGlue runs the guest vblank at twice the render
  limit, or twice `video_mode_refresh_rate` (60) when no limit is set. That
  is FH1 logic: FH1 waits two vblanks a frame, and so does FH2. So by
  default FH2 renders 60 frames a second, twice its console rate. Whether
  its simulation keeps real time at 60 is not checked yet. The capped run
  is the console-equivalent cadence and the baseline for LV-0.19.
- **Fixed cost.** Doubling the frame rate adds only 0.42 of a core. Most of
  the cost does not scale with frames.
- **The render thread spins a full core.** At the 30 cap, the thread that
  registers FH2's GPU interrupt callback used 99.7 % of a core, out of 1.89
  cores for the whole process (a 30 s per-thread sample). The CP thread
  used 28 %, and the next guest threads 14 %, 13 % and 8 %. This matches
  what fh2-recompiled measured on Android: FH2's D3D layer re-reads the
  ring read pointer in a loop while it waits for ring space, and ShiftGlue
  publishes the pointer only after a whole burst. Their hook is on the
  base disc's `sub_82B72A90` (the ring-room wait, called from
  `sub_823F94D0`). Pinyon's address mapper maps those exactly to TU v2
  `sub_82B72EA8` (size 0xE0) and `sub_823F9528`. As a check, it also maps
  their setjmp and longjmp onto the v2 addresses carried from FH1.
- **GPU thread.** It spends 0.5-0.7 s of each second in `WAIT_REG_MEM`, and
  submission keeps it busy 0.1-0.2 s.
- **Locks.** Critical-region contention is 2,300 a second and blocks for
  8 ms a second in total. Not a problem here.
- **Memory.** Resident memory is 1.5 GB in the opening drive.

`source_frame_count` and the `fh1_*` thread columns read 0: they come from
Pinyon's FH1 hooks. For FH2, guest frames are presents minus duplicates.

## Next

1. Fix the ring-room spin: publish the read pointer after each block, and
   have `sub_82B72EA8` wait on the CP's progress instead of spinning. Then
   re-measure both cadences.
2. Reach a race (finish the opening drive) and record free roam and a race
   at both cadences.
3. Bring the reference build to the Odin and the Deck, then run the
   LV-0.x probes.
