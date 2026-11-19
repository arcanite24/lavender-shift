# Shader mods (SR-1, SR-2)

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
| `composite` | `614588022744BF6B` | The game's stages by default. `CURVE` 1-3 swap the filmic curve for Uchimura, Khronos PBR Neutral or an AgX-like curve, matched at mid-grey; `DITHER=1` adds a one-step triangular dither; `BLUR_TAPS` above 5 spreads more motion blur taps over the same span. |

Each replacement keeps the translation's interface: the same bindings, no
more uniform bytes, the same outputs (`docs/SHADER_REPLACEMENT_RESEARCH.md`).
If one does not fit, the log says why and the game's shader is used.

What each has been checked with, and what still needs the game, is in
`docs/GRAPHICS_GATES.md`.
