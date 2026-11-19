# Third-party notices

Lavender Shift downloads the following third-party projects during local
setup. Their own licences apply; they are not relicensed by this
repository. A release also attaches two prebuilt archives that setup
downloads in place of building them: a trimmed copy of the pinned LLVM
release (its tools and compiler headers, with LLVM's licence), and the
ShiftGlue kit, ShiftGlue built at the pinned revision with the
Microsoft C++ runtime DLLs it needs (redistributable under the Visual
Studio licence) and the licences of the code it contains. A third, the
Android shell, is ShiftGlue built for Android with libadrenotools' hooks
and the app's compiled Java code (with SDL's), which the Android build
uses in place of building them.

- [ShiftGlue SDK](https://github.com/arcanite24/shiftglue-sdk), a fork of
  the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- [LLVM](https://github.com/llvm/llvm-project)
- [CMake](https://github.com/Kitware/CMake)
- [Ninja](https://github.com/ninja-build/ninja)
- [Python](https://www.python.org/) (the embeddable package)
- [XZ Utils](https://github.com/tukaani-project/xz)
- [Git for Windows](https://github.com/git-for-windows/git) (MinGit)
- [Microsoft Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/),
  or, when the player accepts Microsoft's license in the launcher, the
  Microsoft C++ libraries and Windows SDK fetched from Microsoft by
  [xwin](https://github.com/Jake-Shadle/xwin)
- AMD FidelityFX and NVIDIA DLSS runtimes, fetched by ShiftGlue's pinned,
  hashed modules when the game is configured
- LSFG frame generation, which runs only from the player's own Lossless
  Scaling install (`present_lsfg_dll`, or Lossless Scaling in their Steam
  libraries), is built by
  ShiftGlue from [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) 1.0.0's
  `framegen` (MIT) and [DXVK](https://github.com/doitsujin/dxvk) 2.6.2's
  DXBC compiler (zlib License), in `thirdparty/lsfg` of its source tree.
  No Lossless Scaling code or shaders are included
- The GSR choice of OUTPUT SCALING is Qualcomm's
  [Snapdragon Game Super Resolution](https://github.com/SnapdragonGameStudios/snapdragon-gsr)
  1 shader (BSD-3-Clause), compiled into ShiftGlue's Vulkan presenter;
  its licence is in `thirdparty/snapdragon-gsr` of ShiftGlue's source tree
  and in the ShiftGlue kit
- For the Android build: the [Android NDK and platform-tools](https://developer.android.com/studio/terms)
  from Google (the player accepts the Android SDK licence in the launcher
  first; `config/android-toolchain.json`), and
  [Turnip Gen8 V37](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/tag/v37),
  StevenMXZ's build of Mesa's Turnip Vulkan driver (MIT), bundled
  unmodified in the package with its notice
  (`android/drivers/turnip-gen8-v37/NOTICE.txt`)

Exact versions, source URLs and archive hashes are recorded in
`config/release-toolchain.json`. ShiftGlue's own dependencies (SDL3,
Vulkan headers and loader, glslang, SPIRV-Tools, spdlog, libmspack and
others) are fetched by its pinned source tree as submodules and keep their
upstream notices in `thirdparty/` there.

This repository includes one third-party file:
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)'s
gamepad mappings (zlib License), unmodified in
`thirdparty/sdl-gamecontrollerdb` with its licence; the build copies them
beside the game as `gamecontrollerdb.txt`.

The launcher is built with .NET 8 (MIT) and ships self-contained, so its
executable includes the .NET runtime.
