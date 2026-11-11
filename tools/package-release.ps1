[CmdletBinding()]
param(
    # Where the release ZIP and its .sha256 go.
    [string]$OutputDirectory = '',
    # Package the committed HEAD even with uncommitted changes (they are not
    # included either way: the payload is always HEAD).
    [switch]$AllowDirty,
    # RL-10: a folder of prebuilt assets (tools/repack-toolchain.py and
    # tools/build-shiftglue-kit.ps1 outputs with their .json sidecars) and the
    # URL they are published under. The payload's toolchain config then pins
    # them, so setup downloads them instead of building ShiftGlue and
    # unpacking the full LLVM release.
    [string]$PrebuiltAssets = '',
    [string]$ReleaseBaseUrl = ''
)

# RL-5: builds LavenderShift-<version>-windows-x64.zip:
#   LavenderShiftLauncher.exe   single-file, self-contained launcher
#   lavender-shift-source.zip   the committed source tree (git archive of HEAD)
# The payload holds source only. Setup downloads the prebuilt ShiftGlue kit
# and trimmed LLVM the payload's config pins (-PrebuiltAssets), or clones
# ShiftGlue at the pinned revision without them, and the player's disc
# supplies every game file. The audit below refuses console executables, disc images
# and large binaries so game data cannot slip into a release.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'out/release' }
$project = Join-Path $root 'launcher/LavenderShift.Launcher/LavenderShift.Launcher.csproj'
$version = ([xml](Get-Content -Raw -LiteralPath $project)).Project.PropertyGroup.Version | Select-Object -First 1
if (-not $version) { throw "No <Version> in $project" }

$dirty = @(git -C $root status --porcelain --untracked-files=no --ignore-submodules=dirty)
if ($dirty.Count -gt 0 -and -not $AllowDirty) {
    throw "The checkout has uncommitted changes; commit them or pass -AllowDirty (the payload is HEAD):`n$($dirty -join "`n")"
}
$commit = (git -C $root rev-parse HEAD).Trim()
$pin = (Get-Content -Raw -LiteralPath (Join-Path $root 'config/release-toolchain.json') | ConvertFrom-Json).rexglue.revision
$submodule = ((git -C $root ls-tree HEAD thirdparty/shiftglue-sdk) -split '\s+')[2]
if ($submodule -ne $pin) {
    throw "config/release-toolchain.json pins ShiftGlue $pin but HEAD's submodule is $submodule. Update the pin."
}

$work = Join-Path $root "out/release-work"
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
[void](New-Item -ItemType Directory -Force -Path $work, $OutputDirectory)

# The source payload. git archive is deterministic for a commit and skips
# the submodule link. The launcher is not needed to set up the game.
$payload = Join-Path $work 'lavender-shift-source.zip'
git -C $root archive --format=zip --output=$payload $commit -- . ':(exclude)launcher'
if ($LASTEXITCODE) { throw 'git archive failed' }
if ([bool]$PrebuiltAssets -ne [bool]$ReleaseBaseUrl) { throw 'Pass -PrebuiltAssets and -ReleaseBaseUrl together.' }
if ($PrebuiltAssets) {
    $configEntry = 'config/release-toolchain.json'
    $pinned = Join-Path $work 'release-toolchain.json'
    $archive = [IO.Compression.ZipFile]::Open($payload, 'Update')
    try {
        $entry = $archive.GetEntry($configEntry)
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $pinned)
        python -I (Join-Path $PSScriptRoot 'apply-prebuilt-pins.py') --config $pinned --assets $PrebuiltAssets `
            --base-url $ReleaseBaseUrl | Out-Host
        if ($LASTEXITCODE) { throw 'Pinning the prebuilt assets failed' }
        $entry.Delete()
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $pinned, $configEntry)
    }
    finally { $archive.Dispose() }
}

$blocked = '\.(iso|xex|xexp|xcp|stfs|god|bin|zip|7z|rar|bik|xma|xpr|dds|xds|fbo)$'
$archive = [IO.Compression.ZipFile]::OpenRead($payload)
try {
    $problems = @($archive.Entries | Where-Object {
        $_.Name -and ($_.FullName -match $blocked -or $_.Length -gt 2MB)
    } | ForEach-Object { "$($_.FullName) ($([Math]::Round($_.Length / 1KB)) KiB)" })
    $count = @($archive.Entries | Where-Object { $_.Name }).Count
}
finally { $archive.Dispose() }
# The AOT catalog holds pipeline hashes and render-state edits, no shader
# code (tools/aot_pipelines.py); it is the one large file allowed.
$problems = @($problems | Where-Object { $_ -notmatch '^config/aot-pipelines\.json ' })
if ($problems.Count -gt 0) {
    throw "The payload holds files a release must not ship:`n$($problems -join "`n")"
}

$publish = Join-Path $work 'publish'
dotnet publish $project -c Release -o $publish --nologo | Out-Host
if ($LASTEXITCODE) { throw 'dotnet publish failed' }

$stage = Join-Path $work 'stage'
[void](New-Item -ItemType Directory -Force -Path $stage)
Copy-Item -LiteralPath (Join-Path $publish 'LavenderShiftLauncher.exe') -Destination $stage
Copy-Item -LiteralPath $payload -Destination $stage
$readme = @"
Lavender Shift $version

1. Extract this whole ZIP into a folder with 20 GB free and a short path,
   for example C:\Games\LavenderShift.
2. Start LavenderShiftLauncher.exe.
3. Choose your Forza Horizon 2 USA disc image (.iso) and title update v2,
   then press Set up. The first setup builds the game on this PC and takes
   15 minutes to an hour, depending on the PC.

Your saves are in the user folder beside the launcher. To update, extract
a newer release into the same folder and run setup again.

Needs Windows 10 2004 or newer, a CPU with AVX2 (Intel Haswell, AMD Zen
or newer) and a Vulkan GPU.

No game files are included. Lavender Shift only runs from a copy of the
game you own.

Source: commit $commit
"@
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme.Replace("`n", "`r`n"), [Text.UTF8Encoding]::new($false))

$name = "LavenderShift-$version-windows-x64.zip"
$zip = Join-Path $OutputDirectory $name
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$zip.sha256", "$hash  $name`n", [Text.UTF8Encoding]::new($false))
Remove-Item -LiteralPath $work -Recurse -Force

[pscustomobject]@{
    release = $zip
    sha256 = $hash
    version = $version
    commit = $commit
    payload_files = $count
    size_mb = [Math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)
}
