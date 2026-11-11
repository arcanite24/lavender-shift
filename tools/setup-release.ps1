[CmdletBinding()]
param(
    # The player's Forza Horizon 2 USA disc image.
    [Parameter(Mandatory)] [ValidateNotNullOrEmpty()] [string]$IsoPath,
    # The title update v2 package, or a ZIP holding it.
    [Parameter(Mandatory)] [ValidateNotNullOrEmpty()] [string]$UpdatePath,
    # Player data folder whose cache receives the ahead-of-time pipelines.
    [string]$UserRoot = '',
    # One '::lavender::{json}' line per progress event, for the launcher.
    [switch]$JsonEvents
)

# RL-2: from a source tree (a checkout or the release archive's payload) and
# the player's two files to a built game. Each step skips work that is
# already done, so a failed setup continues where it stopped. The player's
# files are only read. On failure .local/logs/setup-error.json says which
# step failed and why (the same record Pinyon Shift's launcher reads).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# The launcher reads the redirected output as UTF-8 (file names may not be
# ASCII); Windows PowerShell would otherwise write the OEM code page.
if ($JsonEvents) { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) }

. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = Get-LavenderRepoRoot
$logs = Resolve-LavenderLocalPath -RelativePath '.local/logs'
[void](New-Item -ItemType Directory -Force -Path $logs)
$errorPath = Join-Path $logs 'setup-error.json'
if (Test-Path -LiteralPath $errorPath) { Remove-Item -LiteralPath $errorPath -Force }
$statePath = Resolve-LavenderLocalPath -RelativePath '.local/setup-state.json'
$gameRoot = Resolve-LavenderLocalPath -RelativePath '.local/game'

# Runs tools/lavender_setup.py and turns its "disc: x of y GB" lines into
# progress between two percentages.
function Invoke-GameSetup {
    param([string[]]$Arguments, [string]$Step, [int]$From, [int]$To)
    $python = Get-LavenderPython
    $log = Join-Path $logs "setup-$($Arguments[0]).log"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $python -I (Join-Path $PSScriptRoot 'lavender_setup.py') @Arguments 2>&1 | ForEach-Object {
        $line = $_.ToString()
        Add-Content -LiteralPath $log -Value $line
        if ($line -match '^disc: ([\d.]+) of ([\d.]+) GB') {
            $share = [double]$Matches[1] / [Math]::Max([double]$Matches[2], 0.01)
            Write-LavenderEvent extract ([int]($From + ($To - $From) * $share)) `
                "Extracting your disc image ($($Matches[1]) of $($Matches[2]) GB)." -JsonEvents:$JsonEvents
        }
    }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) {
        $last = @(Get-Content -LiteralPath $log -Tail 1)
        $failure = [Exception]::new("$Step failed: $($last -join ' ')")
        $failure.Data['step'] = $Step
        $failure.Data['exit_code'] = $code
        $failure.Data['log'] = $log
        $failure.Data['hint'] = 'Lavender Shift supports the Forza Horizon 2 USA disc with title update v2. Choose those files and start setup again.'
        throw $failure
    }
}

# Whether an earlier setup already extracted this exact source file.
function Test-Extracted {
    param([string]$Catalog, [string]$Source)
    if (-not (Test-Path -LiteralPath $Catalog -PathType Leaf)) { return $false }
    $data = Get-Content -LiteralPath $Catalog -Raw | ConvertFrom-Json
    $data.source -eq [IO.Path]::GetFileName($Source)
}

try {
    $config = Get-LavenderReleaseToolchain
    $iso = (Resolve-Path -LiteralPath $IsoPath).Path
    $update = (Resolve-Path -LiteralPath $UpdatePath).Path
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Lavender Shift requires 64-bit Windows.' }
    if ([Environment]::OSVersion.Version.Build -lt [int]$config.minimum_windows_build) {
        throw "Lavender Shift requires Windows build $($config.minimum_windows_build) or newer."
    }
    if (@(Get-Process -Name 'lavender_shift' -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'Close Lavender Shift before setup. Windows locks the game files while it runs.'
    }
    $firstBuild = -not (Test-Path -LiteralPath $statePath -PathType Leaf)
    $buildToolsMissing = [string]::IsNullOrWhiteSpace((Get-LavenderVisualStudioRoot -AllowMissing))
    Assert-LavenderFreeSpace -Root $root -FirstBuild:$firstBuild -BuildToolsMissing:$buildToolsMissing

    Write-LavenderEvent tools 5 'Preparing the build tools. Nothing from your game is uploaded.' -JsonEvents:$JsonEvents
    & (Join-Path $PSScriptRoot 'provision-toolchain.ps1') -JsonEvents:$JsonEvents | Out-Host
    & (Join-Path $PSScriptRoot 'prepare-rexglue.ps1') -JsonEvents:$JsonEvents | Out-Host

    if (Test-Extracted -Catalog (Join-Path $gameRoot 'disc-catalog.json') -Source $iso) {
        Write-LavenderEvent extract 40 'Your game files are already extracted.' -JsonEvents:$JsonEvents
    }
    else {
        Write-LavenderEvent extract 20 'Extracting your disc image. The original file is not modified.' -JsonEvents:$JsonEvents
        Invoke-GameSetup -Arguments @('disc', $iso) -Step 'Extract the disc image' -From 20 -To 38
    }
    if (Test-Extracted -Catalog (Join-Path $gameRoot 'update-catalog.json') -Source $update) {
        Write-LavenderEvent extract 40 'The title update is already extracted.' -JsonEvents:$JsonEvents
    }
    else {
        Write-LavenderEvent extract 39 'Reading the title update.' -JsonEvents:$JsonEvents
        Invoke-GameSetup -Arguments @('update', $update) -Step 'Extract the title update' -From 39 -To 40
    }
    Invoke-GameSetup -Arguments @('codegen') -Step 'Prepare the executables for translation' -From 41 -To 41

    & (Join-Path $PSScriptRoot 'build.ps1') -Step all -JsonEvents:$JsonEvents | Out-Host

    Write-LavenderEvent shaders 96 'Preparing graphics pipelines from your disc.' -JsonEvents:$JsonEvents
    $shaderArguments = @{}
    if ($UserRoot) { $shaderArguments.UserRoot = $UserRoot }
    & (Join-Path $PSScriptRoot 'prepare_shaders.ps1') @shaderArguments | Out-Host

    $state = [ordered]@{
        schema_version = 1
        completed_utc = [DateTime]::UtcNow.ToString('o')
        disc = [IO.Path]::GetFileName($iso)
        update = [IO.Path]::GetFileName($update)
        rexglue_revision = $config.rexglue.revision
        result = 'ready'
    }
    [IO.File]::WriteAllText($statePath, ($state | ConvertTo-Json) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
    Write-LavenderEvent play 100 'Ready to play.' -JsonEvents:$JsonEvents
}
catch {
    # Plain text and an exit code: Write-Error under 'Stop' would hide the
    # real step behind its own position.
    $failure = $_
    try {
        $record = New-LavenderFailureRecord -ErrorRecord $failure
        $record.system = Get-LavenderSystemSummary -Root $root
        [IO.File]::WriteAllText($errorPath, ($record | ConvertTo-Json -Depth 4) + [Environment]::NewLine,
            [Text.UTF8Encoding]::new($false))
        foreach ($line in (Format-LavenderFailureRecord -Record $record)) { [Console]::Out.WriteLine($line) }
        [Console]::Out.WriteLine("Failure details: $errorPath")
    }
    catch {
        [Console]::Out.WriteLine("Setup failed: $($failure.Exception.Message)")
        [Console]::Out.WriteLine("The failure report could not be written: $($_.Exception.Message)")
    }
    [Console]::Out.Flush()
    [Console]::Error.WriteLine("Setup failed: $($failure.Exception.Message)")
    exit 1
}
