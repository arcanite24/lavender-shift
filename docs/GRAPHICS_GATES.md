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
