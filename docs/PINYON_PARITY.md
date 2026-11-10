# Pinyon parity: what Lavender still needs

This plan lists what Pinyon Shift (FH1) gives players and what it does for
performance, and sorts each piece by what Lavender needs before it can
have it. It was assessed on 2026-11-09.

There are three kinds of work:

1. **Carry:** generic SDK or host code. It needs only a config switch or
   a copy of Pinyon's app code.
2. **Map:** a Pinyon hook on an FH1 address. Pinyon's address mapper
   (`horizon1-recomp/tools/map-fh1-v4-addresses.py`, FH1 v4 image
   against FH2 TU v2) proposes the FH2 site. Each one is then confirmed
   in game.
3. **Find:** no mapped site yet. This needs reverse engineering in FH2
   from scratch.

**Ground rules:**
- **Every setting applies while the game runs, whatever reverse
  engineering that takes.** A row that cannot do this yet stays out of
  the menu.
- **Desktop first:** Windows only for now ([ground rule 5](APPROACH_RESEARCH.md#ground-rules)).
- **Write findings down in the same change.** Each confirmed FH2 address
  goes into the repository's records. LP-0.2 sets up where.

## Where Lavender stands

| Area | Lavender today |
| --- | --- |
| Boot and play | The title, the opening drive and free roam on Windows/Vulkan; no race yet |
| Settings menu | F6 or Back+Start. DISPLAY: fullscreen, aspect, vsync, game frame rate, frame-rate limit, output scaling, sharpening, HDR. GRAPHICS: presets, resolution scale, MSAA, anisotropic, trilinear, texture detail and size, post AA, AA on the HUD. All live ([STAGE0](STAGE0.md#settings-menu)) |
| Shaders | AOT pipelines built from the player's disc before launch ([STAGE0](STAGE0.md#shader-pipelines)) |
| Pacing | Each frame presented once by vsync ([STAGE0](STAGE0.md#frame-pacing)); ring-room wait hook ([STAGE0](STAGE0.md#ring-room-wait)) |
| Renderer | ShiftGlue's FH1 executor, at SDK defaults. Pinyon's GPU work is mostly in already: native EDRAM surfaces, rows on demand, folded clears, specialised EDRAM passes, resolve-to-texture, recorder memos, D3D shader cache, async submission and pipeline binds |
| Codegen | All of Pinyon's generic options: CR/CTR/XER locals, fibers, the CRT, rodata folding, host-local saves and LR, direct calls |
| Input | Keyboard through `mnk_mode`, pads through the SDK |
| Missing entirely | Audio settings, controls pages, gameplay switches, profile and save tools, achievements, mods, launcher and setup, crash reporting |

## Mapped FH2 sites

The mapper places 24 of the 30 FH1 v4 hook sites that Pinyon's settings
and frame-rate fixes use. Mapping methods:

- **exact:** the whole function is identical once branches and addresses
  are masked;
- **aligned:** the run of instructions around the site matches inside a
  function paired through calls;
- **context:** a unique window of instructions around the site;
- **data / data-ref:** taken from address pairs in matched code.

Every site is a candidate (confidence "probable") until a hook shows the
expected effect in FH2. One check passed already: Pinyon's GPU-fence
predicate maps exactly onto `sub_82B72EA8`, the ring-room wait this
project had already found from fh2-recompiled's work.

| Pinyon use | FH1 v4 | FH2 TU v2 candidate | Method |
| --- | --- | --- | --- |
| Bloom scale (`disable_bloom`) | 82494E44 | **82474E2C (runs each frame)** | context |
| Motion blur switch | 82E3A924 | **82F0483C (confirmed)** | aligned |
| Depth of field switches (3 sites) | 8248AB7C, 82487B54, 82487B84 | 8246AC34, 82467C0C, 82467C3C | aligned |
| Render-scenario control load (shadows, reflection rate) | 82E43458 | **82F0D058 (confirmed)** | exact |
| Bool-control vtable | 8224F824 | 8224B584 | data-ref |
| Float-control vtable | 8224F754 | **8224B4B4 (confirmed)** | found: the table of the five mapped functions |
| Camera vertical FOV return | 828FF264 | **829BB414 (confirmed)** | context |
| Main-view aspect return (ultrawide) | 8241228C | 823F2354 | found: the same eleven instructions, unique |
| HUD hidden byte | 833F0F91 | **83449CA2 (confirmed)** | data |
| Launch-options object, vtable | 833D02E0, 82014B1C | **83427B80, 8201703C (confirmed)** | data |
| Opening-movie end of stream | 82F1F504 | 82FEB3DC | exact |
| Immersive camera layer factor | 82908120 | 829C4678 | exact |
| AI difficulty | 826B01C8 | — | find |
| AI skill, temperament, config | 82521224, 82521228 | 82511B14, 82511B18 | aligned |
| AI skill adjustment after a race | 826C39E8 | 8276D0C0 | aligned |
| Simulation step gate (backpressure) | 824ABC50 | 8248BCE8 | neighbour |
| Late-swap test | 82AABDB4 | 82B71844 | exact |
| UI animated-camera step | 826F2968 | 8279A368 | aligned |
| Crowd animation steps (3 sites) | 82ECAB68, 82ECB27C, 82ECB594 | —, 82F95EC4, — | 1 of 3 (context) |
| Simulation delta store | 8241DC44 | **823FDEF4 (confirmed)** | aligned |
| Render-job throttle | 82424FB0 | 82405248 | exact |
| GPU-fence predicate | 82AAD418 | **82B72EA8 (confirmed)** | exact |
| Vehicle pose write | 82C84424 | 82D4C36C | context |
| Pause-menu action switch | 827EC040 | — | find |

Bold sites are confirmed in game; the findings records
([`docs/findings/`](findings/README.md)) say how. The whole of Pinyon's FH1
v4 symbol table (193 symbols) was also run through the mapper: 165 map,
and they are kept as unverified candidates in
[`config/fh2-symbols-mapped.toml`](../config/fh2-symbols-mapped.toml).

To rerun it: extract both images with `pinyon_shift_fh1_archive_extract
--xex-image ... --apply-patch`, then pass the mapper a JSON list of
`{address, kind}`. The site list for this table lives in the session
scratchpad, not in the repository, because it is only a list of
addresses.

## LP-0: groundwork

| ID | Item | Done when | Size |
| --- | --- | --- | --- |
| LP-0.1 | **FH2 simulation at 60.** Check that free roam keeps real time at 60 guest frames per second (the open STAGE0 step): time a fixed drive or an in-game clock at 30 and at 60 | The speed matches at both rates, or the step to fix is named | S |
| LP-0.2 | **Records.** Add `config/fh2-symbols.toml` for named addresses and findings files (`docs/findings/`) for everything else, in Pinyon's format (address, name, what it does, how it was found, confidence, source) | Each later item writes to them | S |
| LP-0.3 | **Hook plumbing.** Port Pinyon's mid-function hook flow (`[[midasm_hook]]` in the analysis TOML, the app hook functions, `host_overridden_functions` for whole-function overrides) with one test hook | One mapped site hooked, and its effect seen in game | S |
| LP-0.4 | **Reach a race** and add a scripted race route next to the free-roam route, from a pinned seed | Race and free-roam routes run unattended | M |

## LP-1: carry now (no reverse engineering)

**Defaults Pinyon writes that Lavender lacks.** Measure each on the free-roam
route before keeping it:

| ID | Item | Pinyon's result | Gate on FH2 |
| --- | --- | --- | --- |
| LP-1.1 | `gpu_record_thread=true`: separate decoder and recorder threads | 1x race 17.67 → 16.5 ms on Vulkan | Process CPU and frame p99 no worse; pacing unchanged |
| LP-1.2 | `clear_memory_page_state=false`: stop re-uploading every CPU-written page each frame | Race 12 % faster, about 20 MB a frame less upload | Faster, and captures of free roam, menus and a movie match |
| LP-1.3 | First-run hardware defaults: pick a preset from GPU, CPU and display, and repair a partial settings file | — | A new user root starts on the right preset |
| LP-1.4 | Check the x86 compile flags: Pinyon pins `-ffp-contract=off` with FMA, and `-fno-strict-aliasing` on generated code | Correctness for fused multiply-add | Flags match, or the reason for a difference is written down |

**Player features that are generic SDK or host code:**

| ID | Feature | Notes |
| --- | --- | --- |
| LP-1.5 | DISPLAY: monitor, window size, Windows HDR, the full HDR calibration page | SDK cvars |
| LP-1.6 | CONTROLS: button remap, invert look, chords, map controller, rumble, mouse steering, keybinds and RESET KEYS | Pinyon's `src/config/default_keys.h` and menu rows |
| LP-1.7 | AUDIO: master volume and mute | Mixer levels and Your Music need asset and XMP checks first (LP-2.8) |
| LP-1.8 | PROFILE: gamertag, language, save backups and restore | Language list from FH2's own region codes. Check whether a language change can apply live, through a string-table reload |
| LP-1.9 | Host XAM dialogs and keyboard, through the SDK's `XamUiProvider` | — |
| LP-1.10 | Diagnostics: crash reporter with a sanitised ZIP, stall dump, hitch sampler, CPU baseline guard, session events | Copies from Pinyon's `src/` |
| LP-1.11 | Photo save (F8), F3 performance panel, frame inspector | SDK pieces; the inspector needs the executor's frame layout checked on FH2 |
| LP-1.12 | Mods: overlay device, texture replacement, shader overrides, a separate modded profile | Archive rebuilding needs FH2's size checks (LP-2.10) |
| LP-1.13 | Launcher and setup: disc and title-update verification, install, update feed, portable mode, Add to Steam, Logs, Report a problem | Pinyon's C# made title-generic, with FH2 hashes. Its own milestone, after LP-2 |

## LP-2: settings that need FH2 reverse engineering

Ordered by what players notice most and how complete the map is. Each row
must apply live, as in Pinyon:

| ID | Setting | Mechanism (from Pinyon) | FH2 state |
| --- | --- | --- | --- |
| LP-2.1 | **Bloom, motion blur, depth of field** (also forced off under a temporal upscaler, so its history does not smear) | Bloom: zero the frame's interpolated bloom scale. Blur: skip one instruction. DoF: force three decisions off | All 5 sites mapped (context/aligned) |
| LP-2.2 | **Shadows** and **reflection updates** | Record each render scenario as it loads, then set its `SkipShadowMapUnlessCockpit` and `EnvMapFrequencyScale` controls. Shadows only by day, because headlights need the shadow passes | Load hook mapped exactly and the bool vtable mapped; float vtable and control ids still to find. Night-time headlight dependence to recheck |
| LP-2.3 | **Field of view** | Scale the camera's vertical FOV return value | Mapped (context) |
| LP-2.4 | **Ultrawide** (hor+) | Replace the main view's aspect so culling and projection widen together | Getter found by its instructions (`0x823F2354`); gameplay and map callers mapped |
| LP-2.5 | **HUD hide** (F2), **HUD scale**, **safe zone** | Hold the hidden byte; scale is the executor's HUD split after the tone map | Byte mapped (data); check the front-buffer format the HUD split relies on |
| LP-2.6 | **Camera editor** (chase height, distance, stiffness, driving view) and **immersive camera** | Edit the loaded `CameraSettings.ini` tuning block and per-camera values; scale tagged camera-physics layers | Layer site mapped exactly; tuning-block offsets need FH2's camera layout |
| LP-2.7 | **Quality-of-life switches**: skip voice-overs, no auto reset, no attract video, photo camera through walls, skip intro movies | Developer flag bytes on the launch-options object; movie end-of-stream for splash movies only | Object, vtable and movie site mapped; flag offsets need FH2's option visitor |
| LP-2.8 | **Sound mix** and **Your Music** | Rewrite the Gameplay snapshot levels in the audio XML; the host player mutes the radio through XMP polling | Check that FH2 ships the same XML and polls XMP. For the mix to apply live, the snapshot must be reloaded or written in memory |
| LP-2.9 | **AI opponents** (stock, fair, hard) | Remove rubber-banding and catch-up, raise difficulty, zero the post-race skill change | 3 of 4 sites mapped; needs a race (LP-0.4) |
| LP-2.10 | **Mod archives**: accept rebuilt archive sizes | Three size and block checks | Not in the 30; map next |
| LP-2.11 | **Pause menu SETTINGS row** | Rename a pause row and catch its activation | Not mapped |
| LP-2.12 | **DualSense feed**, **telemetry out**, **achievement progress**, **trainer** (credits, time of day, free camera, travel) | Read the car's dynamics, the profile and the world state through named offsets | Needs FH2's semantic table: world holder, player car, dynamics offsets, profile fields. Large; after LP-2.1 to LP-2.7 |
| LP-2.13 | **Restart-free graphics rebuild** | Today a resolution-scale change stalls 6.1 s and an MSAA change 1.9 s, mostly reloading stored pipelines | SDK work, no RE: rebuild in the background and swap, or prewarm the new scale's pipelines first. Gate: no frame over 100 ms |

Pinyon measured bloom, blur and DoF off as saving no performance; they
are look settings. Shadows and reflection rate are the performance cuts
(Odin: shadows -6.4 ms, reflections 25.4 → 23.2 ms).

## LP-3: frame-rate correctness above 30

FH2 renders 60 by default. Pinyon needed these hooks for 60-120. Each one
is checked on FH2 first: keep it only if FH2 shows the problem.

| ID | Fix | FH2 state | Check |
| --- | --- | --- | --- |
| LP-3.1 | Step backpressure: hold the simulation step while the render thread is two steps behind | Mapped (neighbour) | Steps per shown frame at p99 |
| LP-3.2 | Release late swaps: report a late swap as on time | Mapped exactly | Pinyon p99 16.5 → 13.1 ms at 120; check FH2's catch-up frames (the cause found in STAGE0's frame pacing) |
| LP-3.3 | Real-time steps for fixed 30 Hz updaters: the UI camera and the crowds | UI camera mapped; crowds 1 of 3 | Menu camera and crowd speed at 60 against 30 |
| LP-3.4 | Render-job throttle: wait in host time instead of a yield loop | Mapped exactly | Crashes or hangs at a high frame rate |
| LP-3.5 | Host simulation delta and game speed | Mapped (aligned) | Comes with LP-0.1 |

## LP-4: optimizations

The renderer side is already shared. What remains is CPU work on FH2's
own code:

| ID | Item | Pinyon's result | FH2 state |
| --- | --- | --- | --- |
| LP-4.1 | **GPU-fence wait blocks** instead of spinning | -15 % process CPU | Done as the ring-room wait (same function). Check the spin-then-block threshold against Pinyon's 20 µs |
| LP-4.2 | **Render-idle sleep**: `SleepEx(0)` becomes a 200 µs sleep | Odin render thread 477-839 → 242-253 ms/s | Find the call site |
| LP-4.3 | **Guest-native ports**: CRT `pow`, the LZX decoder, the skip for unchanged `SetPWLGamma`, gamma-ramp reuse, the empty array walk | 2.2x pow, 2.2x LZX (3.47 → 1.58 s), 30 % of the render thread for gamma | Likely shared engine or CRT code: map with the mapper's function mode, check the hash guard |
| LP-4.4 | **Physics codegen lists** (`register_locals_functions`, `fixed_fp_mode_functions`, `single_precision_functions`) and the lockstep reference lists | Odin job worker -22 % instructions; desktop title thread 5.91 → 4.57 ms with FMA | Map the 131-function physics family, keep functions that get at least 3 % shorter, verify with the reference tools |
| LP-4.5 | **Audio**: native FMOD output, native ParamEQ, XHV2 voice idle (FMOD stereo is Linux only) | XAudio2 pass 0.49 → 0 ms; voice 0.121 → 0.013 ms a frame | Map the FMOD wrapper and vtables |
| LP-4.6 | **Renderer defaults tuned to FH1's layout**: `fh1_untile_height=720`, `fh1_shared_depth_alias`, `fh1_stable_alpha_to_coverage`, `fh1_glow_depth_test`, `fh1_readback_small_float_resolves` | — | Check each against FH2 captures; these are visual risks, not speed |
| LP-4.7 | **GPU profile of FH2's frame**: GPU time is 13.7 ms per guest frame at 1x on the RTX 4080, close to the 16.7 ms budget | — | `fh1_native_gpu_profile` per pass, then pick the passes to cut. This is LV-0.13 on the desktop |

Pinyon's lessons table lists what did not pay off on FH1 (PGO, ThinLTO
of generated code, display-list compiler, occlusion culling, sub-1x
scale, the D3D-level renderer at -0.9 ms and others). None of these is
retried without a new FH2-specific reason.

## Progress

Measured on the RTX 4080 desktop, 1x, on the free-roam route (four
minutes of scripted driving from the AOT-pipeline user root; the numbers
are means over the drive).

| ID | State | Result |
| --- | --- | --- |
| LP-0.1 | Done | 60.00 simulation steps and 1.000 simulated seconds per second at 60 |
| LP-0.2 | Done | [`config/fh2-symbols.toml`](../config/fh2-symbols.toml) and [`docs/findings/`](findings/README.md) |
| LP-0.3 | Done | Mid-function hooks in `config/analysis/default.toml`, one source file per area |
| LP-0.4 | Blocked on a seed at an event start | FH2 starts a championship only when the car drives into its map icon: fast travel goes to hubs only, and the hubs' event lists need Xbox Live (Rival, Online Road Trip). The title's AI can take the player's car (`lavender_ai_drives_player`, Pinyon's setter, mapped), but in free roam it holds the car still, GPS route or not, and FH2's AI launch options have no reader. The route needs a save made at an event's start, driven there once; inside a race the AI driver should then drive, as in Pinyon (not checked) |
| LP-1.1 | Rejected, stays off | Alone it took process CPU 1.58 → 1.54 cores (within the runs' ±0.07 spread); with page state off it cost 1.42 → 1.51 cores, frame p50/p99 equal, repeats 0.46 → 0.38/s. Fails the CPU gate |
| LP-1.2 | Default off (`clear_memory_page_state=false`) | Process CPU 1.58 → 1.36 cores; frame p50/p99 16.67/17.12 → 16.64/17.15 ms; repeats 0.34/s either way; free-roam and title images unchanged |
| LP-1.3 | Done | A new settings file starts at LOW-SPEC 60 without a discrete GPU of 4 GB or more and 6 or more cores, otherwise at the defaults. Pinyon's rule; FH2's low end is not measured |
| LP-1.4 | Done | Generated code now gets `-ffp-contract=off` (presets) and `-fno-strict-aliasing`, as Pinyon's does; `-fasync-exceptions` is opt-in (`LAVENDER_RECOMP_SEH`) because codegen emits no SEH scope for FH2 |
| LP-1.5 | Done except the Windows HDR switch | DISPLAY adds MONITOR and WINDOW SIZE (live, through the SDK's `ApplyConfiguredSizeAndMonitor`), and with HDR offered or on: HDR, PAPER WHITE, HIGHLIGHTS, GAMMA, COMPARE and the three-step HDR CALIBRATION page (black level, paper white, peak; patterns drawn by the SDK's HDR pass, never saved). The page notes when HDR is on but Windows does not offer it. Pages opened in game with every row on screen; the patterns not seen, since the display did not offer HDR during the check. Pinyon's WINDOWS HDR row (it turns Windows HDR on for the session and restores it) is not ported |
| LP-1.6 | Done | CONTROLS page ported from Pinyon: MOUSE AND KEYBOARD, CONTROLLER BUTTONS (remap, invert look), MAP A CONTROLLER, RACING WHEEL (SDL steering and pedal tuning; Pinyon's separate-device and force-feedback rows are left out), KEYBINDS and RESET KEYS. New settings files get Pinyon's driving keys (`src/config/default_keys.h`). The DualSense rows are left out. Opened in game; remapping not exercised by a script |
| LP-1.7 | Master volume and mute done | New AUDIO page: MASTER VOLUME (0-100, squared curve, the SDK's output gain) and MUTE, both live. The gain is set; how it sounds is not checked by ear |
| LP-1.8 | Save backups done; gamertag and language left out | PROFILE > SAVE BACKUPS, Pinyon's class (`src/save_backups.cpp`) limited to the profile folders (16 hex digits, not `0000000000000000`), since Lavender keeps config, cache, photos and logs in the same user folder. A backup at session start when the saves changed, then at most every ten minutes after the game saves, and at exit; 10 kept (`lavender_save_backups`, `lavender_save_backup_count`). A restore is scheduled from the menu and applied at the next start before the title runs, after a `before-restore` backup; only the profile folders are swapped, with a rollback on failure. Checked: a session backup holds only `B13EBABEBABEBABE` (9 files, 22.7 MB), and a scheduled restore applied, with the saves intact. GAMERTAG and LANGUAGE apply only at the next start, against this menu's rule, and FH2's language codes are not checked |
| LP-1.9 | Done | `src/ui/xam_dialogs.cpp`, Pinyon's: the game's message boxes and on-screen keyboard are host UI screens (FH2's fonts, pad, keyboard and mouse, B or Escape to cancel; `lavender_host_xam_dialogs`, default on), made on first use. Free roam and the menus never opened one, so `LAVENDER_SHIFT_XAM_DIALOG_SELF_TEST=<seconds>` opens a message box and then a keyboard through the provider: the chosen button and the typed text ("LAVENDER") came back. Typing needed two fixes, both missing in Pinyon too: the host UI now turns SDL text input on while a text screen is on top (SDL3 sends no characters otherwise), and ShiftGlue's ImGui drawer no longer swallows characters when no ImGui text field has focus |
| LP-1.10 | Crash report, stall dump, hitch sampler and session events done | `src/diagnostics.cpp`: each run gets a session ID and `<user>/logs/<session>.jsonl` with every recorded event. The crash reporter writes a minidump and a symbolized text report to `<user>/crashes` (`LAVENDER_SHIFT_CRASH_SELF_TEST=1` checks it: report and dump written). The stall watchdog (`lavender_stall_dump_seconds`, 20) logs every guest thread's registers and back chain when no simulation step comes, or when `<user>/logs/dump-threads` is created (checked: 76 threads dumped). The hitch sampler is opt-in (`lavender_hitch_sample_ms`). Not carried: the sanitised ZIP (Pinyon's launcher makes it; Lavender has none yet) and the CPU baseline guard (Lavender builds on the PC that runs it) |
| LP-1.11 | Photo and keys done | F8 saves the game frame (1280x720 PNG, no host overlays) to `<user>/photos`; seen in free roam. The title screen's movie captures black. F11 and Alt+Return toggle fullscreen and save it. F3 is the SDK's panel. Frame inspector not ported |
| LP-2.1 | Done | BLOOM, MOTION BLUR, DEPTH OF FIELD on GRAPHICS, live. Motion blur needed more than Pinyon's skip: FH2 stores the strength only while a scene loads, so the blur objects are kept and written live. With blur on, the road under each moving car stayed sharp in a square: the velocity pass misread the car shadow's stencil mark as an object ID. Fixed with a constant fixup (STAGE0.md, "Motion blur") |
| LP-2.2 | Done | SHADOWS and REFLECTION UPDATES, live; shadows off is seen in free roam |
| LP-2.3 | Done | FIELD OF VIEW 90-130 % on DISPLAY, live |
| LP-2.4 | Done, menus' letterbox unchecked | ASPECT RATIO > WIDER VIEW. The aspect getter was found by its instructions; every 1280x720 view except the pause map's is widened, as in Pinyon. In a 2520x1080 window the gameplay view shows more on both sides, the car keeps its proportions and the HUD its shape. Not checked: the letterbox for menus and the map. In a resized window the SDK takes `window_width` as the guest's display mode (an explicit `--video_mode_width=1280` counts as unset because it equals the default), so the guest's display is 21:9 there and nothing is letterboxed |
| LP-2.5 | Done | DISPLAY > HUD: the HUD switch (F2), HUD SCALE 80-120 % and SAFE ZONE 0-10 %, live, through the SDK executor's HUD split (`fh1_hud_scale`, `fh1_hud_safe_zone`). FH2's HUD draws match the split's detection (the `k_2_10_10_10_AS_10_10_10_10` draws after the blur composite). The scale applies only in the gameplay view: `fh1_hud_scale_suspended` follows the wide-view detection, so menus, the map and loading screens keep their layout. Seen in free roam at 80 % with a 5 % margin: the speedometer and GPS move in toward the centre |
| LP-2.7 | Done | GAMEPLAY page (voice-over skip, automatic reset, attract video, photo camera); intro movies skipped by default (`lavender_skip_intro_movies`) |
| LP-2.11 | Done | The Home page's ONLINE ROAD TRIP tile reads PC SETTINGS and opens the host settings menu (`lavender_pause_settings`, default on); Escape returns to the pause menu, and with a pad the host menu's QUIT GAME row closes the game. Offline the tile needed Xbox Live: its press reaches `sub_826CDF40`, which explains why Live is unavailable, and the hook skips that popup for the tile's feature ID only, so the Social page's Live tiles keep theirs. The label is IDS_MultiplayerOption, whose ID global 0x834584E4 the string-table lookup `sub_82E34E90` is hooked to answer. FH2's own SETTINGS tile is unchanged |
| LP-2.13 | Done for MSAA; scale at 130-150 ms, over the 100 ms gate | Measured on the RTX 4080 desktop in free roam with `lavender_cvar_cycle` (scale 1x/2x every 120 s, MSAA off/4x every 120 s in between), stall = the longest frame at the switch. Before: every change rebuilt the whole renderer, and the frame took about 620 ms (1,015 ms the first time), 500 ms of it translating the 756 stored shaders one by one. Now: an MSAA change rebuilds only the FH1 executor (6-11 ms; no frame over 66 ms, the first 4x frames build their pipelines). A scale change translates on worker threads, three quarters of the logical processors (500 → 83 ms), hands the driver's 27-40 MB pipeline cache to the new pipeline cache instead of saving and reading it back (driver cache 17 → 0 ms), and keeps each scale's translations, so switching back to a scale used before loads them (83 → 35 ms; 28 MB a scale). Rebuild 65-90 ms back to a known scale, 140 ms to a new one; the switch frame 132-147 ms, 258 ms the first time. The rest is the next frame, which reloads every texture and render target because the texture cache is rebuilt for the new scale. Below 100 ms needs that cache kept across scales (its resolved textures are scaled) |
| LP-2.12 | Candidates | All 193 symbols of Pinyon's FH1 v4 table run through the mapper: 165 placed, in [`config/fh2-symbols-mapped.toml`](../config/fh2-symbols-mapped.toml) |
| LP-3.2 | Default on, no change measured | Free roam is never a whole frame late on the 4080: repeats 0.38/s and frame p99 17.14 ms on and off. Kept for slower GPUs, as Pinyon's 120 fps race showed (p99 16.5 → 13.1 ms) |
| LP-3.3 | Crowds done; UI camera ported, not seen | `src/fixed_steps.cpp`, default on (`lavender_fixed_steps_real_time`). The crowd update runs 180 times a second in free roam (three model sets at 60 frames), one animation frame each: twice the console's speed. Each call now gets the 30 Hz steps of real time since the set's previous call (90 a second). The UI camera's fixed step is scaled the same way at the mapped site, which free roam and the title screen never call. The mapper had placed one of the three crowd sites; the other two came from aligning the function with FH1 v4's |
| LP-3.4 | Default on | Ported as is (identical site); FH2 has not shown the FH1 crash |
| LP-4.2 | Ported, no effect in FH2 | The idle `SleepEx` ran 2,051 times while loading and never while driving: FH2's render thread does not wait on the simulation in play |
| LP-4.3 | Done (array walk not kept) | `src/guest_native/`, Pinyon's dispatcher (hash guard, per-replacement cvar, verify mode) and its bit-identical CRT `pow` (`lavender_native_pow`) with FH2's addresses: the function and its callees match FH1 v4's after masking, and the constants were mapped and checked by value. In two minutes of free roam with `lavender_guest_native_verify=1` it ran 14.2 million calls, about 110,000 a second, with no mismatch and none declined; verified calls took 79 ns native against 136 ns recompiled, about 6 ms of CPU a second (0.1 ms a frame) across the threads that call it. Verify mode writes its summary every 10 s, since Lavender has no route end. Pinyon's native LZX block decoder (`lavender_native_lzx_decoder`) maps exactly: from boot to two minutes of free roam it decoded 9,173 blocks with no mismatch, 0.61 s native against 1.12 s recompiled (1.8x; Pinyon measured 2.2x). The gamma hooks (`src/gamma_hooks.cpp`, `lavender_reuse_gamma_ramp` and `lavender_skip_unchanged_device_gamma`) sit at FH1's offsets in the same builder: FH2 also rebuilds the same ramp every frame (600 in 10 s) and hands it to `SetPWLGamma`, which finds nothing changed; with both on, every rebuild is reused and every device call skipped, saving 23 µs plus 7.7 µs of the render thread a frame here (`lavender_gamma_log` counts and times them). The empty array walk maps exactly (`sub_8255A148`) but is not hot in FH2: 288 releases over 77,201 elements from boot to a minute of free roam, so the skip was dropped |
| LP-4.5 | Done (FMOD stereo not ported: Linux only) | `src/guest_native/fmod_output.cpp`, `fmod_dsp.cpp` and `voice_chat.cpp`, Pinyon's ports with FH2's addresses: all ten XAudio2 methods, FMOD's two call sites, the source voice's interface table, the ParamEQ unit and the voice-chat tick map exactly (findings: Audio). Shadow mode (`lavender_native_audio_shadow`) found the guest pass's free-roam output equal to FMOD's ring sample for sample; during the intro movie the ring is silent and the movie's own voice keeps the guest path on, as designed. Native output (`lavender_native_audio_output`) takes over once the movie ends; the ParamEQ twin verified clean under `lavender_fmod_dsp_verify`; the voice tick (`lavender_voice_chat_idle`) is skipped. Two A/B pairs of free roam on the desktop (30 s and 20 s windows, all three on against all off, in both orders): the process used 128.3 against 130.7 % and 122.7 against 125.4 % of a core, about 0.4 ms of CPU a frame, in line with Pinyon's 0.49 ms XAudio2 pass. Not listened to: the display and speakers were off during the runs |
| LP-4.6 | Checked; all five stay on | Replays of a free-roam driving frame with each turned off, against two baseline replays (which differ only in the 64-row band of replay noise). `fh1_glow_depth_test` off: a headlight glow shows through the player car's rear window, the FH1 bug, so FH2 needs it. `fh1_shared_depth_alias` off: no change outside the noise band. `fh1_stable_alpha_to_coverage` off: only edge dither changes (0.6 % of pixels) at MSAA OFF. `fh1_untile_height=720` is FH2's height too. `fh1_readback_small_float_resolves`: FH2 makes one 32_FLOAT resolve of 4 KB or less a frame, as FH1's auto exposure does (60 readbacks a second on, none off); the CPU read could not be shown (the resolve access trace catches writes only), and a live A/B was swamped by the falling light at 48x time of day, so it stays on at the cost of one small readback a frame |
| LP-4.7 | Measured: the GPU is not the limit on the RTX 4080 | Free roam at 1x, `fh1_native_gpu_profile`, six 600-frame windows: draw renderings 4.3-4.6 ms a frame, transfers 0.5-0.8, resolves 0.45-0.55, clears and texture loads under 0.2, so about 5.5-6 ms of GPU work. The 13.7-16.6 ms "frame" figures (and the perf CSV's guest-frame GPU time, p50 15.9 ms) are the span from the frame's first to last timestamp; the timed passes cover about 6 ms of it, and the rest is untimed work or the GPU waiting for submissions (not split further). Heaviest: the 4x MSAA scene (1.2 ms over ~28 renderings), the 48 small 1x passes of the post chain (0.48 ms), uploads (0.34 ms), the 2x MSAA 128-row pass (0.34 ms). Nothing to cut here for speed on this GPU; repeat on a low-end GPU before choosing passes |

Not started: LP-2.6 camera, LP-2.8 audio mix, LP-2.9 AI (needs a race),
LP-2.10 mod archives, LP-3.1 (needs a race), LP-4.4, and the
generic features LP-1.12 and LP-1.13.

## Not carried

- **FH1 content:** the Rally adapter, 1000 Club, Treasure Map, XE and
  UDLC installers, the DLC catalog, custom radio stations (FH1's FSB
  layout), car-card repair and career repair.
- **FH1's v4 build machinery:** Lavender has one version.
- **FH1 stability guards:** about a dozen null and geometry guards for
  FH1 crashes. Each is added for FH2 only when FH2 hits the same crash.
- **Car importer:** it targets FH1's car formats. It can be reopened once
  FH2's car data is compared.

## Order

1. **LP-0** (groundwork), then **LP-1.1 to LP-1.4** (one measured run
   each). These are cheap and set the baseline.
2. **LP-2.1, LP-2.3, LP-2.5 and LP-2.7:** fully or mostly mapped, quick
   wins in the menu.
3. **LP-2.2, LP-2.13 and LP-4.7:** the performance settings, the rebuild
   stall, and knowing where the GPU frame goes.
4. **LP-3** once a race runs; **LP-2.9** with it.
5. **LP-1.5 to LP-1.12** (the generic features) alongside, as they are
   independent.
6. **LP-4.2 to LP-4.5**, then **LP-2.12** (the semantic table), then
   **LP-1.13** (the launcher).
