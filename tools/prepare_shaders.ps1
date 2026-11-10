[CmdletBinding()]
param(
    # Player data whose cache receives the pipelines. Defaults to .local/user.
    [string]$UserRoot = '',
    # Pinyon Shift's archive helper (method-21 ZIP members and XEX images);
    # Lavender does not build its own yet.
    [string]$ArchiveExtractor = '',
    # Rebuild even when the record says the file is current.
    [switch]$Force
)

# Writes <user>/cache/shaders/shareable/4D530AA4.fbo.aot.xpso: every
# pipeline in config/aot-pipelines.json, rebuilt from the player's own disc,
# title update and patched executable. ShiftGlue builds them in the
# background from the start, so the first drive does not wait for them
# (vulkan_aot_pipelines). The catalog holds hashes and edits only, no game
# data; tools/aot_pipelines.py describes it. Skips the work when the catalog
# has not changed since the last run (<user>/cache/aot-pipelines.json).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $UserRoot) { $UserRoot = Join-Path $root '.local/user' }
if (-not $ArchiveExtractor) {
    $ArchiveExtractor = Join-Path (Split-Path -Parent $root) 'horizon1-recomp/out/build/win-amd64-release/pinyon_shift_fh1_archive_extract.exe'
}
$python = Join-Path $root '.local/toolchain/python-3.13.15/python.exe'
$catalog = Join-Path $root 'config/aot-pipelines.json'
$cache = Join-Path $UserRoot 'cache'
$output = Join-Path $cache 'shaders/shareable/4D530AA4.fbo.aot.xpso'
$record = Join-Path $cache 'aot-pipelines.json'

$key = (Get-FileHash -Algorithm SHA256 -LiteralPath $catalog).Hash
if (-not $Force -and (Test-Path -LiteralPath $output) -and (Test-Path -LiteralPath $record) -and
    ((Get-Content -Raw -LiteralPath $record | ConvertFrom-Json).catalog -eq $key)) {
    Write-Host 'AOT pipelines are current.'
    return
}
if (-not (Test-Path -LiteralPath $ArchiveExtractor)) {
    Write-Warning "AOT pipelines skipped: $ArchiveExtractor is not built"
    return
}

$work = Join-Path $root ".local/aot/work-$([guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Force -Path $work)
try {
    # The executable as it runs: TU v2 applied (codegen keeps each XEX beside
    # its .xexp).
    $image = Join-Path $work 'default.v2.bin'
    & $ArchiveExtractor --xex-image (Join-Path $root '.local/game/codegen/default.xex') $image --apply-patch
    if ($LASTEXITCODE) { throw "could not extract the patched executable image" }
    & $python -I (Join-Path $PSScriptRoot 'extract_shader_corpus.py') (Join-Path $root '.local/game/disc') `
        --output (Join-Path $work 'corpus.json') --binary-dir (Join-Path $work 'corpus') `
        --archive-extractor $ArchiveExtractor --content-root (Join-Path $root '.local/game/update') `
        --executable-image $image
    if ($LASTEXITCODE) { throw 'shader corpus extraction failed' }
    [void](New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output))
    & $python -I (Join-Path $PSScriptRoot 'aot_pipelines.py') generate --corpus (Join-Path $work 'corpus') `
        --catalog $catalog --output $output
    if ($LASTEXITCODE) { throw 'AOT pipeline generation failed' }
    @{ schema_version = 1; catalog = $key } | ConvertTo-Json | Set-Content -LiteralPath $record -Encoding utf8
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
