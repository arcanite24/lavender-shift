# Third-party notices

Lavender Shift downloads the following third-party projects during local
setup. Their own licences apply; they are not relicensed by this
repository.

- [ShiftGlue SDK](https://github.com/arcanite24/shiftglue-sdk), a fork of
  the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- [LLVM](https://github.com/llvm/llvm-project)
- [CMake](https://github.com/Kitware/CMake)
- [Ninja](https://github.com/ninja-build/ninja)
- [Python](https://www.python.org/) (the embeddable package)
- [XZ Utils](https://github.com/tukaani-project/xz)
- [Git for Windows](https://github.com/git-for-windows/git) (MinGit)
- [Microsoft Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)

Exact versions, source URLs and archive hashes are recorded in
`config/release-toolchain.json`. ShiftGlue's own dependencies (SDL3,
Vulkan headers and loader, glslang, SPIRV-Tools, spdlog, libmspack and
others) are fetched by its pinned source tree as submodules and keep their
upstream notices in `thirdparty/` there.

The launcher is built with .NET 8 (MIT) and ships self-contained, so its
executable includes the .NET runtime.
