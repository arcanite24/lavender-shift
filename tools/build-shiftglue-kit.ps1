[CmdletBinding()]
param(
    # Where the kit's build tree goes; a fresh folder outside the checkout
    # keeps it apart from the developer build.
    [Parameter(Mandatory)] [string]$BuildRoot,
    # The kit archive to write (a .zip).
    [Parameter(Mandatory)] [string]$Output,
    [ValidateRange(0, 64)] [int]$Parallel = 0
)

# RL-10 (docs/RELEASE_BACKLOG.md, after Pinyon Shift's TP-2 and TP-3): build
# the ShiftGlue kit that setup downloads instead of cloning and compiling
# ShiftGlue: the code generator, the runtime and graphics backend DLLs with
# their import libraries, and the headers and CMake package the game build
# consumes. Nothing in it is game-derived. Pinyon Shift's kit cannot stand in:
# it is built from another ShiftGlue revision with another CPU baseline.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

. (Join-Path $PSScriptRoot 'release-common.ps1')
$config = Get-LavenderReleaseToolchain
# The kit stages the Build Tools' redistributable C++ runtime; the fetched
# sysroot has headers and import libraries only.
$env:LAVENDER_SHIFT_MSVC_SYSROOT = '0'
$environment = Enter-LavenderBuildEnvironment
$sdkRoot = Resolve-LavenderRexGlueRoot
if ($Parallel -eq 0) { $Parallel = Get-LavenderBuildJobCount }
$env:SOURCE_DATE_EPOCH = '1784764800'

$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$buildTree = Join-Path $BuildRoot 'build'
$prefix = Join-Path $BuildRoot 'kit'
$logs = Join-Path $BuildRoot 'logs'
foreach ($folder in @($buildTree, $logs)) { [void](New-Item -ItemType Directory -Force -Path $folder) }
if (Test-Path -LiteralPath $prefix) { Remove-Item -LiteralPath $prefix -Recurse -Force }

$git = Get-LavenderGit
$revision = (& $git -C $sdkRoot rev-parse HEAD).Trim().ToLowerInvariant()
if ($revision -ne $config.rexglue.revision) {
    throw "ShiftGlue is at $revision, but config/release-toolchain.json pins $($config.rexglue.revision)."
}
# Line-ending churn is not a change; anything else would ship unpinned code.
$changes = @(& $git -C $sdkRoot -c core.autocrlf=true diff --ignore-submodules=dirty --name-only HEAD)
if ($changes.Count -gt 0) { throw "ShiftGlue has local changes: $($changes -join ', ')" }

# The options the game's CMake presets compile the SDK with (CMakePresets.json
# windows-base): clang, x86-64-v3 with exact floating-point contraction, the
# Vulkan backend, Release, Tracy off. The source prefix map keeps the build
# machine's paths out of the binaries.
$flags = "-ffp-contract=off -march=x86-64-v3 -ffile-prefix-map=$($sdkRoot -replace '\\', '/')=shiftglue"
$python = (Get-LavenderPython).Replace('\', '/')
Invoke-LavenderBuildCommand $environment.CMake @('-S', $sdkRoot, '-B', $buildTree, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
    "-DCMAKE_C_FLAGS=$flags", "-DCMAKE_CXX_FLAGS=$flags",
    '-DREXGLUE_USE_VULKAN=ON', '-DREXGLUE_ENABLE_TRACY=OFF', '-DSDL_HIDAPI_LIBUSB=OFF',
    '-DREXGLUE_INSTALL_REGISTER_PACKAGE=OFF', "-DPython3_EXECUTABLE=$python", "-DPYTHON_EXECUTABLE=$python",
    "-DCMAKE_INSTALL_PREFIX=$prefix", '-DCMAKE_LINKER_TYPE=LLD',
    "-DCMAKE_EXE_LINKER_FLAGS=-Xlinker /Brepro $(Get-LavenderCodegenManifestLinkFlag)",
    '-DCMAKE_SHARED_LINKER_FLAGS=-Xlinker /Brepro') `
    (Join-Path $logs 'configure.log') 'ShiftGlue kit configuration failed.' -Step 'Configure the ShiftGlue kit'
Invoke-LavenderBuildCommand $environment.CMake @('--build', $buildTree, '--target', 'install',
    '--parallel', "$Parallel") (Join-Path $logs 'build.log') 'ShiftGlue kit build failed.' -Step 'Build the ShiftGlue kit'

# SDK files the game compiles or includes itself (the SDK's rex_app.cpp reads
# ImGui and RenderDoc headers), at their source-tree paths
# (LAVENDER_SHIFT_SDK_SOURCE_DIR in cmake/rexglue.cmake).
$mirror = Join-Path $prefix 'share/rexglue/source'
foreach ($item in @(
    @{ From = 'thirdparty/crypto'; Filter = 'sha256.*' },
    @{ From = 'thirdparty/imgui'; Filter = '*.h' },
    @{ From = 'thirdparty/renderdoc'; Filter = '*.h' },
    @{ From = 'thirdparty/tomlplusplus/include'; Filter = '*'; Recurse = $true },
    @{ From = '.'; Filter = 'LICENSE*' }
)) {
    $from = Join-Path $sdkRoot $item.From
    $to = Join-Path $mirror $item.From
    [void](New-Item -ItemType Directory -Force -Path $to)
    if ($item.ContainsKey('Recurse')) { Copy-Item -Path (Join-Path $from '*') -Destination $to -Recurse -Force }
    else { Copy-Item -Path (Join-Path $from $item.Filter) -Destination $to }
}

# No vendor binary goes in the kit (docs/LEGAL.md): AMD's signed FidelityFX
# DLL and NVIDIA's DLSS runtime are fetched at the game's configure, pinned
# and hashed, by the same SDK modules a source build runs.
$modules = Join-Path $prefix 'share/rexglue/cmake'
[void](New-Item -ItemType Directory -Force -Path $modules)
foreach ($module in @('rexglue_dlss.cmake', 'rexglue_ffx_api_headers.cmake')) {
    Copy-Item -LiteralPath (Join-Path $sdkRoot "cmake/$module") -Destination $modules
}
$ffxNotice = Join-Path $prefix 'share/rexglue/upscalers/fidelityfx'
[void](New-Item -ItemType Directory -Force -Path $ffxNotice)
Copy-Item -LiteralPath (Join-Path $sdkRoot 'thirdparty/fidelityfx-vk/LICENSE.txt') -Destination $ffxNotice

# The Microsoft C++ runtime the kit's DLLs were built against, staged beside
# the game (Microsoft's app-local deployment), so it starts on a PC without
# the matching redistributable and a build from the sysroot needs no Build
# Tools at all.
$redist = Get-ChildItem -LiteralPath (Join-Path $environment.VisualStudioRoot 'VC/Redist/MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.' } | Sort-Object { [version]$_.Name } | Select-Object -Last 1
$crt = Get-ChildItem -LiteralPath (Join-Path $redist.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
    Select-Object -First 1
$crtTarget = Join-Path $prefix 'redist'
[void](New-Item -ItemType Directory -Force -Path $crtTarget)
Copy-Item -Path (Join-Path $crt.FullName '*.dll') -Destination $crtTarget

$kit = [ordered]@{
    schema_version = 1
    platform = 'windows-x64'
    shiftglue_revision = $revision
    llvm = $config.llvm.version
    msvc_toolset = $env:VCToolsVersion
    windows_sdk = $env:WindowsSDKVersion.TrimEnd('\')
    cpu_baseline = 'x86-64-v3'
    configuration = 'Release'
}
[IO.File]::WriteAllText((Join-Path $prefix 'kit.json'), (($kit | ConvertTo-Json) + [Environment]::NewLine),
    [Text.UTF8Encoding]::new($false))

& (Get-LavenderPython) -I (Join-Path $PSScriptRoot 'repack-toolchain.py') kit --source $prefix --output $Output `
    --top 'shiftglue-kit'
if ($LASTEXITCODE -ne 0) { throw 'Could not write the kit archive.' }
