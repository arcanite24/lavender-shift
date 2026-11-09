# Lavender Shift: approach research

Written 2026-11-09. Lavender Shift is a port of Forza Horizon 2 for the
Xbox 360 (the Sumo Digital version, not the Xbox One game) to Windows,
Linux, the Steam Deck, Android and macOS. This page answers one question:
given what Pinyon Shift measured on Forza Horizon 1, how should the port
be built so that it runs as close to a native build as possible?

Nothing was built or run for Lavender Shift except the dump analysis in
[What FH2 is](#what-fh2-is). FH1 numbers are Pinyon Shift's
measurements, cited by document and item (Pinyon docs are on
[GitHub](https://github.com/arcanite24/pinyon-shift/tree/dev/docs)).
Estimates are marked **(est.)**. Item prefix: LV-.

## Ground rules

Set by the maintainer on 2026-11-09:

1. **Performance first, correctness second.** The goal is near-native
   CPU and GPU cost. Bit-exact 360 behaviour is not a goal. A transform
   that is faster and leaves the game playable is taken, and the bugs it
   causes are fixed one at a time.
2. **One game version.** The base disc with the latest title update
   applied, from the first build. No second address map, ever.
3. **Vulkan from the start**, on every platform.
4. **One SDK for both games.** The work goes into ShiftGlue (Pinyon's
   fork of ReXGlue), which must run FH1 and FH2. There is no second fork
   (decided after the first draft of this page). The reference model is
   still UnleashedRecomp: more native code, more reverse engineering.

The yardstick is Pinyon's NN one: total CPU milliseconds per frame over
all threads, plus resident memory, on the Odin 2 Portal and the Steam
Deck LCD, in a race at 60 fps. The desktop is secondary. This page adds
four measures to it:
- GPU ms per pass, since the GPU binds both handhelds;
- p95 and p99 frame times;
- the critical path of the render and simulation threads, not only the
  sum over threads;
- a 20-minute soak with clocks and battery current. The Astra holds
  60 fps for about a minute, then falls to 26-28 fps; performance hints
  did not help (A60-5.2).

## Answer in short

- **FH2 is FH1's engine.** It shares 70.5 % of its code bytes and 81 %
  of its functions with FH1 v4 under a masked hash, with the same
  compiler, the same middleware and the same file formats. 168 of its
  174 shader files are byte-identical to FH1's. Pinyon's findings,
  seams and tools mostly carry over. TU v2 is the last update; it
  rebuilds `default.xex` page-exact on our USA disc. The 360 version has
  no DLC. See [What FH2 is](#what-fh2-is).
- **FH1's overhead is not mainly "recompiled code".** At the NN baseline
  the Odin spent 38.8 ms of CPU a frame in a 60 fps race. Pinyon's
  estimate of the distance to a native build is 15.4-23.1 ms
  ([NN, What native means](https://github.com/arcanite24/pinyon-shift/blob/dev/docs/NEAR_NATIVE_RESEARCH.md#what-native-means-layer-by-layer)):
  - GPU front end and submission: 9.6-10.7 ms;
  - guest bookkeeping in the translated code: 4.3-8.4 ms;
  - polling, HLE sync and write watches: 1.5-4.0 ms;
  - audio and media: about 2.5-4 ms.

  With exact codegen, every large measured win in the field came from
  replacing a platform seam, not from a better translator.
- **XenonRecomp is not a lighter translator than ShiftGlue.** Both emit C++
  text with a register context, byte-swapped loads and stores and a
  dispatch table. ReXGlue's generator descends from XenonRecomp's. No
  360 recompiler emits LLVM IR or does whole-program optimisation.
  UnleashedRecomp is fast for two other reasons. It deletes the emulated
  GPU, ring buffer, MMIO and physical-memory layers. It also turns on
  every codegen option that trusts the PowerPC calling convention, which
  Sonic Unleashed tolerates.
- **Exactness blocked part of what FH1 left on the table, but less than
  it looks.** The biggest case is the register context. Under contracts
  FH1 could prove, 0.9 % of hot-thread instructions are removable; if
  callees are trusted to follow the ABI, 23.7-29.6 % are (Pinyon
  CS-0.2). That ceiling charges nothing for spills, and Pinyon measured
  spills clawing back 40-68 % of such gains on arm64 and 85 % or more
  on the Deck (NN-1.10). The realistic gain is about 3-8 % of guest CPU
  (est.). Other candidates: volatile registers treated as dead after a
  call (crashed on one stale-r6 read), and one FP mode on x86 (arm64
  already has it). The ground rules make these available, with a tool
  that finds where the assumption breaks.
- **A pure UnleashedRecomp renderer does not fit this engine.** FH1 draws
  64-83 % of its drawn draws from prebuilt D3D command buffers (display
  lists with PM4 inside), not from draw calls (Pinyon DL-0.1).
  fh2-recompiled measured about 60 % of FH2's draws coming from
  recorded buffers. The renderer must own
  the command-buffer API as well. It translates each list once, when the
  list is recorded or loaded, replays the translation, and never decodes
  PM4 at frame time.
- **Recommended stack**, all inside ShiftGlue and shared by FH1 and FH2
  (details in [Design by layer](#design-by-layer)):
  - CPU: a **trusted-ABI profile** in ShiftGlue's generator. A poison
    build finds the call sites where a title breaks the convention.
  - GPU: a D3D-level renderer for this engine, at its graphics device
    class and at D3D's command-buffer API, with each game supplying its
    own addresses. Vulkan 1.3 with bindless resources; shaders and
    pipelines compiled ahead of time; textures converted at install;
    native render targets. No EDRAM model, no predicated tiling, no PM4
    per frame.
  - Runtime: a lean mode. A flat 4 GB guest map with no physical views
    and no GPU mirror, host threads, kernel objects on host primitives,
    no global lock, and fences that complete at submit.
  - Audio: FMOD stays translated. XMA is decoded natively on a worker,
    output is native, and the mix is forced to stereo.
  - Content: one image (base + TU v2) built at install from the player's
    files.
- **Estimated CPU end state on the Odin: about 23-28 ms a frame (est.).**
  FH1 today runs at 32.5-34.4 ms (NN-1.9), and Pinyon's native estimate
  is 16-23 ms. Most of the 5-10 ms comes from the renderer; see
  [Budget](#budget-estimate). Every row has a Stage 0 probe that
  measures its premise first.
- **On both handhelds the GPU, not the CPU, sets the frame rate.** The
  Odin's GPU takes 15.3 of 16.7 ms only with shadows off and
  reflections at a quarter rate. The Deck's GPU takes 17.2-17.9 ms at
  its 15 W limit, where CPU time saved becomes GPU clock. The renderer
  rewrite saves little GPU time by itself: in-run ceilings for
  resolves and transfers were 0.15-0.28 ms and 0. The GPU levers with
  measured ceilings are [first-class items](#gpu-work-the-limit-on-handhelds):
  - occlusion culling (oracle -2.2 to -3.2 ms on the Odin's main pass);
  - a cheaper sun-shadow path (6.4 ms today);
  - render scale below 1x;
  - resolving reflection-cube faces straight into the cube.
- **The game's own pacing matters as much as translation.** FH2 was
  built for 30 fps. FH1 needed vblank, step-gate, late-swap and
  render-job throttle fixes to run at 60. With fences completing early,
  the host must bound frames in flight itself, and the CPU reads some
  GPU results (auto-exposure luminance, thumbnails) that need a
  deliberate readback design.
- **Stage 0 needs a running FH2 on the handhelds, not only on the
  desktop.** It comes from today's ShiftGlue, running the TU v2 image on
  the existing PM4 path through Pinyon's Android and Deck pipelines. The
  FH1 numbers above are only a proxy. The one FH2 handheld number
  (fh2-recompiled: 20-30 fps in a heavy race on the Galaxy S23, the
  Odin's chip) says nothing yet about which side binds.
- **Recent recomp work changes details, not the direction.** Three
  ReXGlue ports now render at the D3D or engine level. Their claimed
  gains are against Xenia-style emulation, which Pinyon's executor had
  already left behind. Lavender Shift takes:
  - re:Blue's BSD-3 D3D layer as a code base for everything except
    command buffers;
  - DolRecomp's liveness masks as the model for the trusted-ABI static
    pass;
  - pipelines queued at asset load;
  - worker pools that never wait on a descheduled thread.

  ReXGlue upstream (v0.10.1) has no speedups. Its forks and the game
  repos that carry the SDK add:
  - runtime hygiene: sub-millisecond sleeps, fewer XMA wakes, and
    SPIR-V freed after use (-167 MB RSS in FH1, claimed);
  - three FH2 bring-up fixes from fh2-recompiled;
  - a list of correctness fixes ShiftGlue lacks.

  One new lever needs a decision: a 30 Hz guest (FH2's source rate)
  with in-between frames, either re-drawn (re:Blue, Xerenge) or
  generated from FH2's velocity buffer (fh2-recompiled). See
  [Recent recomp work](#recent-recomp-work-what-to-adopt).

## What FH2 is

Measured from the dumps in `E:\roms\xbox360` on 2026-11-09. Executables
were decrypted and patched with Pinyon's archive tool (the ShiftGlue
`.xexp` code path); disc listings come from Pinyon's XDVDFS reader.
Functions are counted from `.pdata`. Work files and scripts are local
only and are not committed: they hold game data.

**FH2 is the FH1 engine on a newer XDK, built from code at or after
FH1's v4 update.**

- **Lineage.** The PDB path is `...\Forza2\Game\exe\Xbox 360\Release_Final\`,
  the same engine tree as FH1 under the codename "Anthem". The compiler
  and linker builds are identical (LINK 10.0.11886), and so is the
  middleware: FMOD Ex ("FMOD Xenon Output"), Anark Gameface 4.0.7, RAD
  Granny, SQLite, zlib, libpng and libjpeg. There is no Havok, Bink,
  Scaleform or Wwise. Of FH2's 8,210 RTTI type names, 7,309 also exist
  in FH1 v4.
- **XDK.** 28 static libraries at 2.0.21256, against FH1's 2.0.21119.
  The set is FH1 v4's minus MFPLAT, including the XBC, XLRC and XAUTH
  libraries FH1 gained only in v4. D3D9, D3DX9 and XGRAPHICS are linked
  in, so the D3D seam is a newer build of the same library.
- **Shared code.** Each function's bytes are hashed with branch
  displacements, `lis` immediates, r2/r13 offsets and inline jump tables
  masked (mask B). Against FH1 v4, 81.0 % of FH2's functions and 70.5 %
  of its code bytes match. With every D-form displacement masked, the
  match is 88.9 % and 80.6 %. Against FH1's base disc only 76.3 % of
  functions match, so the v4 knowledge is the one that transfers.
  Unique matches stay in address order 96.7 % of the time.
- **Symbol transfer.** Pinyon's v4 address mapper, run unchanged, maps
  163 of FH1's 193 v4 symbols and hook sites into FH2. Cross-game
  correctness is not checked. The unmapped semantic symbols are mostly
  FH1's collectible activities.
- **Content.** Formats are the same throughout:
  - zip archives with XMem/LZX members (678 archives, 260,445 members,
    5.93 GB);
  - `.xds` textures, `.carbin`, Granny `.gr2`, Anark UI scenes;
  - FMOD FSB4/FEV1 banks;
  - WMV video (19 files, not Bink).

  168 of the 174 `.fxobj` shader files are byte-identical to FH1's, so
  Pinyon's shader corpus and AOT pipeline work carries over almost
  whole. New in FH2: car recordings (`.fcr`, `.fcx`) and navigation data.

| | FH2 USA | FH2 + TU v2 | FH1 base | FH1 + v4 |
| --- | --- | --- | --- | --- |
| Title ID / media ID | 4D530AA4 / 7C7779D1 | | 4D5309C9 / 2DC7007B | |
| Version | 0.0.0.12 | 0.0.2.12 | 0.0.0.10 | 0.0.4.10 |
| Image size | 24,117,248 | 24,117,248 | 23,199,744 | 24,313,856 |
| `.text` bytes | 15,735,468 | 15,743,052 | 14,738,996 | 15,373,220 |
| `.pdata` functions | 64,287 | 64,306 | 57,127 | 60,903 |
| Imports | `xboxkrnl` 206 + 13 variables, `xam` 182 | same | 209 + 13, 174 | 175 `xam` |
| Build date | 2014-08-23 | 2014-10-03 | 2012-09-04 | 2013-03-31 |

ReXGlue finds 85,237 functions in the same image (fh2-recompiled);
`.pdata` undercounts, as it did on FH1.

**The title update.** TU v2 is the last FH2 360 update for every media
ID in the Xbox Unity community database (v1 and v2 only). The package
we have, "(World) (v2) (54FD9324)", is an STFS update with three `.xexp`
deltas (`default` 2.1 MB, two facades 0.4 MB each). It also carries a
`media.zip` with 287 members (strings, XML, configuration, UI scenes)
and voice-over banks in ten languages. **The update changes data as
well as code.**

- It was built for another pressing: delta source 0.0.0.9, media
  54FD9324. Applied to our USA disc (7C7779D1), its page digests
  verify:

  | Module | Pages verified |
  | --- | --- |
  | `default` | 368/368 |
  | `XMediaFacade` | 53/53 |
  | `SpeechFacade` | 44/46 |

  The two failing `SpeechFacade` pages are code the patch copies from
  its own base, which differs on our disc. The update changes only 14
  words of that module, mostly imports and timestamps. FH1's v4 update
  was in the same position and applied exactly.
- **Code change, base to v2.** 256 new functions (234 KB, 1.6 % of
  code), against 7,763 (2.71 MB) for FH1's v4. Only 575 functions keep
  their start and length, with 1,494 distinct address shifts. Base and
  v2 are separate address maps even for a small update, so supporting
  both would repeat FH1's cost in full.

**DLC.** None for the 360 version. Turn 10 announced car packs for Xbox
One only, and Storm Island was Xbox One only.

**Fast & Furious** is a separate title (4D530AB5, media 11BC0A01). Its
TU v1 is a delta against a base we do not have (it was a free
download), and its base image is about 94 % of FH2's size. It is out of
scope: a second title is a second image.

## Where FH1's frame goes

Odin 2 Portal, race at 60 fps, run `xa2`, base image, 2026-10-13
([NN, Where the overhead is](https://github.com/arcanite24/pinyon-shift/blob/dev/docs/NEAR_NATIVE_RESEARCH.md)).
NN's later stages brought the process to 32.5-34.4 ms (NN-1.9). The split
is what matters here.

| Bucket | ms a frame | Notes |
| --- | ---: | --- |
| Whole process | 38.80 | fit 28.7 ms + 6.97 µs per draw, 1,396 draws |
| Guest threads | 24.01 | render 5.33, simulation 5.76, FMOD 4.86 (mixer 4.46), jobs 1.33, other 6.73 |
| PM4 decoder, recording inline | 8.92 | fit 1.73 ms + 5.17 µs per draw |
| GPU submission | 3.08 | 7.79 ms busy wall time |
| Pipeline compiles | 0.51 | before the AOT catalog |
| XMA worker and host audio | 1.16 | |
| GPU | 15.28 of 16.7 | main 4x scene in 17-23 render passes, split by uploads |

By library, guest code is 48.0 % of process cycles, the GPU backend
19.3 %, kernel 9.1 %, runtime 8.2 %, libc 6.7 % and Turnip 4.5 % (NN-0.1).
The Deck spends 59-74 ms of process CPU at 3,400-4,500 draws. Recorder,
decoder and submission take 32-38 ms of that, and its APU sits at its
15 W limit (NN-0.2).

What a native build would spend instead (NN, est.):

| Layer | FH1 on ShiftGlue | Native | Distance on the Odin |
| --- | --- | --- | --- |
| CPU code | registers written through to `ctx`, save/restore helper calls, `volatile` byte-swapped accesses, indirect dispatch, per-site FP-mode toggles | the game compiled natively | 1.5-2x native instructions; 4.3-8.4 ms of bookkeeping |
| GPU | the guest D3D runtime writes PM4; the host decodes it, re-derives every draw, records Vulkan, emulates EDRAM and untiles predicated bins | the engine records Vulkan directly at 0.6-1.0 µs a draw | 7.5-8.1 ms decoder, 2.1-2.6 ms submission |
| Runtime, kernel, memory | kernel objects behind one global lock, guest polling, a 512 MB GPU mirror, write-watch page protection | OS events and atomics, memory sized to use | 1.5-4.0 ms; 512 MB |
| Audio | FMOD Ex translated at 5.1 and folded to stereo, XMA through FFmpeg's API, emulated XAudio2 | native stereo mix, one output path | about 2.5-4 ms |

### What Pinyon already proved about the renderer

[D3D_RENDERER_RESEARCH.md](https://github.com/arcanite24/pinyon-shift/blob/dev/docs/D3D_RENDERER_RESEARCH.md)
(DL) tried a D3D-level renderer inside ShiftGlue, beside the PM4 path.
It failed its gate: process CPU -0.9 ms against -3 ms. The reasons matter
for Lavender Shift because they are not the D3D-level idea's fault:

- **The PM4 path still ran.** The guest D3D runtime kept writing PM4 and
  the decoder kept walking it, so the slice removed only part of the
  decoder (-0.27 to -1.03 ms).
- **Validation was removed and it still did not pay.** DL-2.1's first
  list compiler checked every list word at every run. That added 3.21 G
  cycles while packet dispatch fell from 3.07 to 0.80 G. The second
  attempt replaced the check with page watches, so an unwritten list was
  not read at all. The decoder fell 0.79 ms, but process CPU did not
  move (+0.01 ± 0.32 ms). What remained was the PM4 path around the
  lists and the backend draw path.
- **The host draw path stayed.** About 60 % of per-draw cost remains in
  pipelines, constants, descriptors and the draw (1.6-1.8 µs a drawn
  draw), plus 2.0-2.8 ms of driver emission. The executor that consumed
  the description was the Xenos-shaped one.
- **The ceiling was measured.** With the PM4 path skipping derivation,
  recording and submission for the visible draws, Odin process CPU fell
  6.28 ± 0.42 ms (DL-1.1). That is before paying for native draws. The
  probe still walked every packet, which understates the ceiling by
  perhaps 1-2 ms. It also dropped uploads a renderer would keep, which
  overstates it by about as much.

The seam itself was found and works. The engine draws through its own
virtual class (`CD3D9GraphicsDevice`, 67 slots) that reaches every D3D
draw entry point. Display lists run through `D3DDevice_RunCommandBuffer`
after per-object fixups go through D3D's fixup dispatcher, one setter per
resource type. A draw description built from the device's shadow state
and dirty masks matched 4.68 M HUD draws with 0 mismatches (DL-1.2).
The records are in Pinyon's `site/findings/rendering.toml`, group "D3D
inside the game".

## What exactness cost FH1

Each row was measured in Pinyon Shift and not taken, or taken only in a
safe and smaller form, because it was not exact. Under the
[ground rules](#ground-rules) each becomes a candidate. "Ceiling" is a
deliberately wrong knock-out.

| Technique | Speed at stake | What broke or blocked it in FH1 | Lavender Shift |
| --- | --- | --- | --- |
| Trust callees' ABI for `ctx` publish sets | ceiling 23.7 % render, 25.4 % simulation, 29.6 % mixer instructions (CS-0.2, no spill charge); after NN-1.10's measured spill clawback (40-68 % arm64, 85 %+ Deck) and the 0.52-0.6 instruction-to-CPU elasticity, about 3-8 % of guest CPU (est.) | 48,101 indirect call sites without proven targets; the proven net was 0.9 % | **Take**, with an exception list from the poison build; count spills first (LV-0.5) |
| Volatile registers dead after a call | 8.2-13.9 % of static host instructions (est., NL model) | Poison build crashed in 2 s: one caller leaves r6 stale and the callee stores through it (NL-0.1) | **Take**, with that call site and any others the poison build finds as exceptions |
| Non-volatile registers as locals, no publish | unsafe form: -4.3 % static; the exact form was 20-27 % *slower* because it published at every call (NL-1.4) | 13 functions reached through indirect calls read a caller's register through `ctx` | **Take** the unsafe form with exceptions |
| One FP mode, no per-site toggles | FP-mode writes were 65-71 % of one physics routine's cycles before NN-1.1 and NN-5.1 took fixed mode on arm64 (FPCR writes 246 → 19) | Permanent FTZ/DAZ risks integer punning through 26,288 `lfs`→`stfs` copies; fixed IEEE mode was correct but slower on x86 (+0.35-2.40 ms) | **Already taken on arm64.** x86 is open: choose there in LV-0.6 |
| XenonRecomp's vector math shortcuts (`dp_ps`, unfused multiply-add) | under 1 % | Collision compares `vmsum` results; fh2-recompiled saw FH2 collision break and ported ShiftGlue's exact intrinsics | **Do not take.** Here exactness is playability |
| Guest fences complete early | exact completion, even asynchronous, costs Odin +26-31 %, Deck +122-127 % frame time (CS-0.4) | — | **Keep early completion**, with a host bound on frames in flight and a readback design (see [Frame pacing](#frame-pacing-fences-and-readbacks)) |
| Display lists replayed without validation | not an exactness block: DL-2.1's page-watch version had no per-run check and still measured +0.01 ± 0.32 ms process CPU, because the PM4 path still ran | Rare non-API writes into lists: 0.01-0.02 a frame (DL-0.1) | **Take**, inside a renderer with no PM4 path |
| Native-endian guest memory | 5-14 % bound; `bswap` is 4.7 % of instructions | Every host component would need both byte orders | **Do not take**; byte swapping stays |
| No `volatile` on guest accesses | Odin -4 to -6 % per call in a native region; plain stack accesses 7-9 % slower on desktop | No correctness failure recorded | **Measure** per access class on arm64 (LV-0.5) |
| Draw each tiled draw once, full-size target | skipping later tiles: ceiling 8.8 ms of the Odin 1x frame; host-merged untile already took it on the Odin (32.0 → 29.2 ms at 4x); on the desktop it was CPU-neutral and it is set per preset; whether the Deck's 3,400-4,500 draws include tile replays is not recorded | The game's own non-tiling mode drops the static world | **Take** at the API on every device: one logical draw, one host draw |
| Resolves as aliases, memory copy only when the CPU reads | 2.41 ms ceiling on UHD 620 | The 18 reflection-cube faces and two textures read through memory | **Take**: cube faces resolve straight into the host cube, the other readers are listed by the census |
| Fewer host MSAA samples | 3x race 29.1 → 22.3 ms; Odin 18.69 → 16.19 ms | an accepted fidelity trade | **Preset**, as in Pinyon |
| Occlusion from the previous frame | oracle -2.2 to -3.2 ms main pass on the Odin; the cost is vertex and binning work, not shading | objects vanish for a frame as they appear | **Take** as two-phase culling, which avoids most of the popping (see [GPU work](#gpu-work-the-limit-on-handhelds)) |

## Prior art

| Project | CPU | GPU | Version policy | Licence | Result |
| --- | --- | --- | --- | --- | --- |
| [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) (Sonic Unleashed) | XenonRecomp, all 8 register options on | about 42 D3D functions replaced by host code; ring and `Vd*` stubbed; no EDRAM, no PM4 | base + TU required, `.xexp` applied at recompile and install | GPL-3.0 | 60 fps on the Deck at 13-14 W (third-party report) |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), [plume](https://github.com/renderbag/plume) | PPC → C++ | Xenos → HLSL → DXIL/SPIR-V ahead of time; RHI for D3D12, Vulkan, Metal | `.xexp` support | MIT | game-specific by design |
| MarathonRecomp (Sonic '06) | XenonRecomp fork, all options off | UnleashedRecomp's renderer; tiling forced off | base + TU | GPL-3.0 | not released |
| SpeedBreaker (NFS Most Wanted) | XenonRecomp, all options off | its own PM4 processor on Vulkan | USA disc | GPL-3.0+ | Deck 60 fps claimed |
| CoD3Recomp (ReXGlue) | ReXGlue | D3D device plus a non-executing reader for packets copied into lists | — | — | 91 → 182 fps in one scene (DL prior art) |
| [fh2-recompiled](https://github.com/Gabrick75/fh2-recompiled) (FH2) | ReXGlue 0.10, CR/XER/CTR/non-volatile as locals | Xenia emulation, or a native Vulkan renderer that still reads PM4 | base disc only (0.0.0.12) | GPL-3.0 (SDK BSD-3) | Galaxy S23: native 20-30 fps in a heavy race, emulated 12-17 |
| deivid22srk/ForzaHorizon2Recomp-Android | XenonRecomp | planned | retail base | MIT | scaffold; 84.4 % of 2,918 shaders translate |
| [re:Blue](https://github.com/zolaware/reblue) (Blue Dragon) | ReXGlue | D3D hooks (device, draw, resource, shader, state) on plume, D3D12 or Vulkan; bindless; PSO predictor; re-drawn in-between frames | — | BSD-3 | Deck steady 30 fps at Medium, about 11 W (claimed) |
| [Xerenge](https://github.com/shipa-2/Xerenge) (Burnout Revenge) | ReXGlue | D3D calls hooked, drawn through plume; never parses PM4 or emulates EDRAM; re-drawn frames; ETC2 cache | — | GPL-3.0 | Linux, Windows, Android, macOS, Xbox One |
| [Skate 3 Recomp](https://github.com/mchughalex/skate3recomp) 2.0 | — | engine-level renderer reading guest entities; hand-ported material shaders | — | none | over 2× the frame rate at about ¼ GPU power (claimed) |
| [FH1-ReImagined](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined) (FH1) | ReXGlue fork | Xenia | base or TU4 + DLC | GPL-3.0 | Deck about 15-20 fps free roam, about 8 fps in an 8-car race (v0.1.27) |
| Pinyon Shift (FH1) | ShiftGlue | Xenia-derived PM4 path with a game-specific executor | base and v4 side by side | BSD-3 | Odin and Deck hold 60 fps with quality presets |

What the FH2 projects found that Lavender Shift uses (facts only; their
code is GPL and is not copied):

- FH2's `default.xex` recompiles to 85,237 functions under ReXGlue.
  Upstream XenonRecomp found 0 switch tables, 64 unknown mnemonics and
  about 128.5 k functions, so it over-splits.
- About 920 draws a frame go through D3D's draw functions, of about
  2,350 the GPU runs. The rest come from about 650 indirect-buffer runs
  a frame of buffers recorded at run time. Each is reused about 22
  times, and about 3 % change between runs.
- Shaders ship as 174 `.fxobj` files holding about 2,921 containers.
  XenosRecomp needed fixes for FH2: undeclared vertex fetches, D3D's own
  shaders, `SV_VertexID` in `r0.x`, the `cube` swizzle, 10_11_11 inputs
  and `exp_adjust`.
- The Kinect modules `XMediaFacade` and `SpeechFacade` load with
  `XexLoadImage`.
- Forcing flush-to-zero everywhere made car parts flicker, so VMX code
  keeps denormals.
- Collision fails with inexact vector math.

## Recent recomp work: what to adopt

A survey on 2026-11-09 covered:
- the [recomp.fyi](https://recomp.fyi/) tracker;
- the hedge-dev, RT64 and N64Recomp repositories;
- the ReXGlue ports with their own renderers;
- DolRecomp;
- Xenia Canary's commits since October;
- Mesa's feature table.

"Claimed" marks a number a project states that we did not reproduce.

**Where the scene stands:**
- **hedge-dev's tools are quiet.**
  - XenonRecomp's and XenosRecomp's last commits are from August 2025.
  - UnleashedRecomp has had only fixes since its macOS release.
  - The active piece is plume (MIT), which RT64 now also uses.
- **ReXGlue is where the 360 work happens.**
  - recomp.fyi lists 262 ReXGlue projects, 62 of them started in the last four weeks, against 35 on XenonRecomp.
  - ShiftGlue appears there as a toolchain of its own.
- **Three ports now render at the D3D or engine level** instead of emulating Xenos:
  - re:Blue (Blue Dragon, BSD-3, by ReXGlue's author);
  - Xerenge (Burnout Revenge, GPL-3);
  - Skate 3 Recomp 2.0 (no licence).
- **Their gains do not carry over as numbers.** They are measured against Xenia-style emulation:
  - Skate 3 claims over twice the frame rate at about a quarter of the GPU power.
  - re:Blue claims a steady 30 fps on the Deck at Medium and about 11 W.
  - FH1-ReImagined, on the Xenia path, shows that baseline: about 8 fps in an 8-car race on the Deck.

  Pinyon's executor already removed most of that GPU cost: its resolve and transfer ceilings on the Odin are 0.15-0.28 ms. So the multipliers do not transfer to Lavender Shift; the code and the designs do.

| Candidate | Where | Evidence | For Lavender Shift |
| --- | --- | --- | --- |
| A D3D-level layer to start from | [re:Blue](https://github.com/zolaware/reblue) `src/gpu` (BSD-3): device, draw, resource, shader and state hooks; bindless resources; a frame ring; native texture mirrors; occlusion; a PSO recorder, predictor and precache; D3D12 or Vulkan through plume, MoltenVK on macOS | source read | **Adopt as a source.** Its licence matches ShiftGlue's, so code can be taken with attribution. It has no command-buffer path, so recording, replay, fixups, asset lists and clones stay Lavender Shift's own work. LV-0.18 reads it against the seam list. |
| Re-drawn in-between frames | Xerenge `plume_framegen` and `plume_interp` (GPL-3), re:Blue `frame_interp` (BSD-3), RT64 (MIT) | shipped, off by default. Draws are matched across frames by shader, buffers and index range, and camera cuts are handled. No CPU numbers are published | **Maintainer's decision** (see [Decisions](#decisions-for-the-maintainer)). FH2's source rate is 30 fps. The guest runs at 30 Hz and the renderer draws every other frame from interpolated constants. Guest work per shown frame drops by however much of it scales with frame rate (LV-0.19 measures that); GPU work per shown frame stays the same. Costs: one guest frame of input latency against a 60 Hz guest, and draw-matching artefacts on particles, skinned meshes and UI. It needs the D3D-level renderer. |
| Static register-liveness masks | [DolRecomp](https://github.com/ExpansionPak/DolRecomp) `--native-abi` (GPL-3): per-function input, output, escape, may-define and must-define masks; native call edges pass only live registers | code read, no numbers | **Adopt the design** (our own code) as the trusted-ABI static pass. The masks find most exceptions before the poison build runs. Passing live registers as C++ parameters stays optional: NN-1.10 bounds it at 1.4-2.0 %. |
| Fast `vmsum` (single-precision dot product) | Xenia Canary x64, on by default since October 2026 | in Xenia, the exact form is 3× slower and makes `XMVector4Transform` 2.9× slower | **Do not take.** It is the XenonRecomp shortcut that broke FH2 collision in fh2-recompiled, and Pinyon measured that class of shortcut at under 1 %. |
| Table-gather `vexptefp` and `vlogefp`, inline `fres`, FLOAT16 pack and unpack | Xenia Canary x64 | 26.5 → 2.5 ns per op | Only if the LV-0.3 profile shows these ops: a per-op saving needs volume. |
| Binding model | Mesa features (main). Turnip has graphics-pipeline libraries, descriptor buffers, multi-draw, dynamic-rendering local read and present wait. It lacks descriptor heaps, shader objects and device-generated commands, which RADV has | feature table read | On every device: bindless sets bound once a frame, plus push constants. Pinyon already has push descriptors (PB-2.1). LV-0.14 measures descriptor buffers and heaps rather than assuming them. Proprietary Adreno and Mali drivers are unknown. |
| Worker pools that wait on nobody | Xerenge `plume_parallel.h` | on Xbox One, a descheduled worker cost 14 ms a race frame until the pool was capped | **Adopt** for any parallel recording: the caller works too, there is no fork-join over a fixed worker count, and the pool leaves cores for the title, audio and present threads. |
| Pipelines queued at asset load | UnleashedRecomp (model and shader-list loads, plus an embedded list), re:Blue (priority and background lanes), MarathonRecomp | source read | **Adopt** behind the AOT catalog: a catalog miss is queued when the asset that needs it loads, not at its first draw. |
| Upscalers on Vulkan | FSR SDK 2.4 (2026-10-28): FSR 4.1.2 upscaling and frame generation on Vulkan. Snapdragon GSR2: open source, upscaling only. Qualcomm Adreno Neural Fusion: licence unclear | vendor claims | Candidates for the sub-1x presets on the Deck and Adreno. Pinyon's upscaling backlog carries the integration. |
| KosmicKrisp instead of MoltenVK | Mesa (MIT): Vulkan 1.4 conformant, macOS 26 on Apple silicon; RPCS3 ships it | claimed | **Probe** on the Mac mini (macOS 26): pipeline compile time and frame time against MoltenVK. |
| Dynamic-rendering local read on tilers | Vulkan extension; Turnip and RADV | — | Candidate for passes that read the previous pass's pixel at the same position, so they stay in tile memory on Adreno. Example: the sun-shadow mask reads camera depth. LV-0.13 lists the candidates. |
| BOLT or hot/cold splitting | LLVM BOLT on AArch64 | no game case found | Only if LV-0.3 shows front-end stalls (i-cache, iTLB) on the Odin. BOLT is not in the NDK (NN-1.9), so it needs a host LLVM build. |

**Already done, measured, or ruled out:**
- **MOVBE and x86-64-v3:** Pinyon already builds an x86-64-v3 variant (NL-1.8).
- **POSIX waits on a futex epoch:** Xenia Canary measured -21 % of cycles in Lost Odyssey. ShiftGlue's event-driven POSIX waits, on by default, already do this.
- **Inline `__savegprlr` and `__restgprlr`** (Xenia Canary): ShiftGlue's `host_local_register_saves` already does this.
- **ADPF hint sessions:** in two 20-minute soaks on the Odin, the hint made no difference to throttling (A60-5.2).
- **Native FMOD DSP loops:** ruled out, because they would replace FMOD's internals rather than its documented API (LEGAL.md).
- **Hand-ported material shaders (Skate 3):** ruled out as a hand translation of game code (LEGAL.md).
- **Xenia Canary's GPU fixes** (memexport writeback, textures queued once, persistent shared memory): apply them to the Stage 0 reference build only if LV-0.3 shows those costs, since that path retires.
- **General LLVM lifters** (Biotite, Elevator, MirrorBall): ruled out. They run at about QEMU's speed and use no ABI knowledge.

### ReXGlue upstream and forks

A second pass on 2026-11-09 read:
- ReXGlue upstream against ShiftGlue (`101ad468`);
- 465 branches across 228 forks;
- about 45 game repos that carry their own copy of the SDK.

Every fork checked keeps the BSD-3 licence; game repos vary. None of the gains below were reproduced here.

**Upstream has no speedups for us.**
- v0.10.1 (2026-11-06) is 25 commits past our merge base (v0.10.0). ShiftGlue already has its own versions of the useful codegen changes: `stwcx.` as a compare-and-swap, and `vpk` aliasing.
- **Do not take `bd833a2a`.** It emits an `mfence` for every guest `sync` and `eieio` on x86. ShiftGlue fences on arm64 only, on purpose.
- **Take the merged correctness fixes:**
  - content files truncated through the open handle, which protects saves;
  - content results tested with `XSUCCEEDED`;
  - leaked sockets, events and completion ports.
- **Open PRs worth taking:**
  - #558, a high-resolution vblank timer with `WAIT_REG_MEM` polls woken by the vblank. It took Dead or Alive 4's vblank interval from 14.6-32.3 ms to 16.53-16.81 ms. FH2 needs this general form for LV-0.12, because Pinyon's vblank deadline wait is FH1-only.
  - The gap-fill fixes #554 and #461, for FH2's function discovery.

**For FH2's reference build** (fh2-recompiled's `sdk/` is BSD-3):
- Publish the ring read pointer after each block, with a wake, and hook FH2's ring-room spin (`5ccb81a`). This took the menu from 8.6 to 9.9 fps and fixes a `WAIT_REG_MEM` deadlock in the middle of a burst. ShiftGlue writes the pointer only after the whole burst.
- Cap MSAA only for surfaces with a pitch of 1024 or more: 6.8 → 14.1 fps.
- `XamXStudioRequest` must return `E_INVALIDARG`, or FH2's garage sticks on its loading screen.
- fh2-recompiled also generates presented frames from FH2's own velocity and depth buffers (30 → 60 fps presented). This is the image-space alternative to re-drawn frames, and it needs no draw matching.

**For the shared runtime and renderer:**

| Change | Source | Evidence (claimed) | Where it lands |
| --- | --- | --- | --- |
| Sub-millisecond delays really sleep, spinlocks park on `WaitOnAddress`, and repeated yields back off | Xerenge's SDK (BSD-3); Switch ports (ideas) | an audio worker used a third of a core; other threads 80-90 % of a core; idle workers 13-29 % each | [Kernel](#kernel-and-runtime). ShiftGlue's `XThread::Delay` truncates to whole milliseconds and then yields. Count FH1's and FH2's calls first |
| Wake the XMA worker on a kick only when it sleeps, and keep a mask of enabled contexts | three ports (ideas) | 10 k kicks a second were 15-21 % of a thread | [Audio](#audio-and-video). ShiftGlue wakes on every kick and sweeps 320 contexts |
| Free each shader's SPIR-V once its module exists, and cap the driver pipeline-cache data held in memory | ReIGlue (FH1-ReImagined's SDK, BSD-3) | FH1 RSS 1,204 → 1,037 MB, PSS 942 → 781 MB | [Memory](#memory); applies to the AOT catalog too |
| A record thread only with 6 or more physical cores | ReIGlue measurements | FH1 free roam -11.4 % on 4 cores without SMT, +5.3 % on 8 | Supports placing work per device; Pinyon's presets force it on |
| PGO for the renderer, not only the generated code | a Crackdown port's SDK patch (BSD-3) | command processor 1.30 → 1.14 µs a draw | [CPU build](#cpu-shiftglues-generator-with-a-trusted-abi-profile): the new draw path is a PGO candidate |
| Uploads moved ahead of the render pass, measured on more devices | ReIGlue, upstream PR #536, Gears of War 2 port | FH1 on the desktop at 144 Hz: 52 k → 10.6 k render passes a second, and with two more patches the race 72.8 → 97.2 fps. Gears 2 on a Galaxy S25: 55 → 26 passes a frame | Pinyon rejected it on the Odin only (GPU pass P1); it stays a per-device measurement |
| Structured control flow and fused pixel math from the shader translator | sa2-nx family (Switch); a Naruto port tuned on Intel GPUs (BSD-3) | -15.6 % GPU time on PC; +21 % at 720p on Intel integrated graphics | LV-0.8, as requirements on either translator (check the sa2-nx part's licence before reading its code). ShiftGlue has `spirv_fast_pixel_math`, off on Windows and Linux and never tried on the UHD 620 |
| No `volatile` on guest memory | masseffect-nx | +15-20 % fps | Conflicts with Pinyon's measurement: plain stack accesses were 7-9 % slower on the desktop. LV-0.5 decides per access class |
| Opt out of Windows power throttling (EcoQoS) | a GTA V port | about +3 % fps | one Win32 call; desktop A/B |
| Asynchronous log writer; rumble on a worker thread | a Far Cry 3 Switch port; Proving Ground's SDK patch (BSD-3) | log writes caused 19 of 54 stutters on a Switch SD card. On the Deck's virtual pad, the rumble caller stalled 60 s, against 0.2 ms after the fix | [Hitches](#hitches). ShiftGlue rumbles under the input lock, and FH1 rumbles about 120 times a second |

**Only for the Xenia path** (the reference build, and Pinyon until LV-1.6):
- shared-memory "hot pages" (upstream PR #549: NFS Carbon's race 87 → 147 fps on an RTX 5080), worth it only if FH2 takes many more write faults than FH1's 7 a frame;
- skipping transfers into targets that a draw fully overwrites;
- stencil-bit transfers through compute plus conditional rendering.

**What the forks show about renderers.**
- Xerenge, PGR4, reNut and the deko3d Switch ports all take the game at its D3D calls, but none publishes a comparison against the emulated path. The new renderer is judged against Pinyon's numbers only.
- ReIGlue's D3D12 renderer ran slower than its Vulkan path: 26.5-37.6 against 51.3-53.7 fps. That supports a single Vulkan backend.

**Correctness fixes ShiftGlue lacks** (both games use them, so they go into ShiftGlue whatever Lavender Shift decides):
- arm64 `mffs` returns round-up and round-down swapped (`context.h:165`);
- arm64 `fctid` and `fctiw` ignore FPCR rounding;
- `vpkd3d128` FLOAT16 loses the sign when the destination aliases the source;
- the vblank and swap paths increment one counter non-atomically;
- driver pipeline-cache data is read while other threads create pipelines, which crashed on Adreno and Xclipse;
- an XMA warm-up frame is played mid-stream: FH1's engine note had 10,022 silent gaps, 1,198 with the fix;
- `SignalAndWait` is not atomic (PR #531);
- string cvars are saved without escaping (PR #519).

## Design by layer

### One ShiftGlue for FH1 and FH2

ShiftGlue stays the only SDK. The new work lands there as options a
title selects, and both games use them.

- **Split by layer, as today.**
  - The SDK holds mechanisms: the generator and its profiles, the kernel
    layer, the renderer for this engine, Vulkan, audio and tools.
  - Each game repo holds its own image's addresses, hooks and findings.
    Pinyon already works this way: its `src/native_renderer` hooks carry
    FH1's addresses, and the SDK's FH1-named code (executor shaders,
    shader pack, frame dump) holds no guest addresses.
- **An engine layer for both games.** The D3D seam (device class, list
  executor, fixup dispatcher, recording brackets, draw entry points)
  exists in both executables; LV-0.2 maps it. The renderer is written
  once against that seam. Each title supplies a table of its addresses
  and vtable slots.
- **Per-title profiles at compile time, not runtime switches.** A layer
  FH2 does not use must not cost FH2 anything while it is off. Pinyon
  saw both failure modes. MMIO store checks are 3.2 % of Odin process
  time (inclusive, NN-0.1), even where they never fire. Statics
  duplicated across shared libraries voided NN-3.1's first run. So:
  - the trusted-ABI profile is a generator option, with its exception
    list in the game's config;
  - the memory macros come in a lean variant: no MMIO checks, no
    physical-view offset;
  - the kernel object model is selected at build time;
  - the GPU plugin is per title. Pinyon already builds a separate
    `librexgpu-fh1`, so a `rexgpu-fh2` plugin carries no PM4, EDRAM,
    mirror or write-watch code.

  Runtime settings remain only for things that cost nothing when off.
  In-run A/B against the PM4 path uses a measurement build that carries
  both paths; shipping FH2 builds carry only the new one.
- **FH2 first, FH1 after.** FH2 uses these profiles from its first
  build. FH1 adopts each one once it has passed on FH2, after its own
  poison-build and route checks.
- **The old path has a deadline.** A D3D-level path running beside the
  PM4 path cannot reach its gain. DL built one inside ShiftGlue and
  measured -0.9 ms, because the PM4 path kept running and the draw
  executor stayed Xenos-shaped. So the PM4 path is the fallback and the
  measuring baseline only while the engine renderer is being built.
  Once the engine renderer covers FH1, both games move to it and the
  Xenia GPU path can be retired. Retiring it saves the SDK about 95 k
  lines of graphics C++ that its contracts (MMIO, physical views, the
  mirror, the PM4 ring, EDRAM) keep alive.
- **One SDK change, two games.** Every SDK change runs both games'
  routes before it lands. A change that helps FH2 and hurts FH1 is a
  per-title option or is not merged.
- **Pinyon keeps its two FH1 images.** That cost stays in Pinyon's repo.
  Lavender Shift's repo has one image.

### Version policy

- **One image:** the USA disc's executables with TU v2's `.xexp` deltas
  applied (0.0.2.12). The installer builds it from the player's own
  files. It checks every page digest (the method Pinyon used for v4) and
  refuses input it cannot patch. The base image is never recompiled.
- **TU data too.** The installer lays the update's `media.zip` and
  voice-over banks over the disc files, so the game sees one content
  tree.
- **`SpeechFacade`** is the one module the 54FD9324 package does not
  rebuild page-exact on our disc (44/46 pages). The plan stubs the
  Kinect modules anyway (LV-0.10). If they turn out to be needed, take
  the disc's `SpeechFacade`, which the update changes by 14 words, or
  the update built for media 7C7779D1 (it exists in the Xbox Unity
  database).
- Every address, hook, symbol and finding refers to that image only.
  There is no offsets file and no base-to-update map.
- The 360 version has no DLC; `xam` content support covers saves and the
  update only.
- Fast & Furious is a separate title and out of scope. Supporting it
  would bring back the two-image problem.

### CPU: ShiftGlue's generator with a trusted-ABI profile

**Why not XenonRecomp:**
- It does not handle FH2 as it is: it finds 0 switch tables, misses
  mnemonics and over-splits functions.
- It has no multi-module support.
- Its vector shortcuts break collision on this engine.

**Why ShiftGlue's generator.** It is BSD-3 and descends from XenonRecomp
already. It has run this engine's code for months, and it carries every
kept Pinyon codegen result:
- the exact 4 KB guest map, CTR and XER as locals (-22.6 % static);
- single-entry functions with host fibers (-7.4 %);
- CR in locals with ABI-bounded syncs;
- `host_local_lr` and `host_local_register_saves`;
- `fold_rodata_loads`;
- the exact VMX intrinsics;
- SEH, `rexcrt` and multi-module support;
- the small code model and LSE.

It is about 23 k lines. Its contract with the runtime is the context
layout, the memory macros and the import thunks, so a lean runtime mode
needs new macros, not a new generator.

**The trusted-ABI profile** (new; this is where the CPU gain is):
- Argument and return registers (r3-r10, f1-f13, v2-v13) still go
  through `ctx` at calls. Every other register is a C++ local: volatile
  registers are dead after a call, and non-volatile registers are never
  published to `ctx`. LR is never stored, and the save and restore
  helpers compile to nothing. Passing arguments as C++ parameters
  instead has a static bound of 1.4-2.0 % (NN-1.10), so it is optional.
- **Spills decide the gain.** PowerPC has 32 GPRs, 32 FPRs and 128 VMX
  registers. x86-64 has 16 GPRs and arm64 31. When a register that lived
  in `ctx` becomes a local, it may come back as a spill to the host
  stack. Pinyon measured that clawback at 40-68 % on arm64, 60-75 % on
  Win64 and 85 % or a net loss under the Deck's SysV ABI (NN-1.10). In
  NN-5.1, 61 of 80 physics functions got longer with every register a
  local. So the profile is applied per function where the spill-charged
  count says it wins, not everywhere.
- `ctx` survives only at listed exceptions, hooks, `longjmp` and
  exception edges, and thread entry.
- A **poison build** writes canary values into every register the
  profile treats as dead, at calls and returns. It traps when one is
  read. Pinyon's NL-0.1 poison build found the r6 case in two seconds.
  It runs over boot, menus, free roam, a race and the map. Each trap
  becomes a TOML exception, such as "share r6 between these two
  functions" or "this indirect target reads the caller's r4".
- A static pass reports registers read on some path before any write
  in the function or its callees, so most exceptions are found before
  a run. DolRecomp's native-ABI masks (input, output, escape, may- and
  must-define per function) are the model; its code is GPL, so ours is
  written fresh.
- **FP:** one host FP mode per thread, with no per-site toggles. Pinyon
  already runs fixed IEEE mode on arm64 (NN-1.1, NN-5.1: FPCR writes
  246 → 19). x86 still toggles; LV-0.6 chooses its mode and measures the
  denormal cost Pinyon saw there.
- **Memory:** byte swaps stay. There are no MMIO checks (in FH1 only 13
  D3D functions touch the GPU register window, and those are replaced)
  and no physical-view compare (see [Memory](#memory)). The dispatch
  table is unchecked and sized to the image. Whether `volatile` stays
  is decided per access class by LV-0.5.
- **Output** stays C++ text. The IR route (Pinyon CS-2) existed to carry
  provable publish sets; with trusted calls, Clang's own SSA promotion
  does that work.
- **Build:** the small code model and a symbol order file. PGO measured
  -0.13 to -0.72 % and needs a profile kept in step with codegen.
  ThinLTO was neutral on the desktop and never measured on the Odin.
  Both stay options to re-measure once the trusted profile changes the
  code. The renderer's draw path is a PGO candidate of its own: a
  Crackdown port's patch took its command processor from 1.30 to
  1.14 µs a draw.

### GPU: a D3D-level renderer that owns the command-buffer API

The renderer is host code that replaces the guest D3D runtime at its API.
The guest D3D runtime never writes PM4 and nothing decodes PM4 at frame
time.

**Seams** (FH1 addresses are in Pinyon's findings; LV-0.2 maps them to
FH2):
- **The engine's graphics device class** carries immediate draws and the
  state calls before them. Most D3D state setters are inlined into the
  engine as stores plus dirty bits, so the draw hook reads the device's
  shadow state and dirty masks, as DL-1.2 did.
- **Resources:** texture, vertex buffer, index buffer, surface, shader
  and vertex-declaration create, lock and unlock.
- **Command buffers:**
  - Begin and End. While a list is being recorded, draw hooks append
    host draw records to a host list instead of writing PM4.
  - `RunCommandBuffer` replays the host list.
  - The fixup dispatcher's setters become bindings: a fixup writes a
    bindless index or a buffer address into the host list's parameter
    block.
  - The asset list loader parses a list's PM4 once, at load, into a
    host list.
  - The clone path copies the host list.
- **Frame and GPU-to-CPU edges:** resolve, set render target, clear,
  present and `VdSwap`, fences and `InsertCallback`, and occlusion
  queries.

**No validation per run.** A host list is invalidated by re-recording, by
a fixup, or by a page watch on the list's memory. FH1 saw 0.01-0.02
non-API writes into lists a frame.

**Draw path.** The target is ≤ 1 µs a draw of CPU before the driver on
the Odin (est.). Pinyon's best complete record cost is 650-760 ns
(NA-2.4). The driver then adds its own floor, and Turnip's submission
alone costs about 2.2 µs a draw (3.08 ms over 1,396 draws). At
1,400-2,350 draws a frame that is 4-7 ms in all on the Odin (est.).
LV-0.14 measures the floor first.
- Vulkan 1.3 with descriptor indexing and buffer device addresses:
  bindless textures and samplers; constants as buffer addresses in push
  constants. Turnip has no shader objects, descriptor heaps or
  device-generated commands (RADV has all three), so the common path is
  graphics-pipeline libraries plus dynamic state.
- Constant blocks are converted to little-endian once, when they are
  set. In FH1, the shader-table constants come from about 290 static
  sources. Per-object constants go through D3D's dirty-constant flush,
  1,805 calls a frame.
- Dynamic rendering, dynamic state for viewport, scissor, depth bias and
  stencil, and as few pipelines as the census allows. Pipeline binds
  will not amortise: the Deck changes pipeline 1,147 times a frame, at
  2.5 draws a run (CS-0.3).
- **Recording where the device has room.** On the Odin, record inline
  on the guest render thread with one submission thread; Pinyon's
  inline recording is how Android runs today. Workers recording
  secondary command buffers are used only where a device has idle cores
  and a measurement shows it pays: small secondaries cost GPU time
  (LS-3.4). Threads are placed per device. Deck placement alone saved
  6 % of process CPU (NN-3.14), and on the Astra an unpinned decoder put
  25 % of frames over 17.5 ms against 2 % pinned. Any worker pool lets
  the calling thread work too and never waits on a fixed worker count:
  on Xbox One, Xerenge lost 14 ms a race frame to one descheduled worker
  until it capped the pool.
- **Render passes.** Draws to the same targets stay in one rendering.
  On Vulkan on the RTX 4080, a per-draw barrier once ended every
  rendering: 4,850 renderings a frame against 297 after the fix, and
  the race went from 31.9 to 19.6 ms (NP-12.4). Moving uploads ahead of
  a pass is measured per device. On the Adreno 740 it made the GPU
  0.21-0.43 ms slower, because those passes render in system memory
  (GPU pass P1).

**Surfaces:**
- Render targets are native and full size: no EDRAM, no 10 MB limit, no
  predicated tiling. The D3D tiling replay is hooked to draw each
  logical draw once.
- Resolves are image copies or aliases. Bytes go back to guest memory
  only for the readers the census lists: FH1's car thumbnails,
  reflection-cube faces and the textures read through memory.

**Shaders and pipelines** are compiled ahead of time from the disc's
`.fxobj` containers, plus the shaders D3D itself creates. The translator
is either ShiftGlue's Xenia-derived one, retargeted to a native constant
ABI, or XenosRecomp (MIT) with the FH2 fixes listed above (LV-0.8
decides). Pipelines come from the census tuples into a catalog built at
install, the way Pinyon's AOT catalog took Odin waits from 6-12 to 0-1.

**Textures** are transcoded at install from the game's packs. Untiling,
byte order and format conversion happen once, into a native pack. At
run time a texture is created from the pack by asset and hash and is
never untiled.

**Vertex and index data:**
- Static buffers are uploaded once, at unlock.
- Dynamic buffers are copied at unlock or at the draw, for the ranges
  the draw reads. Pinyon's two estimates disagree: about 20 MB a frame
  (DL), against about 370 pages, roughly 1.5 MB, re-uploaded a frame
  (NN-3.12). That is the difference between about 0.1 ms and 2-4 ms of
  copy CPU, so LV-0.4 measures it.
- The GPU never reads guest memory in place. That is what lets fences
  complete early.

### Frame pacing, fences and readbacks

FH2 was built for 30 fps, and FH1 needed these fixes to run at 60 (all
in Pinyon's `site/findings/threads.toml`, "Frame pacing" and "Present
and fences"):
- the guest vblank at twice the frame limit;
- the #381 step gate (without it, 9-14 % of frames drift by more than
  4 ms and 3-6 % repeat);
- releasing late swaps (p99 16.5 → 13.1 ms at 120 fps);
- fixes for systems that add a constant per update;
- a render-job throttle (an overfull queue crashed 7 of 7 runs on the
  8 Elite).

These are LV-0.2 transfer items, and the gate is steps per swap at p99.
Pinyon's vblank deadline wait is FH1-only, so FH2 takes the general
form from upstream PR #558: a high-resolution vblank timer that also
wakes `WAIT_REG_MEM` polls.

- **Frames in flight.** The title waits on the previous present's fence
  and so runs at most one frame ahead of the GPU. With fences completing
  early, that throttle is gone. The host keeps 1-2 frames in flight
  itself, and input latency is measured. Present wait is on Turnip and
  RADV; present timing is on RADV and only on Adreno 750 or newer under
  Turnip, so not on the Odin's 740. RT64 turned present wait off by
  default on NVIDIA and Linux, so the choice is per driver.
- **Readbacks.** The CPU reads some GPU results: the 32×32 scene
  luminance resolve for auto-exposure, car thumbnails and loading-time
  occlusion queries. Handing the game previous-frame data for every
  resolve never reached free roam in FH1. Real completion at every fence
  costs +26-31 % on the Odin. So data that tolerates latency (exposure)
  is read back asynchronously, a frame late. Only exact one-off reads
  (thumbnails) drain the GPU, and the gate is drains per frame.
- **Menus.** At Press Start the frame function runs back to back,
  about 950 ms of CPU a second (NN-4.7), so menus are capped.
  FH1-ReImagined parks FH1's main-menu render thread between frames
  (`sub_82DD9D58`, called from `0x8259FB5C`): 28.5 → 0.41 ms a frame on
  that thread, and 55.6 → 6.5 ms for the whole process (claimed).
  Pinyon hooks only the `SleepEx` at `0x8259FBEC`. LV-0.2 maps the
  function into FH2.
- **Refresh rate.** On 144-240 Hz displays an uncapped frame rate scales
  CPU work 2.4-4x. Low-end machines default to a 60 fps cap.

### GPU work: the limit on handhelds

The renderer rewrite itself saves little GPU time. On the Odin, upload
hoisting made the GPU slower, and the in-run ceilings for skipping
resolves and transfers were 0.15-0.28 ms and 0 (NN-2.7; the 2.9-3.1 ms
"emulation structure" is a profiled share, not a saving). Frame-rate
headroom on the handhelds has to come from GPU work the native renderer
makes possible:

- **Occlusion culling.** 54-60 % of FH1's main-scene draws pass no
  samples, and an oracle saved 2.2-3.2 ms of the Odin's main pass (#784).
  The cost is vertex and binning work, so culling before the draw is
  what pays. The plan is two-phase culling: draw last frame's visible
  set, build a depth pyramid, then test the rest. That avoids most of
  the popping of plain previous-frame culling. Object bounds come from
  the install-time conversion of vertex data.
- **Sun shadows.** SMOOTH 60 on the Odin turns shadows off, which saves
  about 6.4 ms. That cost is the camera-depth pre-pass and the
  screen-space shadow mask, not the casters (MR-3, MR-4). A cheaper
  shadow path is the largest single lever for shipping with shadows on.
- **Render scale below 1x.** It is cheap with native render targets. At
  50 %, Pinyon's RS-0 probe saved 29.5 % on the UHD 620, 14.5 % on the
  Deck and 8.5 % on the Odin. Pair it with an upscaler (Pinyon's
  upscaling backlog has FSR, DLSS and XeSS). FSR SDK 2.4 (2026-10-28)
  brought FSR 4.1.2 to Vulkan, and Snapdragon GSR2 is an open-source
  upscaler for Adreno.
- **Tile memory on Adreno.** Some FH1 passes render in system memory
  on the Adreno 740 (GPU pass P1). A pass that reads only the previous
  pass's pixel at the same position (the sun-shadow mask reads camera
  depth) can share one rendering with it through dynamic-rendering local
  read, on Turnip and RADV, and stay in tile memory. LV-0.13 lists the
  candidates.
- **Reflection cube.** The 18 faces resolve straight into the host cube,
  without the round trip through guest memory.
- **Untiling everywhere.** At the API, each tiled draw is drawn once on
  every device. If the Deck's 3,400-4,500 draws a frame include tile
  replays, that cuts them substantially, for CPU and GPU (LV-0.3 counts
  draws by tile pass).
- **Power on the Deck.** At 15 W, CPU and GPU share one budget. LV-0.3
  measures the split, so CPU savings can be counted as GPU clock.
- **Gate:** GPU ms per pass from an FH2 per-pass profile (LV-0.13), not
  frame time.

**Platforms:** Vulkan on every target. MoltenVK on macOS, where Pinyon
already runs; KosmicKrisp (Mesa's Vulkan 1.4 driver on Metal, macOS 26
on Apple silicon) is a cheap probe against it on the Mac mini. No D3D12
backend. plume is optional: one backend does not need an RHI, though
re:Blue's code is written against it.

### Memory

- One 4 GB reservation at a fixed host base.
- The physical heap lives at one virtual range only, if LV-0.7 confirms
  that no guest code outside the replaced D3D functions uses the 360's
  other physical views. That removes the per-access offset compare that
  ShiftGlue keeps on Windows and Apple silicon (64 KB and 16 KB pages
  cannot map the 4 KB-offset view).
- No GPU mirror. On the Odin sparse binding already returned 290 MB of
  it (NN-6.1), so the saving there is about 220 MB; on other devices it
  is up to 512 MB. Host GPU memory holds only host-format resources.
- **Discard texture pages after upload.** The guest still loads texture
  payloads into its heap, and the guest arena is the largest item in
  the Odin's PSS (457.6 MB). Pinyon's NN-6.6 failed only because every
  page was mapped twice. With a single-mapped arena, pages whose data
  went to the GPU, and that the CPU never reads again, can be discarded
  (est. 150-300 MB). LV-0.4 watches for CPU reads after upload first.
- **Shader memory.** Each shader's SPIR-V is freed once its module
  exists, and the driver pipeline-cache data held in memory is capped.
  ReIGlue measured FH1 RSS 1,204 → 1,037 MB and PSS 942 → 781 MB.
- Guest heaps are sized to use (UnleashedRecomp uses o1heap). Pinyon
  had surfaces at 247 MB until it sized them on demand (94 MB). Lazy XMA
  contexts saved it 112 MB of heap.

### Kernel and runtime

A lean kernel layer in ShiftGlue, shared by both games. FH2 uses 206
`xboxkrnl` functions plus 13 variables, and 182 `xam` functions; FH1
uses 209 and 174. Eight of the `xam` imports
are new against FH1 (`XamGetLanguage`, the `XamLrc*` and `XampXAuth*`
families); ShiftGlue only stubs them today.

- Host threads 1:1. Events, semaphores and mutants run on futexes or
  `WaitOnAddress`, with no handle-table lock and no process-wide lock.
  Interrupts-off sections that guard a single reservation become
  atomics; 1,001 of FH1's 1,062 do (NN-3.1). The other 61, and interrupt
  dispatch, keep their own narrow lock.
- Every wait blocks, and timers are native. The title reads
  `KeTimeStampBundle` from memory through its own `GetTickCount`
  (`sub_823F18B8` in FH1, NN-3.7), so that function is hooked to read
  the host clock, instead of a 1 ms host timer ticking the bundle. Waits
  gave Pinyon its largest CPU wins outside the renderer: the fence-poll
  hook -15 %, event-driven POSIX waits -6 to -9 %. Delays under a
  millisecond really sleep (ShiftGlue truncates them to whole
  milliseconds and then yields), spinlocks park, and repeated yields
  back off. Xerenge's SDK found threads burning 13-90 % of a core on
  these.
- Fibers are served at the XAPI level, so functions stay single-entry.
- `xam`: content (saves and the update), profile and achievements, UI
  stubs. No Live, and no Kinect: the facade modules are stubbed if LV-0.10 shows
  the game runs without them.
- ShiftGlue's existing pieces stay where they fit: the XEX loader and
  `.xexp` patcher (with Pinyon's multi-record header fix), the STFS and
  XDVDFS readers, and `xam` content. What changes is the object model
  and the lock underneath them. Because both games use the same kernel
  layer, that change is made once.

### Audio and video

- **FMOD Ex stays translated.** It has no native build for these
  platforms, and Pinyon's policy replaces middleware only at its
  documented API.
- **Output** goes through Pinyon's native path (NN-4.4: XAudio2 pass
  0.49 ms → 0), with FMOD forced to stereo (NN-4.2: -1.0 ms).
- **XMA** is decoded by Xenia's BSD decoder on a worker. Contexts are
  allocated lazily. A kick wakes the worker only when it sleeps, and the
  worker walks a mask of enabled contexts instead of all 320; elsewhere,
  10 k kicks a second cost 15-21 % of a thread.
- **FMOD's own format.** After the stereo fix, FMOD's mixer is still
  2.5-3.0 ms a frame on the Odin (NN-4.2), and muting reverb in the
  game's XML still computes it. FMOD Ex's documented API sets the
  software sample rate and channel count before init
  (`setSoftwareFormat`, `setSoftwareChannels`) and can turn reverb off.
  Estimated -0.7 to -1.2 ms at 32 kHz, as a handheld preset (LV-0.17).
- **Video:** 19 WMV files on the disc (not Bink). They are decoded
  natively through FFmpeg's WMV decoders, replacing the guest player at
  its API (LV-0.11).

### Hitches

A long frame is also a physics jump, because the game folds a long frame
into one large step. Each of these is a measured FH1 cause, and each has
a design answer:

| Cause in FH1 | Cost | Answer |
| --- | --- | --- |
| Driver allocations | 5-20 ms per 64 MB block | pre-reserve heaps and sub-allocate |
| First-use pipelines | 96 of 233 in a first race differ only in inherited state (NL-3.6); MoltenVK 100-190 ms each | AOT catalog; a miss is queued when its asset loads, as UnleashedRecomp and re:Blue do, with graphics-pipeline-library fast linking if it still reaches a draw (Turnip and RADV; not MoltenVK) |
| SQLite views | 158-186 ms per read (FH2 links SQLite too) | run Pinyon's SQL profiler on FH2 |
| The 30-second cache save | — | off the game threads |
| LZX decompression while streaming | — | optionally decompress archives at install |
| Log writes on game threads | 19 of 54 stutters in a Switch port | an asynchronous log writer |
| Rumble under the input lock | FH1 rumbles about 120 times a second; on the Deck's virtual pad another port saw a 60 s stall | rumble applied on a worker |

The gates are Pinyon's cold and warm hitch routes
([HITCH_QUALIFICATION.md](https://github.com/arcanite24/pinyon-shift/blob/dev/docs/HITCH_QUALIFICATION.md)).

### Tools instead of exactness gates

Pinyon's verifiers (lockstep, region oracle, replay) existed to prove
transforms exact. Lavender Shift keeps the measuring tools and replaces
the gates with tools that find breakage fast:

- **Measurement** is built in from day one:
  - per-thread CPU counters;
  - in-run A/B toggles for hot settings, with disjoint blocks;
  - a cross-build A/A;
  - draw-normalised metrics;
  - the Pinyon route runner with pinned seeds.

  Judge CPU work by per-thread instructions over four or more runs, not
  by desktop frame time.
- **The poison build** for ABI exceptions.
- **Frame captures** compared against a reference run, as regression
  alarms, not as gates.
- **RE records:** findings TOML from day one, for the single image. FH1's
  536 findings transfer through LV-0.2.

## Budget (estimate)

CPU on the Odin, race at 60 fps. FH1 today runs at 32.5-34.4 ms a frame
of process CPU (NN-1.9, Stage 1 builds). Pinyon has already taken
FMOD stereo, the native XAudio2 output, fixed FP mode on arm64,
host-merged untiling and the shader-load cache, so those are not
counted again. The figures are savings against that build, and they
are wide on purpose. They apply to FH1-like content; LV-0.3 replaces
them with FH2 numbers.

| Change | Saving (ms, est.) | Basis | Probe |
| --- | ---: | --- | --- |
| Engine renderer: no PM4 decode or derivation, no tile replays, no write watches | 3-5 | DL-1.1 gross ceiling 6.3 ms; the native draw path with Turnip's ~2.2 µs a draw is paid back; the packet walk the probe kept adds 1-2 | LV-0.4, LV-0.14 |
| Lean memory macros: no MMIO checks | 0.5-1.2 | MMIO checks 3.2 % of process, inclusive (NN-0.1) | LV-0.7 |
| Trusted ABI, spill-charged | 0.6-1.6 | 3-8 % of guest CPU | LV-0.5 |
| FMOD software format at 32 kHz, reverb off | 0.7-1.2 | mixer 2.5-3.0 ms after stereo | LV-0.17 |
| Kernel: no process-wide lock, hooked tick count | 0.3-1.0 | 0.55-1.28 ms blocked a frame (critical path more than total CPU) | LV-1.1 |
| **Total** | **about 5-10** | → **about 23-28 ms**, against Pinyon's native estimate of 16-23 ms | |

**Not counted:**
- Replacing the guest D3D code. Pinyon's DL found that state capture
  roughly cancels it, and DL-2.1 measured the render thread +0.12 ms.
- The GPU. The renderer by itself saves at most a few tenths of a ms of
  it; the frame-rate gains are the [GPU work](#gpu-work-the-limit-on-handhelds)
  items, gated per pass.

**Memory:** about -220 MB of mirror on the Odin (up to -512 MB
elsewhere), and an estimated -150 to -300 MB from discarding uploaded
texture pages.

## Stage 0: measure before building

Stage 0 runs on a **reference build**: the TU v2 image recompiled on
ShiftGlue as it is today, with the existing PM4 path, on the desktop
**and on the Odin and the Deck** through Pinyon's Android and Deck
pipelines. fh2-recompiled shows ReXGlue reaches FH2 races, so this is
days, not weeks (est.). The reference build also takes the FH2 fixes
that fh2-recompiled found:
- the ring read pointer published after each block;
- the MSAA cap for wide surfaces;
- `XamXStudioRequest` returning `E_INVALIDARG`.

See [ReXGlue upstream and forks](#rexglue-upstream-and-forks). A
measurement build later carries both the PM4 path and the new paths, so
in-run A/B stays possible.

| ID | Premise | Method | Gate | Size |
| --- | --- | --- | --- | --- |
| LV-0.1 | TU v2 applies to the supported disc | apply the `.xexp` to the USA disc, check every page digest, record the image hashes | **Done for `default` and `XMediaFacade`** ([What FH2 is](#what-fh2-is)); `SpeechFacade` 44/46, decided by LV-0.10 | S |
| LV-0.2 | FH1 knowledge transfers | masked hashing FH1 v4 → FH2 TU v2 (**done**: 70.5 % of code bytes); verify the mapper's 163 of 193 transfers; find the D3D seam functions by call pattern; port matching findings | the seam functions (device class, list executor, fixup dispatcher, recording brackets, draw entry points) are found in FH2 | S |
| LV-0.3 | ShiftGlue reaches FH2 races, and FH2 is measured where it matters | reference build of the TU v2 image on the desktop, the Odin and the Deck with Pinyon's presets: per-thread `simpleperf`, the memory ledger, the Deck's CPU/GPU power split, draws counted at the API by tile pass (fh2-recompiled's 2,350 may count each tile; FH1 decodes about 3,900 draws for 1,890 drawn), i-cache and iTLB miss counters on the Odin | free roam and a race on all three; a budget table with FH2 numbers replaces the FH1 one | M |
| LV-0.4 | A D3D-level renderer sees the whole frame | Pinyon DL-0.1 census on the reference build: draw sources, list origins (asset, recorded, cloned), fixups, non-API writes into lists, CPU-read resolves, format aliasing, tiling, memexport, queries, polled fences; plus per-draw state cost | non-API list writes ≤ 0.1 a frame, no PM4 written outside D3D, every CPU-read resolve listed | M |
| LV-0.5 | The trusted ABI pays after spills | first a spill-charged static count: compile the hot functions both ways on arm64, Win64 and SysV and count host instructions; then generate FH2 with the profile, run the poison build over the routes until no new trap appears, and compare per-thread instructions on the desktop and the Odin; split `volatile` off per access class | -5 % guest-thread instructions or more on the Odin after spills; exception list stops growing | M |
| LV-0.6 | One FP mode is playable | no per-site toggles: IEEE fixed, then FTZ for VMX only, then FTZ everywhere; 10-minute drives with collisions and physics checks; cost on x86 and arm64 | no visible breakage in the chosen mode | S |
| LV-0.7 | The physical views can go | count guest accesses to `0xA0000000`-`0xFFFFFFFF` outside the D3D functions the renderer replaces | 0, or a short list that can be hooked | S |
| LV-0.8 | Shaders compile ahead of time | translate every `.fxobj` container and every runtime shader with both candidate translators | 99 % or more compile; captures match the reference build's look | S-M |
| LV-0.9 | Textures convert at install | census of texture formats and tiling in the game packs; transcoder prototype | every format maps to a native one | S |
| LV-0.10 | Kinect modules can be stubbed | boot with `XMediaFacade` and `SpeechFacade` replaced by stubs | menus, free roam and a race | S |
| LV-0.11 | Video decodes natively | the container is WMV; find the guest player's API, decode one file with FFmpeg | plays | S |
| LV-0.12 | Pacing fixes transfer | port FH1's vblank, step-gate, late-swap and render-job throttle hooks through LV-0.2; host bound on frames in flight | steps per swap at p99 as in FH1; input latency measured | S |
| LV-0.13 | GPU levers on FH2 | per-pass GPU profile of the race on the Odin and the Deck (NN-0.11 method); the #784 occlusion oracle and census; shadow, reflection and resolve costs | a ranked list of GPU ms by pass; occlusion oracle ≥ 1.5 ms on the Odin to build two-phase culling | M |
| LV-0.14 | The driver's per-draw floor | microbenchmark on Turnip and RADV: ns per draw including submission for push constants plus draw, a pipeline switch every 2.5 draws, and descriptor rebinds | a measured floor; the renderer's per-draw target is set from it | S |
| LV-0.15 | Hitches are known | Pinyon's cold and warm hitch routes on the reference build; driver allocations, pipeline misses, SQLite reads, streaming decompression | each hitch over 8 ms has a named cause | S |
| LV-0.16 | Sustained performance | 20-minute race soak on the Odin and the Deck with clocks, temperatures and battery current | sustained frame rate recorded per preset | S |
| LV-0.17 | FMOD runs lighter | one restart pair per setting: software rate 32 kHz, fewer software channels, reverb off through FMOD's API | -0.7 ms or more on the Odin; a listening check | S |
| LV-0.18 | re:Blue's D3D layer is a usable base | read its `src/gpu` (BSD-3) against the seam list and FH2's census: resources, texture mirrors, bindless allocator, frame ring, PSO predictor, occlusion | a list of files taken, adapted or not used, before LV-1.2 starts | S |
| LV-0.19 | A 30 Hz guest frees real budget | the reference build capped at 30 and at 60 on the Odin and the Deck: process CPU per second by thread, the Deck's CPU/GPU power split, GPU clock | the CPU share that scales with frame rate, and the GPU clock gained on the Deck at 15 W; this sizes the re-drawn-frames decision | S |

## Order after Stage 0

1. **LV-1.1 Runtime and codegen.** The trusted-ABI profile, flat memory
   and the lean kernel mode, all in ShiftGlue. FH2 boots to the title
   screen on them, with the PM4 path still drawing. FH1's routes still
   pass with the switches off.
2. **LV-1.2 Renderer.** Immediate draws (menus and HUD), then host lists,
   resolves and the race. Measure per-draw CPU against the 1 µs target
   at each step.
3. **LV-1.3 Audio, video, input, saves.**
4. **LV-1.4 New paths on the handhelds early.** The reference build is
   on the Odin and the Deck from Stage 0. The new paths go there as soon
   as menus render. Pinyon's lesson is to port to a second architecture
   before the design sets (fences, 16 KB pages, shared-library statics).
5. **LV-1.5 Performance passes** on the yardstick, then the player
   features Pinyon has (presets, launcher, settings).
6. **LV-1.6 FH1 onto the shared paths.** Pinyon adopts the trusted-ABI
   profile, the lean runtime mode and the engine renderer, one at a
   time behind its own gates. The Xenia GPU path is then retired from
   ShiftGlue.

## Shared with Pinyon Shift

- **Code:** all of ShiftGlue, including:
  - the generator;
  - the XEX and `.xexp` tools with page verification;
  - the shader translator;
  - the AOT pipeline catalog;
  - the FMOD output path and the XMA worker.

  From Pinyon's repo, the route, A/B and capture tooling and the
  launcher and setup scripts are generalised to take a title rather
  than copied.
- **Knowledge:**
  - 536 findings (423 confirmed), reached through the 70 % shared code;
  - 7,636 recovered vtables;
  - the D3D seam;
  - the file formats, which are identical;
  - the shader corpus (168 of 174 files identical).
- **Methods:** count the premise before building, waits are performance,
  in-run A/B, cross-build A/A.

## Not carried into Lavender Shift

- **Xenia's GPU emulation at frame time:** PM4 decode, register file,
  EDRAM render-target cache, tiled texture cache, the 512 MB mirror and
  the write watches. They stay in ShiftGlue only as FH2's bring-up path
  and measuring baseline, until FH1 moves too (LV-1.6).
- **The base/update pair** and its address maps.
- **Exactness gates as blockers.** The verifiers stay available as
  debugging tools.

## Risks and open questions

- **Engine differences.** Sumo's changes to the FH1 engine may add PM4
  writers outside D3D or more list mutation. LV-0.4 decides; with no PM4
  fallback, anything the renderer cannot see draws nothing until it is
  hooked.
- **Trusted ABI failures surface far from their cause.** The poison
  build is a prerequisite, not an extra.
- **The driver's per-draw floor.** Pinyon measured Turnip submission at
  about 2.2 µs a draw. If LV-0.14 finds a higher floor, the renderer's
  CPU saving falls to the low end of its 3-5 ms. The GPU levers do not
  depend on it.
- **The CPU gains are modest by themselves.** About 5-10 ms of 33 on
  the Odin. On the Deck they mostly turn into GPU clock under the 15 W
  limit. Frame rate on the handhelds comes from the GPU work items; the
  plan should be judged on those as much as on CPU ms.
- **Shader translation bugs** dominated other D3D-level projects' visual
  bugs.
- **Scale.** A lean runtime mode and an engine renderer are more work
  than any Pinyon backlog. The reference build bounds that risk: FH2
  plays on today's path early, and every probe is measured against it.
- **Two paths in one SDK.** Until FH1 moves (LV-1.6), ShiftGlue carries
  the Xenia path and the new one side by side. Every SDK change must
  pass both games' routes. Exceptions and addresses must stay in the
  game repos, or the SDK fills with per-title special cases.
- **Legal.** Pinyon's LEGAL.md policy carries over: no hand translations
  of game code, no copied game data, middleware replaced only at its
  documented API, and GPL projects used for facts and ideas only.

## Decisions for the maintainer

Decided on 2026-11-09:
- **No new fork.** Everything lands in ShiftGlue, compatible with FH1
  and FH2.

Open:
1. **Licence of the Lavender Shift repo:** BSD-3 like Pinyon and
   ShiftGlue (recommended; GPL projects for ideas only).
2. **Reference build** on today's ShiftGlue for Stage 0 (recommended).
3. **Fast & Furious:** out of scope (recommended) or a later second title.
4. **A 30 Hz guest with re-drawn in-between frames**, as an opt-in
   handheld preset. Pinyon's "not to build" list rules out a simulation
   slower than the source rate. FH2's source rate is 30 fps, so this
   would be frame generation on a steady base rate, which Pinyon plans
   as an opt-in (#523-#526). NEAR_NATIVE_RESEARCH.md still files
   RT64-style decoupling under that rule, though, so it needs your call.
   There are two ways to make the in-between frames:
   - re-drawn frames (re:Blue, Xerenge), which need the renderer's
     draw identity;
   - frames generated from FH2's own velocity and depth buffers, as
     fh2-recompiled does, which need no draw matching but show
     image-space artefacts.

   Recommended: decide after LV-0.19 measures what a 30 Hz guest buys.
