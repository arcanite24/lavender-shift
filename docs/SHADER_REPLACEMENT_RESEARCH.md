# Shader replacements

Which FH2 shaders are worth replacing with our own, either for speed or for
a modern look. SR-0 (below) measured FH2's frame on the RTX 4080; the
weak-GPU measurement is still to do.

## What already exists

ShiftGlue (the same SDK Lavender builds on) can already do everything
this needs. Pinyon Shift built it for FH1 ([its MODDING.md](https://github.com/arcanite24/pinyon-shift/blob/main/docs/MODDING.md#shader-replacements)):

- **Replacement by identity.** A SPIR-V file named
  `shader_<guest hash>_<modification>_<translated hash>.spv` in a mod's
  `shaders/` folder, or in `vulkan_shader_override_dirs`, replaces that one
  translation. If the driver rejects the file, the original translation is
  used instead. `vulkan_shader_overrides=false` turns every replacement off.
  A replacement must keep the original's bindings, inputs and outputs: the
  guest still sets the constants and textures.
- **Discovery.** `vulkan_shader_discovery_frame=N` records one frame's
  draws, sampled bindings, resolves and shader identities.
  `tools/discover-fh1-shaders.py` (Pinyon's) turns that record into a
  per-shader report, with no disassembly.
- **Cost per pass and per shader.** `fh1_native_gpu_profile` times each
  rendering. `fh1_debug_skip_renderings` with `fh1_debug_skip_pixel_shader`
  knocks out one shader's draws live, which gives a ceiling on what
  replacing that shader could save.

A replacement is our own GLSL compiled to SPIR-V, written from the pass's
inputs and outputs. Translated guest shaders and captures stay in `.local/`
and are never committed (docs/LEGAL.md).

## What FH1 measured

Pinyon ran this study on FH1 ([MODERN_RENDERING_RESEARCH.md](https://github.com/arcanite24/pinyon-shift/blob/main/docs/MODERN_RENDERING_RESEARCH.md)),
with a gate of 1 ms of GPU frame time saved on at least one GPU-bound
device:

- Every pass-level idea failed the gate except the lower reflection update
  rate. That covered less MSAA, shadow-caster reuse, depth reuse, fusing the
  bloom reductions, removing copies and batching sprites.
- The only full-screen shaders above the gate were on weak GPUs:
  - the final composite: 6.37 ms on UHD 620, 1.57 ms on Mali, 1.15 ms on
    M4 Pro;
  - one full-size pass that reads scene colour three times and writes it
    back before the composite: 3.29 ms on UHD 620.

  Both are fragment-bound, so they cost more at higher resolution scales.
- On the RTX 4080 nothing passed. FH2 looks the same there (LP-4.7): about
  6 ms of GPU work in a frame, of which 4.3-4.6 ms is the scene.

So on a desktop GPU, replacements are for the look, not for speed. Speed
matters on low-end GPUs, at 2x scale and above, and for later ports.

## Candidates

Ranked by what a player would notice. "Same interface" means the
replacement can go in through the existing override path.

| ID | Shader or pass | Replace with | Why | Interface |
| --- | --- | --- | --- | --- |
| SR-1 | **Final composite** (FH1's reads scene colour, the small bloom image, a mask, a 16³ grading LUT and the exposure) | A modern tone-map curve (AgX or ACES fit) applied after the game's grading, blue-noise dithering against banding, and the HDR path's paper white | The most visible single shader; on weak GPUs it is also the most expensive one | Same |
| SR-2 | **Bloom chain** (downsample and blur passes into the small image) | A 13-tap downsample and tent-filter upsample | Less flicker and aliasing in the glow; the chain's sizes stay | Same, per pass |
| SR-3 | **Ambient occlusion** (FH2 has none) | A new host pass over the scene depth. **Prototype done**, see "SR-3: ambient occlusion" below | The flat look players notice; the biggest visual gain on offer | New: a ShiftGlue pass at swap, not a replacement |
| SR-4 | **Motion blur** (half-resolution velocity buffer, then the blur) | Tile-max velocity blur with more samples | Smoother blur at high frame rates; the stencil fix stays (STAGE0.md, "Motion blur") | Same |
| SR-5 | **Depth of field** | A bokeh gather | Photo mode and replays | Same |
| SR-6 | **The full-size pass before the composite** (FH1's draw 5361 equivalent, if FH2 has one) | A cheaper equivalent with the same output | The one speed candidate above the gate on FH1 (UHD 620) | Same |
| SR-7 | **Shadow filtering** inside the scene's material shaders | A wider PCF kernel or PCSS | Softer, steadier shadow edges | Same, but one replacement per material shader, so dozens |

Already covered elsewhere, so not on this list: anti-aliasing (FXAA, DLSS
and FSR are host effects: `swap_post_effect`, `present_effect`) and upscaling
(Pinyon's upscaling backlog).

## SR-0 results (2026-11-10, RTX 4080, free roam at 2x)

- **FH2's post chain is FH1's.** A one-frame discovery capture at 2x
  shows the same post shaders as Pinyon's, by guest hash, in the same
  order and with the same targets:
  - the reductions;
  - exposure with history;
  - the bloom downsample and upsample;
  - the full-size pass `64AEAE9314A0118B`;
  - the final composite `614588022744BF6B`;
  - the reflection mip shader `21937679208E59A5`.

  Only the first shader of the velocity pass (`ADD2563BA387C2BF`) is
  FH2's own. Across the frame, 116 of 148 pixel shaders and 108 of 140
  vertex shaders are identical to FH1's. So a replacement written for one
  game can serve the other, with only the translated-hash part of its
  file name to regenerate.
- **No AO pass.** Only three groups of draws read the camera depth: the
  screen-space shadow mask and two in-scene shaders. SR-3 is an
  addition, not a replacement.
- **Cost per pixel shader** (`fh1_native_gpu_profile_draws`, mean of six
  10 s windows):

  | Shader | GPU ms a frame | Draws |
  | --- | --- | --- |
  | Final composite `614588022744BF6B` | 0.26 | 2 |
  | Depth-only draws (no pixel shader) | 0.25 | 101 |
  | Reflection mips `21937679208E59A5` | 0.22 | 48 |
  | `539D4A36B27B206D` (scene) | 0.12 | 44 |
  | Shadow mask `93626E75D17576C5` | 0.10 | 3 |
  | Full-size pass `64AEAE9314A0118B` | 0.09 | 1 |
  | Bloom down/upsample, reductions | under 0.05 each | 1-4 each |

  Nothing on this GPU passes a 1 ms gate, so the 1x profile and the
  knock-outs were not run here. Speed questions wait for a weak GPU
  (blindvac's UHD 620, where FH1's composite took 6.37 ms).

Captures stay in `.local/shader-discovery/`.

## SR-3: ambient occlusion (2026-11-10)

ShiftGlue now adds screen-space ambient occlusion at swap
(`swap_ambient_occlusion`, the GRAPHICS menu's AMBIENT OCCLUSION row; off
by default). It works in two passes:

- **A half-size compute pass** (`src/graphics/vulkan/ambient_occlusion.cpp`
  in the SDK). It reads the camera depth snapshot that the upscalers use
  and does a scalable-AO style estimate: 12 samples on a spiral within
  `swap_ambient_occlusion_radius` metres (default 1). The spiral is turned
  per pixel by a 4x4 ordered pattern. The pass fades out between 80 and
  200 m.
- **The scene-only post pass** (`scene_post.cpp`). A 4x4 depth-aware box
  removes the pattern, then the scene colour is darkened by up to
  `swap_ambient_occlusion_strength` percent (40/70/100 in the menu).
  Pixels the HUD covers are left alone (pre-HUD snapshot), as FXAA does.

`swap_ambient_occlusion_debug` shows only the occlusion term.

Two things about the depth had to be right:

- The scene is drawn turned 180 degrees, so a screen pixel's depth is at
  the opposite pixel. Reading it unturned put an upside-down copy of the
  car in the sky.
- Both games store depth reversed (1 near), but differently. FH1's
  matrix gives the usual 0-near depth, and its viewport flips it. FH2's
  matrix is already reversed: depth = -1.5e-5 + 0.300 / view z, so near
  is 0.3 m and far 20 km. The camera fits A + B / view z from the
  matrix's z and w rows and converts a 0-near fit to the stored form, so
  one formula serves both.

The look, in free roam at 2x on the RTX 4080:

- contact shading under cars and along kerbs;
- a darker cockpit;
- depth inside trees and bushes;
- no change to the sky or the HUD.

The cost is not measured yet. The perf CSV's frame GPU time includes the
wait for the 60 fps cap, and at that cap AO on and off read 16.30 and
16.18 ms, within noise. It needs a GPU timestamp around the two passes,
or a run on blindvac's UHD 620. The default stays off until the player
has compared it in play.

## SR-0: the measurement

Run it on the scripted save at 60 fps (`--lavender_frame_rate=60`). Never
run it on the player's save, and never while the player is in the game.

1. Run a discovery frame in free roam and another in a race. The race needs
   the LP-0.4 save. Then report per shader: draws, render target and size,
   sampled textures, resolves. This finds FH2's composite, bloom chain,
   blur, DoF and full-size passes, and confirms whether there is any AO.
2. Take `fh1_native_gpu_profile` per rendering at 1x and 2x on the RTX 4080.
   Do the same on blindvac's UHD 620, the weak-GPU yardstick, once a
   Lavender build runs there.
3. Measure live shader knock-outs for each candidate's shader. This gives a
   ceiling, not a saving.
4. Gate each row:
   - for speed, Pinyon's rule: at least 1 ms saved on one GPU-bound device
     with no regression elsewhere;
   - for looks, an A/B image pair in free roam (day, dusk, night) and the
     player's opinion.

   Replacements stay off by default until they pass, and each has its own
   cvar.

SR-1 is the best first prototype: one shader, same interface, the most
visible change, and on weak GPUs also the most expensive pass. It costs
0.26 ms a frame on the RTX 4080 at 2x, so a heavier modern tone map
still fits there. Because the shader is FH1's, the same file can also
ship in Pinyon Shift.
