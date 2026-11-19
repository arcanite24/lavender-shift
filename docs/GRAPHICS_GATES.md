# Graphics gates (issue #5, phases 1-3)

What each item on `gfx/issue5-phases` has been checked with, and what is
still to check with the game. Everything here was written while another
session had the game, so no item has been seen in play. One game session
can clear the list from the top; every row says what to run and what
passes.

States: **offline-tested** (a test without the game passed), **compiles**
(Windows and Android compile; never executed), **gate pending** (needs the
game).

## Before anything else

1. **Replay fidelity (SR-8).** Every shader A/B below rests on this. The
   existing replays of `.local/logs/{mb,lod,wall}.fh1dump` show solid
   colour bars in places, and nearly every word differs from the capture,
   so it is unproven that a replay reproduces FH2's frame.

   ```
   python tools/frame-replay.py replay .local/logs/mb.fh1dump --out .local/ab/fidelity --label base
   # from a worktree: add --root <main checkout>
   ```

   Pass: `front.png` matches a screenshot of the same frame (F8) apart from
   the HUD, with no bars. If not, the shader rows below need play instead.

   **Run 2026-11-18** (the main checkout's build, `--root` pointing at it
   from the worktree): the replay of `mb` took about ten minutes with
   synchronous pipelines and gave a complete, plausible frame (road, car,
   town, sky, HUD) with no colour bars, so replays are usable for A/B at
   1x. Still to compare against an F8 capture of the same frame.

## Items

| Item | State | Gate (with the game) |
| --- | --- | --- |
| SR-14 rows, auto mip bias, GSR | compiles; on `main` | Each row applies live; GSR on Android |
| SR-9 guest names, interface check | offline-tested (fixtures, all 291 modules of `fr-2x` fit themselves) | A guest-named file loads at 1x and 2x, on Windows and Android; a misfit logs and falls back. Live reload is not done: a restart reloads |
| SR-8 replay A/B, discovery | offline-tested (decode and compare on three dumps; discovery and `--check-mod` on `fr-2x`) | `frame-replay.py ab` gives a pair and a PSNR; fidelity above |
| SR-16 data patches | offline-tested (patches applied to the real TrackSettings.xml: priority, path case, CRLF, problems) | Start with `mods_log_game_files=.xml` and drive in free roam: are `TrackSettings.xml` and `Ribbon_00/TimeOfDay*.xml` opened? Then enable `docs/examples/data-patches` as a mod: shadows reach further. Frame time on the RTX 4080 and UHD 620. No preset row until then |
| SR-13 texture tooling | offline-tested (alias precedence and problems on synthetic folders; `tools/texture-pack.py` round trip for DXT1/3/5, BC4, BC5, RGBA8 with size checks); MODS page compiles | Turn DUMP TEXTURES on in town, run `texture-pack.py census`, then build a 4x pack of the 128 x 128 building atlases (docs/STAGE0.md) and RELOAD TEXTURES: they sharpen without a restart. Also check that a streamed `_B.bix` top mip changes the hash (a pack must cover both). Not done: BC7 or RGBA8 for DXT sources, and cube maps (both caches create the host image in the guest's format; needs a host-format override in the Vulkan and D3D12 caches) |
| SR-20 DRAW DISTANCE (desktop) | compiles; the row sets `lavender_native_scene_cull` and `lavender_lod_distance_scale` (1.5 / 1 / 0.75 / 0.5), DEFAULT leaves both off | Drive the town and the motorway at each choice: pop-in at the edge of the streamed area, draws (F3) and CPU cores against DEFAULT (native culling measured 5.00 against 4.57 cores). Drop FARTHER if streaming cannot keep up |
| SR-19 MLAA (POST-PROCESS AA) | offline-tested (the three passes compile with glslang; a Python model of the same maths cuts the error against an 8x8 supersampled edge by 74-88% at 10-85 degrees); compiles W/A. Our own morphological AA after Reshetov 2009 and SMAA 2012, coverage computed in the shader, no diagonals, Vulkan only | A slow pan past roadside trees and fences: less shimmer than FXAA, the HUD text unchanged (with AA ON THE HUD off), frame time at 1440p and on the UHD 620. Not done: sharpened alpha in the cutout shaders (needs the foliage shaders identified in play) |
| SR-10 host inputs for replacement shaders | offline-tested (fit check: rejected while off, accepted while on, a block over 16 bytes rejected; the blue noise has a flat histogram per channel and about 16% of white noise's energy after a 4x4 blur, 175 ms to make); compiles W/A. `vulkan_shader_override_host_inputs` (restart) adds set 0 binding 5, sampler2D blue noise 64x64 nearest repeat, and binding 6, `{vec2 draw_resolution_scale; float blue_noise_texel; uint version}`; off, set 0 is as before. Never executed | Start with it off and on with no overrides: same frame, no validation errors. Then a replacement that reads both (dither in the composite) loads at 1x and 2x on Windows and Android |
| SR-2 bloom kernels (`docs/examples/shader-mods/bloom`) | offline-tested: the three replacements compile and fit their `fr-2x` translations; a model of a spiky row puts the half steps' mean within 0.98-1.00 of the game's at 1x and 2x (a linear-light kernel came out 25% brighter, so the game's pair averaging of sqrt-encoded texels is kept) and the upsample's tent within 0.6%, 18% smoother. Never executed. The reductions that feed metering (`E17BECBE8BE65806`, `AE59F518D522BDD1`) are not replaced, and no Karis average: nothing in the shader tells the first half step from the rest. Sign modes are taken as unsigned and the gamma flag as off (both hold for the 7e3 targets) | A/B by replay, then in play: bloom no blockier at 2x, the same strength by day and at night, and **no brightness drift over 30 s of driving** (the last upsample feeds back into the next frame through 4D39) |
| SR-11 mid-frame hook (trigger only) | compiles W/A; never executed. `vulkan_midframe_hook_pixel_shaders=<hash,...>` ends rendering before the first draw with each listed guest pixel shader in a frame and runs the passes registered with `AddMidframeHook`; empty (the default) changes nothing. **Not done: its first user**, moving SR-3's AO before the composite: the AO result is half size with its view z and needs the scene-post bilateral upsample and the 180-degree turn, so it needs its own upsample pass and a host-input binding for the composite. The resolve-address and marker triggers are not done either | Replay `mb` with `--set vulkan_midframe_hook_pixel_shaders=614588022744BF6B --set fh1_frame_replay_repeat=2`: the log shows the hook once per frame at the composite's draw, and `front.png` matches the base replay exactly |
| Constant log for replacements | compiles W/A. `gpu_log_pixel_constants=<hash,...>` logs the float constants a guest pixel shader uses, by register, and the non-zero fetch constants 0-15, for the first `gpu_log_pixel_constants_count` draws | Used by the SR-1 and SR-2 replays below |

| SR-1 composite (`docs/examples/shader-mods/composite`) | compiles; the default and the CURVE 1-3, DITHER, BLUR_TAPS and BLUR_SCALE variants fit the `fr-2x` translation. Replay A/B pending (below). Our own code from a functional contract: motion blur with tag rejection (a rejected tap takes the centre; the game falls back along a chain of accepted taps), bloom, exposure, the game's curve from c43/c44, the LUT (3D or stacked, from fetch 7), vignette and colour matrix. The roles of c253-c255 are inferred; the constant log settles them | Replay: the default variant against the game's composite, PSNR well above 40 dB outside blurred pixels. Then in play by day, at dusk and at night, each curve; frame time on the UHD 620 (the game's takes 6.6 ms there) |
| SR-7 shadow mask blur (`docs/examples/shader-mods/shadows`) | compiles and fits. A tent sized in guest texels over every host texel (`RADIUS`, 1 = the game's 3x3 at 1x), red only, writes (lit, 1, 0, 0) like the game's. Replay pending. **Not done:** the seven mask shaders (a rotated Poisson PCF with a per-pixel hash would replace the 4-tap ring), a depth-aware blur (needs the camera depth as a binding), the cube-face shaders, a SHADOW QUALITY row (#3) | Replay at 1x: shadow edges no harder than the game's; in play at 2x: softer than the game's at 2x, no light bleeding past car silhouettes |

## Not done in phases 1-3

What the branch does not have, and what each needs first.

| Item | State | Needs |
| --- | --- | --- |
| SR-12 float scene to the composite | not started | An RGBA16F host copy of the band resolves (`k_2_10_10_10`, bias -2) and the full-size output in the Vulkan texture cache, bound where the bloom chain, the full-size pass and the composite read the scene; replay-checkable with a dark gradient. Ship with SR-1 |
| SR-17 car paint, glass, specular AA | not started | A showroom or photo-mode capture to tell the paint hash from the rest (FH1's car materials share hashes) |
| SR-18 contact shadows | not started | The shadow mask contract (in progress for SR-7) and the sun direction in view space; then an override of the mask's last pass with the camera depth (SR-10 has no depth binding yet) or an SR-11 pass after the mask |
| SR-24 sky and atmosphere | not started | The seven sky shaders identified in a capture with a visible sky and a time-of-day sweep |
| SR-4 motion blur | partial: in the SR-1 composite, `BLUR_TAPS` (more taps over the game's span) and `BLUR_SCALE` (length against the game's, like a shutter angle). No tile-max / neighbour-max reconstruction, no blur after a temporal upscaler | The velocity buffer's scale (a constant log, below) for a reconstruction blur; an SR-11 pass after the upscaler |
| SR-5 depth of field | not started | Photo mode or replay captures; the camera depth at the composite (SR-10 binding) |
| SR-21 our own post chain | not started | SR-12 and a working SR-11 user |
| SR-22 AO v2 | not started | SR-11's first user (the AO move above); XeGTAO or CACAO would need THIRD_PARTY_NOTICES entries |
| SR-23 upscaler inputs | not started | The FSR reactive and transparency inputs exist (`VulkanUpscalerInputs`) and nothing fills them; the presenter already gives the upscalers and frame generation a hudless image and composes the HUD after, so a HUD-only mask may add nothing; the masks need draw classes first (particles, tyre smoke, glass) |
