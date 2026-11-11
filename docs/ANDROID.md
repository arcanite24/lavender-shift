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
`run`, and a string extra `env.NAME` sets an environment variable.

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
