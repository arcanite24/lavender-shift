# Third-party notices

Lavender Shift downloads the following third-party projects during local
setup. Their own licences apply; they are not relicensed by this
repository. A release also attaches two prebuilt archives that setup
downloads in place of building them: a trimmed copy of the pinned LLVM
release (its tools and compiler headers, with LLVM's licence), and the
ShiftGlue kit, ShiftGlue built at the pinned revision with the
Microsoft C++ runtime DLLs it needs (redistributable under the Visual
Studio licence) and the licences of the code it contains.

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

Exact versions, source URLs and archive hashes are recorded in
`config/release-toolchain.json`. ShiftGlue's own dependencies (SDL3,
Vulkan headers and loader, glslang, SPIRV-Tools, spdlog, libmspack and
others) are fetched by its pinned source tree as submodules and keep their
upstream notices in `thirdparty/` there.

The launcher is built with .NET 8 (MIT) and ships self-contained, so its
executable includes the .NET runtime.
