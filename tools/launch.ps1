[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    # Player data (saves, cache, logs). Defaults to .local/user.
    [string]$UserRoot = '',
    # Extra cvars, e.g. '--log_level=debug'.
    [string[]]$GameArguments = @()
)

# Starts the Stage 0 reference build against the extracted game:
# .local/game/disc as game:\ and d:\, .local/game/update as update:\ (the
# title update's patches are applied while loading, as at codegen).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$preset = 'win-amd64-' + $Configuration.ToLowerInvariant()
$exe = Join-Path $root "out/build/$preset/lavender_shift.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Build first: $exe" }
$disc = Join-Path $root '.local/game/disc'
$update = Join-Path $root '.local/game/update'
foreach ($folder in @($disc, $update)) {
    if (-not (Test-Path -LiteralPath $folder)) { throw "Run tools/lavender_setup.py first: $folder" }
}
if (-not $UserRoot) { $UserRoot = Join-Path $root '.local/user' }
[void](New-Item -ItemType Directory -Force -Path $UserRoot)
if (Get-Process -Name 'lavender_shift' -ErrorAction SilentlyContinue) {
    throw 'lavender_shift is already running.'
}
# FH2 needs XamXStudioRequest to fail, or the garage stays under its loading
# screen (ShiftGlue keeps FH1's behavior by default).
$arguments = @("--game_data_root=$disc", "--update_data_root=$update",
    "--user_data_root=$UserRoot", '--xex_apply_patches=true', '--gpu_backend=vulkan',
    '--xam_xstudio_request_unsupported=true') + $GameArguments
Write-Host "$exe $($arguments -join ' ')"
Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) -PassThru
