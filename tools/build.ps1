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
    [ValidateRange(0, 64)] [int]$Parallel = 0,
    [switch]$JsonEvents
)

# Stage 0 reference build of Lavender Shift on Windows: the TU v2 image on
# today's ShiftGlue, with its existing PM4 path (docs/APPROACH_RESEARCH.md,
# "Stage 0"). The tools are the ones config/release-toolchain.json pins
# (tools/provision-toolchain.ps1) plus the Visual Studio Build Tools for the
# Windows SDK and CRT.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = Get-LavenderRepoRoot
# The submodule in a checkout, the pinned clone in .local/rexglue in a
# release install (tools/prepare-rexglue.ps1).
$sdk = Resolve-LavenderRexGlueRoot
$logs = Join-Path $root '.local/logs'
[void](New-Item -ItemType Directory -Force -Path $logs)
if ($Parallel -eq 0) { $Parallel = Get-LavenderBuildJobCount }

# The last Ninja "[done/total]" counter in a log that is still being written.
function Get-NinjaProgress {
    param([string]$Path)
    try {
        $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        try {
            $length = [Math]::Min($stream.Length, 16384)
            [void]$stream.Seek(-$length, 'End')
            $buffer = New-Object byte[] $length
            $read = $stream.Read($buffer, 0, $length)
            $text = [Text.Encoding]::UTF8.GetString($buffer, 0, $read)
        }
        finally { $stream.Dispose() }
    }
    catch { return $null }
    $found = [regex]::Matches($text, '\[(\d+)/(\d+)\]')
    if ($found.Count -eq 0) { return $null }
    $match = $found[$found.Count - 1]
    @([int]$match.Groups[1].Value, [int]$match.Groups[2].Value)
}

function Invoke-Logged {
    # $Event: @{ Percent; Message } announces the step; with To, the step is
    # a Ninja build and its progress moves the bar from Percent to To.
    param([string]$Name, [string]$FilePath, [string[]]$Arguments, $Event = $null)
    $log = Join-Path $logs "$Name.log"
    Write-Host "[$Name] $FilePath $($Arguments -join ' ')"
    if ($Event) { Write-LavenderEvent build $Event.Percent $Event.Message -JsonEvents:$JsonEvents | Out-Host }
    $start = Get-Date
    if ($Event -and $Event.ContainsKey('To')) {
        # Arguments here are plain words (no spaces), so Start-Process's
        # joining is safe.
        $errorLog = "$log.stderr"
        $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -NoNewWindow -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError $errorLog
        $null = $process.Handle  # keeps ExitCode readable after exit
        $shown = $Event.Percent
        while (-not $process.WaitForExit(3000)) {
            $progress = Get-NinjaProgress -Path $log
            if (-not $progress -or $progress[1] -le 0) { continue }
            $percent = [int]($Event.Percent + ($Event.To - $Event.Percent) * $progress[0] / $progress[1])
            if ($percent -le $shown) { continue }
            $shown = $percent
            Write-LavenderEvent build $percent "$($Event.Message.TrimEnd('.')) ($($progress[0]) of $($progress[1]) files)." `
                -JsonEvents:$JsonEvents | Out-Host
        }
        $process.WaitForExit()
        $exit = $process.ExitCode
        if (Test-Path -LiteralPath $errorLog) {
            Get-Content -LiteralPath $errorLog | Add-Content -LiteralPath $log
            Remove-Item -LiteralPath $errorLog -Force
        }
    }
    else {
        # Windows PowerShell 5.1 turns native stderr (a CMake warning) into
        # terminating errors under 'Stop'; the exit code decides instead.
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { & $FilePath @Arguments *> $log }
        finally { $ErrorActionPreference = $previous }
        $exit = $LASTEXITCODE
    }
    $seconds = [int]((Get-Date) - $start).TotalSeconds
    if ($exit -ne 0) {
        Get-Content -LiteralPath $log -Tail 40 | Out-Host
        throw (New-LavenderCommandFailure -FailureMessage "[$Name] failed with exit code $exit after $seconds s (log: $log)" `
            -Step $Name -LogPath $log -ExitCode $exit `
            -CommandLine (Format-LavenderCommandLine -FilePath $FilePath -Arguments $Arguments))
    }
    Write-Host "[$Name] done in $seconds s"
}

# SPIRV-Tools and glslang (the Vulkan backend) need Python at configure time;
# pin the toolchain's instead of whatever the PATH offers (a pyenv shim fails
# there). glslang reads the older PYTHON_EXECUTABLE.
$python = Get-LavenderPython
$pythonCMake = $python.Replace('\', '/')
$generator = Join-Path $sdk 'out/win-amd64/Release/rexglue.exe'
$manifest = Join-Path $root 'lavender_shift_manifest.toml'

$buildEnvironment = Enter-LavenderBuildEnvironment
$cmake = $buildEnvironment.CMake

if ($Step -in @('generator', 'all')) {
    $manifestFlag = '-Xlinker "/MANIFESTINPUT:' + ((Join-Path $root 'config/codegen-windows.manifest') -replace '\\', '/') + '"'
    Push-Location $sdk
    try {
        Invoke-Logged 'generator-configure' $cmake -Event @{ Percent = 42; Message = 'Configuring the recompiler.' } -Arguments @('--preset', 'win-amd64',
            '-DREXGLUE_ENABLE_TRACY=OFF', '-DSDL_HIDAPI_LIBUSB=OFF',
            "-DPython3_EXECUTABLE=$pythonCMake", "-DPYTHON_EXECUTABLE=$pythonCMake",
            "-DCMAKE_EXE_LINKER_FLAGS=$manifestFlag")
        Invoke-Logged 'generator-build' $cmake -Event @{ Percent = 45; To = 56; Message = 'Building the recompiler.' } -Arguments @('--build', '--preset', 'win-amd64-release',
            '--target', 'rexglue', '--parallel', "$Parallel")
    }
    finally { Pop-Location }
}

if ($Step -in @('codegen', 'all')) {
    if (-not (Test-Path -LiteralPath $generator)) { throw "Build the generator first: $generator" }
    Push-Location $root
    try {
        Invoke-Logged 'codegen' $generator -Event @{ Percent = 58; Message = 'Translating the game code.' } -Arguments @('--log-level', 'info',
            '--log-file', (Join-Path $logs 'codegen-detail.log'),
            'codegen', $manifest, '--xex_apply_patches=true')
    }
    finally { Pop-Location }
}

if ($Step -in @('game', 'all')) {
    $preset = 'win-amd64-' + $Configuration.ToLowerInvariant()
    Push-Location $root
    try {
        Invoke-Logged 'game-configure' $cmake -Event @{ Percent = 66; Message = 'Configuring the game build.' } -Arguments @('--preset', $preset, "-DREXSDK_DIR=$sdk",
            '-DREXGLUE_ENABLE_TRACY=OFF', '-DSDL_HIDAPI_LIBUSB=OFF',
            "-DPython3_EXECUTABLE=$pythonCMake", "-DPYTHON_EXECUTABLE=$pythonCMake")
        Invoke-Logged 'game-build' $cmake -Event @{ Percent = 70; To = 95; Message = 'Compiling the game, the longest step.' } -Arguments @('--build', '--preset', $preset, '--parallel', "$Parallel")
        # The archive helper the AOT pipeline preparation reads the disc with.
        Invoke-Logged 'archive-tool-build' $cmake -Arguments @('--build', '--preset', $preset,
            '--target', 'lavender_shift_archive_extract', '--parallel', "$Parallel")
    }
    finally { Pop-Location }
}
