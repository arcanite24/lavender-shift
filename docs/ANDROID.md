# Lavender Shift on Android

**Status: first boot, for developers.** The game cross-compiles for arm64
Android with the NDK and packages into an APK on the PC that built it,
reusing Pinyon Shift's Android pipeline. Everything platform-specific (SDL's
activity, the Vulkan surface, the custom GPU driver loader, thread pinning)
is in ShiftGlue and shared with Pinyon Shift; this repository adds the build
target, the activity, the packager and the driver bundle. Results on devices
are in [Android results](#results).

## What stays private

The package holds the game translated from the player's own disc, as
`lavender_shift.exe` does on Windows, so it is built on their PC, installed
on their own device and never published (see [legal](LEGAL.md)). The game
files go from the PC to the device over adb.

## Requirements

- A Windows PC where the game is already built (`tools/build.ps1`): the
  Android build compiles the code translated there again for arm64 and runs
  the PC's generator.
- The Android SDK with platform-tools, build-tools 36.0.0, android-36 and NDK
  29.0.14206865 (`config/android-toolchain.json`), and JDK 17 or newer
  (`JAVA_HOME` or `javac` on `PATH`).
- The device: arm64 with an ARMv8.2 CPU (the libraries target
  `armv8.2-a`), Android 13 or later, Vulkan 1.3, and about 8 GB free on
  internal storage or an SD card.

## Build, install and run

```powershell
python tools/lavender_android.py build        # out/build/android-arm64-release, then .local/android/lavender-shift.apk
python tools/lavender_android.py install
python tools/lavender_android.py push-data    # .local/game/disc and update
python tools/lavender_android.py run
```

`push-data --device-root /storage/<card>/Android/data/studio.deimos.lavendershift/files`
puts the game on an SD card instead. The activity uses the first of the
app's external files folders (internal storage, then each SD card) whose
`game/disc/default.xex` exists, with `game/disc`, `game/update` and `user`
in it, and passes the same flags `tools/launch.ps1` passes on Windows.
Extra flags go in `user/config/android-arguments.txt` (one per line) or after
`run`, and a string extra `env.NAME` sets an environment variable. A later
`--name=value` replaces an earlier one: the game's parser would otherwise
reject the repeated option and apply none of the arguments.

A new settings file on Android starts at 1x with 2x MSAA, reflections
updated every fourth frame (REFLECTION UPDATES QUARTER), no sun shadows,
bilinear presentation and 60 fps, the cheapest settings measured to hold
60 fps on a Snapdragon 8 Gen 2 ([results](#results)). A settings file
copied from a PC keeps its own values; pick them in the GRAPHICS page.

## GPU drivers

The package bundles the Mesa Turnip build Pinyon Shift ships, Turnip Gen8
V37 (MIT, by StevenMXZ; packaged from Pinyon Shift's
`.local/android/drivers/turnip-gen8-v37` with its notice), and installs it
into `user/drivers` at start. `android_gpu_driver` picks the driver loaded at
start: `auto` (the default) loads Turnip Gen8 V37 on Adreno 7xx and 8xx and
the system's elsewhere, an empty value the system's, and any other value the
folder of that name under `user/drivers`. `push-driver <zip> --name <name>`
copies another adrenotools package there.

## Results

**AYN Odin 2 Portal** (Snapdragon 8 Gen 2, Adreno 740, Android 13),
2026-11-10, game on the SD card, a copy of a Windows player save, 1x
resolution with no upscaler (`--draw_resolution_scale_x=1
--draw_resolution_scale_y=1 --present_effect=none`), the game's own 60 fps
limit. The save loads into free roam and plays on the bundled Turnip. The
GPU stays 99 % busy at its 680 MHz top clock, so the frame is GPU-bound.

Turnip builds, standing at the save's start point (the last 600 frames of
`--perf_log_csv`, about 2.5 minutes after start; traffic changes the draw
count from run to run):

| Driver | Mesa | Frame rate | GPU ms per frame | Draws | GPU µs per draw |
|---|---|---|---|---|---|
| Turnip Gen8 V37 (bundled) | 26.3.0-devel | 45.8 fps | 21.2 | 1,771 | 12.0 |
| Turnip Gen8 V39 | 27.0.0-devel | 46.7 fps | 20.8 | 1,661 | 12.5 |
| Turnip Gen8 V39, patched | 27.0.0-devel | 47.6 fps | 20.2 | 1,539 | 13.1 |
| Turnip Gen8 V37, again | 26.3.0-devel | 45.3 fps | 21.4 | 1,760 | 12.2 |

V39 and its patched build load, render the same scene correctly and run
within 5 % of V37. The differences follow the draw count, and per draw
neither is faster, so V37 stays the bundled default. A fixed route (same
traffic, same time of day) would settle the remaining few percent.

### Holding 60 fps on the Odin 2 Portal

Settings were compared inside one run each (`--lavender_cvar_cycle`
switching them every 20 seconds, alternating, the first 3 seconds of each
window dropped), standing at the save's start point, so heat and traffic
affect both sides alike. GPU time is corrected for the draw count, which
traffic moves by ±20 % (about 0.33 ms per 100 draws). Once the frame
holds 60 fps, the Adreno's governor stretches the GPU's work over the
frame, so the GPU's own busy share (`/sys/class/kgsl/kgsl-3d0`, sampled
every second) and the game's CPU time (`/proc/<pid>/stat`) measure the
headroom better than GPU milliseconds.

| Change | Effect | Kept |
|---|---|---|
| MSAA 4x → 2x | −1.4 ms GPU at about 1,700 draws | 2x |
| MSAA 2x → off | −1.9 ms GPU at about 1,700 draws; at the 60 fps settings, 69 % GPU busy against 76 % | 2x, for its edges |
| REFLECTION UPDATES FULL → QUARTER | −0.75 ms GPU | quarter |
| SHADOWS off (day only) | −0.3 ms GPU | off |
| Bloom off | no change | on |
| Coarse shading 1x2, 2x1 or 2x2 on the main pass (`fh1_coarse_shading`, `vulkan_fragment_shading_rate`) | no change (±0.05 ms): the main pass is not limited by pixel shading at 1x | off |
| `gpu_record_thread` | no change: 2.2 CPU cores, the same frame times | off (the default) |

With 2x MSAA, quarter reflections and no shadows, the game holds 60 fps
at the start point: 16.67 ms average, 16.8 ms at the 95th percentile,
0.0 to 0.5 % of frames over 17.5 ms, the GPU 75 to 80 % busy at 680 MHz
and the game using about 2.2 CPU cores. With MSAA off the GPU is about
69 % busy. A GPU profile (`--fh1_native_gpu_profile`) puts 70 % of the
frame in the main scene pass and 1.6 ms in resolves, and shows about 100
render passes a frame, most ended by uploads and resolves; on a tiled GPU
each break stores and reloads its targets, the next place to look.

### Start and load times

From launch to free roam with the save took about 85 seconds on the Odin
2 Portal (game on the SD card), against about 8 on a Windows PC. Off-CPU
profiles (`simpleperf record --trace-offcpu`, the app is profileable)
found two costs in ShiftGlue's host folder device, both cheap on Windows
and slow on Android, where app storage sits behind FUSE:

- At mount it walked and stat'ed the whole game folder (about 2,900
  entries): 11.9 seconds of every launch. Folders now list their entries
  the first time the game looks in them; mounting takes 7 ms.
- For every name it could not find, it stat'ed the name and listed the
  folder again to look for a casing difference. FH2's loader workers probe
  for missing files often enough that these rescans took about 44 seconds
  of the load, and serialised the workers on the folder tree's lock. A
  miss in a folder already listed (and so compared without case) is now
  final.

With both, launch to free roam takes 16 seconds (three launches, 16.0 to
16.5). Lifting the frame rate limit during the load changed nothing: the
load is not paced by frames.
