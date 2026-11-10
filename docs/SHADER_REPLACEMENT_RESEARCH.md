# Shader replacements

Which FH2 shaders are worth replacing with our own, either for speed or for
a modern look. This is a plan. No FH2 shader has been measured one by one
yet: SR-0 below is that measurement, and every candidate waits on it.

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
| SR-3 | **Ambient occlusion** (none known in FH2) | A new host pass (GTAO or HBAO) over the scene depth, applied before the composite | The flat look players notice; the biggest visual gain on offer | New: a pass ShiftGlue's FH1 executor inserts. Not a replacement, and SDK work |
| SR-4 | **Motion blur** (half-resolution velocity buffer, then the blur) | Tile-max velocity blur with more samples | Smoother blur at high frame rates; the stencil fix stays (STAGE0.md, "Motion blur") | Same |
| SR-5 | **Depth of field** | A bokeh gather | Photo mode and replays | Same |
| SR-6 | **The full-size pass before the composite** (FH1's draw 5361 equivalent, if FH2 has one) | A cheaper equivalent with the same output | The one speed candidate above the gate on FH1 (UHD 620) | Same |
| SR-7 | **Shadow filtering** inside the scene's material shaders | A wider PCF kernel or PCSS | Softer, steadier shadow edges | Same, but one replacement per material shader, so dozens |

Already covered elsewhere, so not on this list: anti-aliasing (FXAA, DLSS
and FSR are host effects: `swap_post_effect`, `present_effect`) and upscaling
(Pinyon's upscaling backlog).

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
visible change, and on weak GPUs also the most expensive pass.
