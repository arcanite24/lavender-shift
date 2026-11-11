[CmdletBinding()]
param([switch]$JsonEvents)

# RL-1: provisions every build tool config/release-toolchain.json pins, from
# hashed downloads, into .local/toolchain; the Microsoft C++ Build Tools are
# installed (one administrator prompt) when no usable Visual Studio exists.
# Adapted from Pinyon Shift's tools/provision-toolchain.ps1, without its
# prebuilt-kit, sysroot and extract-xiso paths: Lavender reads the disc
# image itself (tools/lavender_setup.py) and always builds ShiftGlue from
# source.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = Get-LavenderRepoRoot
$config = Get-LavenderReleaseToolchain
$downloads = Resolve-LavenderLocalPath -RelativePath '.local/downloads'
[void](New-Item -ItemType Directory -Force -Path $downloads)

Write-LavenderEvent tools 18 'Checking the Windows build environment.' -JsonEvents:$JsonEvents
$vsRoot = Get-LavenderVisualStudioRoot -AllowMissing
# Only the hosts of downloads still to come: a provisioned PC builds offline.
$pending = @(foreach ($name in @('git', 'xz', 'llvm', 'python', 'cmake', 'ninja')) {
    $tool = $config.$name
    if (-not (Test-Path -LiteralPath (Join-Path (Join-Path $root $tool.install_path) $tool.executable) -PathType Leaf)) {
        $tool.url
    }
})
if ([string]::IsNullOrWhiteSpace($vsRoot)) { $pending += $config.visual_studio.bootstrap_url }
if (-not (Test-Path -LiteralPath (Join-Path (Resolve-LavenderRexGlueRoot) 'CMakeLists.txt') -PathType Leaf)) {
    $pending += $config.rexglue.repository
}
Assert-LavenderDownloadHosts -Uris $pending

# Downloads and checks the signed Build Tools bootstrapper.
function Get-BuildToolsBootstrapper {
    $bootstrap = Join-Path $downloads 'vs_BuildTools.exe'
    if (-not (Test-Path -LiteralPath $bootstrap -PathType Leaf)) {
        $partial = "$bootstrap.partial"
        Invoke-WebRequest -Uri $config.visual_studio.bootstrap_url -OutFile $partial -UseBasicParsing
        Move-Item -LiteralPath $partial -Destination $bootstrap -Force
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $bootstrap
    if ($signature.Status -ne 'Valid' -or
        $signature.SignerCertificate.Subject -notlike "*$($config.visual_studio.bootstrap_signer)*") {
        Remove-Item -LiteralPath $bootstrap -Force
        throw 'The Microsoft Build Tools installer signature is invalid.'
    }
    $bootstrap
}

# Runs install-build-tools.ps1 elevated (one administrator prompt), with a
# heartbeat while the installer works. Returns whether Windows must restart
# to finish (exit code 3010).
function Invoke-BuildToolsInstaller {
    param([Parameter(Mandatory)] [string]$Arguments, [Parameter(Mandatory)] [string]$Activity)
    $helper = Join-Path $PSScriptRoot 'install-build-tools.ps1'
    $startInfo = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path,
        "-NoLogo -NoProfile -ExecutionPolicy Bypass -File `"$helper`" $Arguments")
    $startInfo.Verb = 'runas'
    $startInfo.UseShellExecute = $true
    try {
        $elevated = [Diagnostics.Process]::Start($startInfo)
    }
    catch {
        $win32 = $_.Exception.InnerException -as [ComponentModel.Win32Exception]
        if ($null -ne $win32 -and $win32.NativeErrorCode -eq 1223) {
            throw ('Windows administrator permission was declined, so the Microsoft C++ Build Tools ' +
                'were not installed. The build needs them: start setup again and choose Yes when ' +
                'Windows asks for permission.')
        }
        throw
    }
    $started = [Diagnostics.Stopwatch]::StartNew()
    while (-not $elevated.WaitForExit(15000)) {
        Write-LavenderEvent tools 21 "$Activity ($([int]$started.Elapsed.TotalMinutes) min so far; the installer window shows its progress)." `
            -JsonEvents:$JsonEvents | Out-Host
    }
    if ($elevated.ExitCode -notin @(0, 3010)) {
        $reason = switch ($elevated.ExitCode) {
            1001 { ' The Visual Studio Installer is already running: close it, then start setup again.' }
            { $_ -in 1003, 8006 } { ' Visual Studio or one of its tools is running: close it, then start setup again.' }
            { $_ -in 1602, 5004 } { ' The installation was cancelled.' }
            1618 { ' Another installation is running: wait for it to finish, then start setup again.' }
            5007 { ' The installer says this PC does not meet its requirements.' }
            -1073720687 { ' The installer could not download its files: check the internet connection.' }
            default { '' }
        }
        $where = "The Visual Studio Installer's own logs are the dd_*.log files in $env:TEMP."
        $since = (Get-Date).Add(-$started.Elapsed).AddMinutes(-1)
        $installerLogs = @(Get-ChildItem -LiteralPath $env:TEMP -Filter 'dd_*.log' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $since })
        if ($installerLogs.Count -gt 0) {
            $copy = Resolve-LavenderLocalPath -RelativePath '.local/logs/build-tools-installer'
            try {
                [void](New-Item -ItemType Directory -Force -Path $copy)
                $installerLogs | Copy-Item -Destination $copy -Force
                $where = "The Visual Studio Installer's own logs from this attempt were copied to $copy."
            }
            catch { }
        }
        throw ("Microsoft C++ Build Tools installation stopped with exit code $($elevated.ExitCode).$reason $where")
    }
    $elevated.ExitCode -eq 3010
}

$restartPending = $false
if ([string]::IsNullOrWhiteSpace($vsRoot)) {
    Write-LavenderEvent tools 20 'Microsoft C++ Build Tools are required. Windows may ask for administrator permission.' -JsonEvents:$JsonEvents
    $bootstrap = Get-BuildToolsBootstrapper
    $restartPending = Invoke-BuildToolsInstaller -Arguments "-Bootstrapper `"$bootstrap`"" `
        -Activity 'Installing the Microsoft C++ Build Tools'
    $vsRoot = Get-LavenderVisualStudioRoot
}

Write-LavenderEvent tools 23 'Preparing Git.' -JsonEvents:$JsonEvents
Install-LavenderGit

Write-LavenderEvent tools 26 'Preparing the pinned LLVM compiler.' -JsonEvents:$JsonEvents
$xzRoot = [IO.Path]::GetFullPath((Join-Path $root $config.xz.install_path))
$xzExe = Join-Path $xzRoot $config.xz.executable
if (-not (Test-Path -LiteralPath $xzExe -PathType Leaf)) {
    $archive = Join-Path $downloads "xz-$($config.xz.version)-windows.zip"
    Invoke-LavenderDownload -Uri $config.xz.url -Destination $archive -Sha256 $config.xz.sha256
    if (Test-Path -LiteralPath $xzRoot) { Remove-Item -LiteralPath $xzRoot -Recurse -Force }
    [void](New-Item -ItemType Directory -Force -Path $xzRoot)
    Expand-LavenderZip -Archive $archive -Destination $xzRoot
}

$llvmRoot = [IO.Path]::GetFullPath((Join-Path $root $config.llvm.install_path))
$llvmExe = Join-Path $llvmRoot $config.llvm.executable
if (-not (Test-Path -LiteralPath $llvmExe -PathType Leaf)) {
    $archive = Join-Path $downloads "llvm-$($config.llvm.version).tar.xz"
    Invoke-LavenderDownload -Uri $config.llvm.url -Destination $archive -Sha256 $config.llvm.sha256
    $staging = Resolve-LavenderLocalPath -RelativePath ".local/toolchain/llvm-$($config.llvm.version)-staging"
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    [void](New-Item -ItemType Directory -Force -Path $staging)
    Expand-LavenderTarXz -Archive $archive -Destination $staging -XzPath $xzExe
    $children = @(Get-ChildItem -LiteralPath $staging -Directory)
    if ($children.Count -ne 1) { throw 'The LLVM archive layout is not recognized.' }
    if (Test-Path -LiteralPath $llvmRoot) { Remove-Item -LiteralPath $llvmRoot -Recurse -Force }
    Move-Item -LiteralPath $children[0].FullName -Destination $llvmRoot
    Remove-Item -LiteralPath $staging -Recurse -Force
}

Write-LavenderEvent tools 30 'Preparing the local Python runtime.' -JsonEvents:$JsonEvents
$pythonRoot = Resolve-LavenderLocalPath -RelativePath $config.python.install_path
$pythonExe = Join-Path $pythonRoot $config.python.executable
$pythonCheck = "import hashlib, json, struct, tomllib, zipfile; assert hasattr(hashlib, 'file_digest')"
$pythonReady = $false
if (Test-Path -LiteralPath $pythonExe -PathType Leaf) {
    & $pythonExe -I -c $pythonCheck
    $pythonReady = $LASTEXITCODE -eq 0
}
if (-not $pythonReady) {
    $archive = Join-Path $downloads "python-$($config.python.version)-embed-amd64.zip"
    Invoke-LavenderDownload -Uri $config.python.url -Destination $archive -Sha256 $config.python.sha256
    [void](New-Item -ItemType Directory -Force -Path $pythonRoot)
    Expand-LavenderZip -Archive $archive -Destination $pythonRoot
}
& $pythonExe -I -c $pythonCheck
if ($LASTEXITCODE -ne 0) { throw 'The local Python runtime failed its check.' }

Write-LavenderEvent tools 31 'Preparing CMake and Ninja.' -JsonEvents:$JsonEvents
$cmakeRoot = Resolve-LavenderLocalPath -RelativePath $config.cmake.install_path
if (-not (Test-Path -LiteralPath (Join-Path $cmakeRoot $config.cmake.executable) -PathType Leaf)) {
    $archive = Join-Path $downloads "cmake-$($config.cmake.version).zip"
    Invoke-LavenderDownload -Uri $config.cmake.url -Destination $archive -Sha256 $config.cmake.sha256
    Expand-LavenderZip -Archive $archive -Destination (Split-Path $cmakeRoot -Parent)
}
$ninjaRoot = Resolve-LavenderLocalPath -RelativePath $config.ninja.install_path
if (-not (Test-Path -LiteralPath (Join-Path $ninjaRoot $config.ninja.executable) -PathType Leaf)) {
    $archive = Join-Path $downloads "ninja-$($config.ninja.version)-win.zip"
    Invoke-LavenderDownload -Uri $config.ninja.url -Destination $archive -Sha256 $config.ninja.sha256
    [void](New-Item -ItemType Directory -Force -Path $ninjaRoot)
    Expand-LavenderZip -Archive $archive -Destination $ninjaRoot
}

# A Visual Studio without a usable Windows SDK or with an old C++ library is
# completed with the installer, then checked again.
function New-RestartRequiredFailure {
    $hint = 'Windows must restart to finish installing the Microsoft C++ Build Tools. Restart, then start setup again; it continues where it stopped.'
    $failure = [Exception]::new($hint)
    $failure.Data['step'] = 'Finish the Microsoft C++ Build Tools installation'
    $failure.Data['error_kind'] = 'reboot-required'
    $failure.Data['hint'] = $hint
    $failure
}
try {
    $environment = Enter-LavenderBuildEnvironment
}
catch {
    $kind = $_.Exception.Data['error_kind']
    if ($restartPending) { throw (New-RestartRequiredFailure) }
    if ($kind -ne 'toolchain-capability') { throw }
    Write-LavenderEvent tools 21 'Updating Visual Studio and adding the Windows SDK the build needs. Windows may ask for administrator permission.' -JsonEvents:$JsonEvents
    $bootstrap = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    if (-not (Test-Path -LiteralPath $bootstrap -PathType Leaf)) { $bootstrap = Get-BuildToolsBootstrapper }
    $restartPending = Invoke-BuildToolsInstaller -Activity 'Updating the Microsoft C++ Build Tools' -Arguments (
        "-Bootstrapper `"$bootstrap`" -Mode Repair -InstallPath `"$vsRoot`" -Add " +
        ((@($config.visual_studio.repair_components) | ForEach-Object { "`"$_`"" }) -join ','))
    try { $environment = Enter-LavenderBuildEnvironment }
    catch {
        if ($restartPending) { throw (New-RestartRequiredFailure) }
        throw
    }
}
$git = Get-LavenderGit
foreach ($required in @(
    @{ Name = 'Git'; Path = $git },
    @{ Name = 'XZ'; Path = $xzExe },
    @{ Name = 'CMake'; Path = $environment.CMake },
    @{ Name = 'Ninja'; Path = $environment.Ninja },
    @{ Name = 'LLVM'; Path = $llvmExe },
    @{ Name = 'Python'; Path = $pythonExe }
)) {
    if (-not (Test-Path -LiteralPath $required.Path -PathType Leaf)) {
        throw "$($required.Name) was not provisioned correctly: $($required.Path)"
    }
}
Write-LavenderEvent tools 32 'Windows build tools are ready.' -JsonEvents:$JsonEvents

[pscustomobject]@{
    result = 'pass'
    git = $git
    cmake = $environment.CMake
    ninja = $environment.Ninja
    llvm = $llvmExe
    python = $pythonExe
}
