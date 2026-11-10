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
| Odin 2 Portal | Waiting: Windows desktop only for now ([ground rule 5](APPROACH_RESEARCH.md#ground-rules)) |
| Steam Deck | Waiting, as above |

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
| Default: guest vblank 120 Hz | 60 | 2.39 cores | ~2,250 | 32,300 |
| `--pinyon_shift_fh1_render_fps_limit=30`: vblank 60 Hz, as on the 360 | 30 | 1.97 cores | ~2,250 | (not read) |

Guest frames are presents minus duplicate presents. An earlier draft of
this table divided draws by all presents and gave 3,200-3,600. The process
CPU here is before the ring-wait fix below.

- **Guest frame rate.** ShiftGlue runs the guest vblank at twice the render
  limit, or twice `video_mode_refresh_rate` (60) when no limit is set. That
  is FH1 logic: FH1 waits two vblanks a frame, and so does FH2. So by
  default FH2 renders 60 frames a second, twice its console rate. Whether
  its simulation keeps real time at 60 was checked under LP-0.1
  ([PINYON_PARITY.md](PINYON_PARITY.md)): it does. The capped run
  is the console-equivalent cadence and the baseline for LV-0.19.
- **Fixed cost.** Doubling the frame rate adds only 0.42 of a core here,
  and 0.54 once the ring-wait fix removes the spin (0.87 → 1.41 cores).
  Most of the cost does not scale with frames.
- **The render thread spins a full core** (fixed: see
  [Ring-room wait](#ring-room-wait)). At the 30 cap, the thread that
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

## Ring-room wait

FH2's D3D layer waits for ring space by calling a poll function in a loop:
TU v2 `sub_82B72EA8`, base disc `sub_82B72A90`. Each call:

1. spins for a moment;
2. re-reads the read pointer that the command processor writes back
   (through the address the device keeps at `+0x2B10`);
3. stamps the time whenever that pointer has moved;
4. returns 1 to keep waiting, or 0 to give up. It gives up when the device
   is lost, or when the pointer has not moved for longer than the game's
   threshold and the game's hang check fails.

On the host the loop never blocks, so the render thread burns a core while
the CP works through a burst. The fix has two parts:

- **ShiftGlue `gpu_read_pointer_publish_dwords`** (default 0, so FH1 is
  unchanged). When it is set, the CP writes the read pointer back every
  that many dwords inside a burst, not only at its end. Every writeback
  also bumps the GPU-write sequence (`rex/system/gpu_write_signal.h`) and
  wakes its waiters.
- **Lavender's override of `sub_82B72EA8`** (`src/ring_wait.cpp`,
  `lavender_ring_wait_us`, default 1000). When the read pointer has not
  moved since the last poll, the override sleeps until the next GPU write
  or the timeout. Then the original poll runs unchanged, so its timestamps
  and its hang check still see real time pass.

The A/B ran the same route (opening drive, standing at the start line for
59 s, then 30 s of throttle) on multivac, one run per cell, on 2026-11-09.
"Off" is `--lavender_ring_wait_us=0 --gpu_read_pointer_publish_dwords=0`,
and "on" is the defaults plus `--gpu_read_pointer_publish_dwords=1024`:

| Cadence | Wait | Process CPU, standing | Process CPU, throttle | Busiest thread | Host frame p99 |
| --- | --- | --- | --- | --- | --- |
| 30 (capped) | off | 1.77 cores | 1.84 | 99.8 % | 33.83 ms |
| 30 (capped) | on | **0.87 cores** | 0.92 | 22.9 % | 33.83 ms |
| 60 (default) | off | 2.26 cores | 2.30 | 99.9 % | 17.12 ms |
| 60 (default) | on | **1.41 cores** | 1.50 | 41.7 % | 17.13 ms |

- The fix saves 0.85-0.90 of a core at both cadences, about half the
  process CPU at the 30 cap. Guest frame rate, frame pacing and the GPU
  thread's counters do not change.
- `tools/launch.ps1` turns it on. The two parts were not measured
  separately: without the publish cvar, read-pointer writebacks do not
  wake the wait, and only fence writes or the timeout end it.
- **A pitfall found on the way:** codegen's `direct_calls_bypass_thunk`
  makes direct calls name `__imp__sub_...`, so an app's strong `sub_...`
  only sees indirect calls. The first A/B showed no change for this
  reason. A replaced function must be listed in `host_overridden_functions`
  (`config/analysis/default.toml`), as Pinyon does for its guest-native
  replacements.

## Settings menu

F6, Back+Start on a pad, or the pause menu's PC SETTINGS tile (the Home
page's ONLINE ROAD TRIP tile, relabelled) opens the host menu Pinyon Shift uses, drawn
with FH2's own fonts and textures from the disc (`src/ui/`). While it is
open the game sees XAM system UI, so FH2 opens its pause menu behind it.
Every row applies while the game runs, with no restart and no restart
badges:

- **DISPLAY:**
  - fullscreen, aspect ratio (with WIDER VIEW), vsync (vsync recreates
    the swapchain), field of view;
  - HUD: the HUD switch (F2), HUD SCALE 80-120 % and a SAFE ZONE margin
    of up to 10 %, which move the gameplay HUD only (menus, the map and
    loading screens keep their layout);
  - one FRAME RATE: 30, 60, 120, 144, DISPLAY (the monitor's refresh rate,
    the default) or UNLOCKED; vsync only decides whether presents wait for
    the display;
  - monitor and window size;
  - output scaling and sharpening;
  - HDR with its paper white, highlights, gamma, an SDR/HDR compare and a
    calibration page, when the display offers it.
- **GRAPHICS:**
  - presets: CONSOLE (30 fps), LOW-SPEC, PERFORMANCE, QUALITY;
  - resolution scale 1-4x, MSAA (4x, 2x, off);
  - anisotropic and trilinear filtering, texture detail, texture size;
  - post-process AA and AA on the HUD;
  - bloom, motion blur, depth of field, shadows, reflection updates.
- **AUDIO:** master volume and mute.
- **CONTROLS:** mouse and keyboard, controller buttons (remap, invert
  look), map a controller, racing wheel (SDL steering and pedal tuning),
  keybinds and RESET KEYS (Pinyon Shift's driving keys,
  `src/config/default_keys.h`).
- **GAMEPLAY:** voice-over skip, automatic car reset, the attract video,
  the photo camera through walls.
- **CHEATS:** any car in any event and no rubber-banding, both on by
  default on the player's own profile; a change applies from the next
  event.
- **PROFILE:** save backups, and a restore chosen from them at the next
  start.

Diagnostics follow Pinyon Shift's: crash reports in `<user>/crashes`, a
session event file in `<user>/logs`, and a stall dump of every guest
thread after 20 s without a simulation step (or when
`<user>/logs/dump-threads` is created).

The settings are saved in `<user>/config/lavender_shift.toml`. To make
these live, ShiftGlue's renderer now rebuilds its texture, pipeline and
render-target caches between frames when the resolution scale, MSAA or
texture size changes. Those cvars moved from "requires restart" to hot
reload, so Pinyon's menu applies them live too.

The rebuild stalls the game once. It first measured 6.1 s for 1x → 2x
and 1.9 s for an MSAA change on multivac; LP-2.13
([PINYON_PARITY.md](PINYON_PARITY.md)) cut it to a frame of about
130-150 ms for a scale change and 6-11 ms for an MSAA change.
The rows in FH2's own code (bloom, motion blur, depth of field, shadows,
reflections, FOV, HUD) use FH2 addresses found for them; the parity plan
lists each one.

## Controllers

Every controller plays as guest user 0, the only signed-in user, as in
Pinyon Shift (`SharedAssignment`, set in `OnPreSetup`). With the SDK's
default slot assignment, a pad behind a virtual one (Steam Input, ViGEm)
or a wheel pressed Start as user 1, and FH2 answered with "No Profiles"
on the title screen; keyboard runs (`--mnk_mode`) never showed it.

A pad reaches everything without a keyboard: Start opens FH2's pause
menu, its PC SETTINGS tile opens the host menu, and the host menu's last
row, QUIT GAME, closes the game the way the window's close button does.

## Motion blur

FH2's camera motion blur builds a half-resolution velocity buffer from
the depth-stencil buffer. The stencil holds an object ID per pixel (the
player car, other cars, the world) that picks the transform the velocity
pass reprojects with. Values with bit 7 set are flags, not IDs: grass
writes 0x81, and the car shadows' stencil volumes mark the road they
cover with 0x80. The pass sends flagged pixels to the world transform,
but its threshold is just above 0x80 (128/255 plus half a 16-bit step),
so 0x80 reads as ID 43, whose transform is identity. That road got no
velocity and stayed sharp: a square with no blur under every moving car.

Found by replaying a frame dump taken at speed and skipping draws
(`fh1_debug_skip_draws`), then reading the resolved depth-stencil and the
pass's constants. The fix is ShiftGlue's `gpu_pixel_constant_fixups`,
which Lavender sets by default: while that pixel shader draws, the
threshold reads 0.5, so 0x80 counts as a flag too. The shadows under the
cars are unchanged, and the car itself stays sharp.

## Shader pipelines

`tools/prepare_shaders.ps1` (run by `tools/launch.ps1`) builds
`<user>/cache/shaders/shareable/4D530AA4.fbo.aot.xpso` before the game
starts, the same way Pinyon does (`vulkan_aot_pipelines`):

1. extract the shaders from the player's own disc, title update and
   patched executable: 12,670 shaders from 4,168 containers, kept in a
   work folder that is deleted afterwards;
2. rebuild every pipeline that `config/aot-pipelines.json` lists. The file
   holds only hashes and pipeline state, no game data: 719 shaders and
   1,604 pipelines, catalogued from two free-roam sessions.

The build takes 6 s and is skipped when the catalog has not changed.

The free-roam route measured it on 2026-11-09 on multivac: 4 min of
driving, throttle held, the same steering seed each run.
`vulkan_log_pipeline_waits_ms=1` counts the frames that waited for a
pipeline:

| Shader state | Frames that waited | Total wait | Longest |
| --- | --- | --- | --- |
| Cold: no stored pipelines, cold driver cache | 99 | 522 ms | 36 ms |
| AOT file only, cold driver cache | 2 | 46 ms | 45 ms |
| Warm (stored from earlier play) | 0 | 0 | 0 |

The AOT file only covers what the catalog saw. More of the map and the
races will add pipelines; rerun `tools/aot_pipelines.py catalog` over a
longer session's stored shaders to grow it.

## Frame pacing

The free-roam judder was not shaders. With the AOT file, the route still
showed 8.4 repeated and 9.0 dropped frames per second of driving. Nearly
every repeat had a drop within 3 frames.

The cause is how frames reached the display:

- FH2 renders a frame every two 120 Hz guest vblanks. When a frame runs
  late (GPU time per guest frame is 13.7 ms on average at 4K, with a p99
  of 17-19 ms), the next one comes after a single vblank to catch up.
- ShiftGlue presented on its own 60 Hz clock (`host_present_fps_limit=60`).
  So the late frame repeated a refresh, and the catch-up frame landed in
  the same refresh as the next one and was dropped.

Lavender now presents each game frame exactly once, paced by vsync. This
is ShiftGlue's `pinyon_shift_fh1_source_presentation=true` with
`host_present_fps_limit=0`, the same defaults as Pinyon. The FRAME RATE
setting (`lavender_frame_rate`) now owns the render limit and keeps the
present limit at 0; only a command-line value overrides either. Same route, the 222 s of driving:

| Presentation | Repeated frames/s | Dropped frames/s | Longest present interval |
| --- | --- | --- | --- |
| Host clock (old default) | 8.43 | 8.95 | 21.4 ms |
| Each frame once, vsync | 0.01 | 0 | 28.7 ms |

The one remaining long interval is a late frame shown one refresh late,
not repeated. The title and loading screens render at 120 frames a second,
one per vblank, and vsync holds them to the display.

## Next

1. Reach a race: drive into an event once and save there (LP-0.4), then
   record a race at both cadences.
2. Grow the pipeline catalog over races and the rest of the map, and
   prewarm pipelines in the background after a settings rebuild.
3. Run the desktop parts of the LV-0.x probes. The Odin and the Deck wait
   until the desktop-only focus is lifted.

The rest of the Pinyon Shift parity work is in
[PINYON_PARITY.md](PINYON_PARITY.md).
