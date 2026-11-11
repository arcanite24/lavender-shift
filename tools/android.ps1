# Runs tools/lavender_android.py with the toolchain's Python, for the
# launcher's Android panel (docs/ANDROID.md):
#   tools/android.ps1 doctor --install --accept-licenses
#   tools/android.ps1 build --jobs 16
#   tools/android.ps1 --serial <device> install
# The arguments pass through unchanged; the exit code is the script's.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')

# Not -I: the embeddable Python's path holds only its own folder, and the
# script adds its folder for apk_sign. -E and -s keep the player's
# PYTHONPATH and site packages out.
$python = Get-LavenderPython
$env:PYTHONUNBUFFERED = '1'
$env:PYTHONIOENCODING = 'utf-8'
& $python -E -s (Join-Path $PSScriptRoot 'lavender_android.py') @args
exit $LASTEXITCODE
