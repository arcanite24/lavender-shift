# Shader mods (SR-1, SR-2, SR-7)

Our own replacements for FH2 post-processing shaders, as GLSL sources.
Each folder has a `shaders.toml` naming the guest pixel shader each source
replaces; `tools/build-shader-mod.py` compiles them to guest-named
overrides (`shaders/shader_<guest hash>.spv`, SR-9) and writes `mod.toml`.

```
python tools/build-shader-mod.py docs/examples/shader-mods/bloom --out <user>/mods/bloom-kernels
python tools/build-shader-mod.py docs/examples/shader-mods/composite --out <user>/mods/composite-gt \
    --define CURVE=1 --define DITHER=1 --name composite-gt
```

Then add the mod's name to `enabled_mods`. glslangValidator comes from the
Vulkan SDK or a glslang build (`--glslang <path>`).

| Folder | Replaces | What changes |
| --- | --- | --- |
| `bloom` | `D6652B8E4866F16B`, `0732AB80912AE1A0` (half steps), `0F5712A5F9D13E62` (upsample) | The half steps keep the game's gain, spread and pair averaging but weigh every texel edge in the footprint (no skipped host texels above 1x); the upsample reads the coarse level through a 3x3 tent instead of one bilinear tap. The two reductions that feed metering (`E17BECBE8BE65806`, `AE59F518D522BDD1`) are left alone so exposure is unchanged. |
| `composite` | `614588022744BF6B` | The game's stages by default. `CURVE` 1-3 swap the filmic curve for Uchimura, Khronos PBR Neutral or an AgX-like curve, matched at mid-grey; `DITHER=1` adds a one-step triangular dither; `BLUR_TAPS` above 5 spreads more motion blur taps over the same span; `BLUR_SCALE` scales the blur's length (a shutter angle against the game's); `RECON=1` is a velocity-only reconstruction blur (SR-4); `AO=1` multiplies the scene by the host's ambient occlusion (binding 7) before the bloom and the curve, with `vulkan_shader_override_host_inputs`, `swap_ambient_occlusion`, `swap_ambient_occlusion_in_scene` and `vulkan_midframe_hook_pixel_shaders=614588022744BF6B`; `SCENE_FLOAT=1` reads the scene from the host's 16-bit copies where they agree with the texel to one 10-bit step: the bands (binding 9) and the full-size pass's output (binding 8, with `unorm10_render_target_as_unorm16`); it needs `scene_host_copy`, `scene_host_copy_pass=64AEAE9314A0118B` and both `64AEAE9314A0118B` and `614588022744BF6B` in the hook list. `SCENE_FLOAT=2` and `3` write the band copy's difference as a check, and `4` the difference, the texel and whether the band was taken. `POST=1` (SR-21) also takes the unclipped band value where the texel is clipped and replaces the game's bloom with the host's (binding 10, `scene_bloom`), mixed in by `POST_BLOOM`. With `POST=1` the game's bloom draws can be dropped: `gpu_skip_pixel_shaders=D6652B8E4866F16B,0732AB80912AE1A0,0F5712A5F9D13E62`. |
| `sky` (mod name `sky-horizon`) | `EE3FFB69590792A2` (the sky's horizon band) | By default the game's model: colour between the away-from-sun and towards-sun constants by a cosine power, alpha fading with height. `PHASE=1` uses a Henyey-Greenstein sun lobe that halves at the same angle, so the haze keeps a faint glow on the far side instead of cutting to 0 at 90 degrees. |
| `shadows` (mod name `shadow-quality`) | `58CE51CE56247C16` (the shadow mask's blur), `93626E75D17576C5` (one of the seven mask shaders, three draws a frame) | The blur is a tent sized in guest texels over every host texel, so the shadow edges are as soft at 2x as at 1x (the game's blur halves its width above 1x); `RADIUS=2` doubles it. The mask shader keeps the game's reconstruction and matrices but replaces the 4-tap ring with a 12-tap Poisson disc of the same radius, rotated per pixel, with bilinear-weighted compares; `TAPS=8` is cheaper. `HOST_NOISE=1` takes the rotation from the SR-10 blue noise (needs `vulkan_shader_override_host_inputs`; without it the override is refused and the game's mask is used); `CONTACT=1` adds contact shadows (SR-18), with `CONTACT_LENGTH`, `CONTACT_STEPS`, `CONTACT_FAR`; `CONTACT=2` and `HOST_NOISE=2` are checks that write the march or the noise as the mask. The other six mask shaders are not replaced yet. |

Each replacement keeps the translation's interface: the same bindings, no
more uniform bytes, the same outputs (`docs/SHADER_REPLACEMENT_RESEARCH.md`).
If one does not fit, the log says why and the game's shader is used.

What each has been checked with, and what still needs the game, is in
`docs/GRAPHICS_GATES.md`.
