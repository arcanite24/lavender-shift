[CmdletBinding()]
param(
    # generator: build rexglue.exe from thirdparty/shiftglue-sdk.
    # codegen:   translate .local/game/codegen with the generator.
    # game:      configure and compile the game (runs codegen if needed).
    # all:       the three in order.
    [ValidateSet('generator', 'codegen', 'game', 'all')]
    [string]$Step = 'all',
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [ValidateRange(0, 64)] [int]$Parallel = 0
)

# Stage 0 reference build of Lavender Shift on Windows: the TU v2 image on
# today's ShiftGlue, with its existing PM4 path (docs/APPROACH_RESEARCH.md,
# "Stage 0"). The toolchain is the one Pinyon Shift pins
# (.local/toolchain: LLVM 20.1.8, CMake 3.31.10, Ninja 1.13.2) plus the
# Visual Studio Build Tools for the Windows SDK and CRT.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $root 'thirdparty/shiftglue-sdk'
$toolchain = Join-Path $root '.local/toolchain'
$logs = Join-Path $root '.local/logs'
[void](New-Item -ItemType Directory -Force -Path $logs)
if ($Parallel -eq 0) { $Parallel = [Environment]::ProcessorCount }

function Enter-BuildEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if (-not $vs) { throw 'Visual Studio Build Tools with the x64 C++ tools were not found.' }
    $devCmd = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
    $marker = '::lavender-environment::'
    $output = @(& $env:ComSpec /d /s /c "`"$devCmd`" -arch=x64 -host_arch=x64 && echo $marker&& set" 2>&1 |
        ForEach-Object { $_.ToString() })
    $index = [Array]::IndexOf($output, ($output | Where-Object { $_.Trim() -eq $marker } | Select-Object -First 1))
    if ($LASTEXITCODE -ne 0 -or $index -lt 0) { throw "VsDevCmd failed: $devCmd" }
    foreach ($line in $output[($index + 1)..($output.Count - 1)]) {
        $at = $line.IndexOf('=')
        if ($at -gt 0) {
            $name = $line.Substring(0, $at)
            if ($name -notin @('TEMP', 'TMP')) {
                [Environment]::SetEnvironmentVariable($name, $line.Substring($at + 1), 'Process')
            }
        }
    }
    $llvm = Join-Path $toolchain 'llvm-20.1.8/bin'
    $ninja = Join-Path $toolchain 'ninja-1.13.2'
    $cmakeBin = Join-Path $toolchain 'cmake-3.31.10-windows-x86_64/bin'
    foreach ($path in @($llvm, $ninja, $cmakeBin, $python)) {
        if (-not (Test-Path -LiteralPath $path)) { throw "Toolchain folder missing: $path" }
    }
    $env:PATH = "$llvm;$ninja;$cmakeBin;$env:PATH"
}

function Invoke-Logged {
    param([string]$Name, [string]$FilePath, [string[]]$Arguments)
    $log = Join-Path $logs "$Name.log"
    Write-Host "[$Name] $FilePath $($Arguments -join ' ')"
    $start = Get-Date
    & $FilePath @Arguments *> $log
    $exit = $LASTEXITCODE
    $seconds = [int]((Get-Date) - $start).TotalSeconds
    if ($exit -ne 0) {
        Get-Content -LiteralPath $log -Tail 40 | Out-Host
        throw "[$Name] failed with exit code $exit after $seconds s (log: $log)"
    }
    Write-Host "[$Name] done in $seconds s"
}

# SPIRV-Tools and glslang (the Vulkan backend) need Python at configure time;
# pin the toolchain's instead of whatever the PATH offers (a pyenv shim fails
# there). glslang reads the older PYTHON_EXECUTABLE.
$python = Join-Path $toolchain 'python-3.13.15/python.exe'
$pythonCMake = $python.Replace('\', '/')
$generator = Join-Path $sdk 'out/win-amd64/Release/rexglue.exe'
$manifest = Join-Path $root 'lavender_shift_manifest.toml'

Enter-BuildEnvironment

if ($Step -in @('generator', 'all')) {
    $manifestFlag = '-Xlinker "/MANIFESTINPUT:' + ((Join-Path $root 'config/codegen-windows.manifest') -replace '\\', '/') + '"'
    Push-Location $sdk
    try {
        Invoke-Logged 'generator-configure' 'cmake' @('--preset', 'win-amd64',
            '-DREXGLUE_ENABLE_TRACY=OFF', '-DSDL_HIDAPI_LIBUSB=OFF',
            "-DPython3_EXECUTABLE=$pythonCMake", "-DPYTHON_EXECUTABLE=$pythonCMake",
            "-DCMAKE_EXE_LINKER_FLAGS=$manifestFlag")
        Invoke-Logged 'generator-build' 'cmake' @('--build', '--preset', 'win-amd64-release',
            '--target', 'rexglue', '--parallel', "$Parallel")
    }
    finally { Pop-Location }
}

if ($Step -in @('codegen', 'all')) {
    if (-not (Test-Path -LiteralPath $generator)) { throw "Build the generator first: $generator" }
    Push-Location $root
    try {
        Invoke-Logged 'codegen' $generator @('--log-level', 'info',
            '--log-file', (Join-Path $logs 'codegen-detail.log'),
            'codegen', $manifest, '--xex_apply_patches=true')
    }
    finally { Pop-Location }
}

if ($Step -in @('game', 'all')) {
    $preset = 'win-amd64-' + $Configuration.ToLowerInvariant()
    Push-Location $root
    try {
        Invoke-Logged 'game-configure' 'cmake' @('--preset', $preset, "-DREXSDK_DIR=$sdk",
            '-DREXGLUE_ENABLE_TRACY=OFF', '-DSDL_HIDAPI_LIBUSB=OFF',
            "-DPython3_EXECUTABLE=$pythonCMake", "-DPYTHON_EXECUTABLE=$pythonCMake")
        Invoke-Logged 'game-build' 'cmake' @('--build', '--preset', $preset, '--parallel', "$Parallel")
    }
    finally { Pop-Location }
}
