# Public release backlog (RL-*)

Goal: a player with a Windows PC, their own Forza Horizon 2 USA disc image
and the title update v2 package downloads one archive, runs the launcher,
and plays. Nothing from the game is distributed. The model is Pinyon
Shift's (its `docs/INSTALLING.md`): the archive holds the launcher and
this repository's source; the launcher fetches pinned build tools and
ShiftGlue, extracts the player's files, generates the translation and
compiles the game on the player's machine.

Scope of the first release: Windows 10 2004+ x64, Vulkan, the Stage 0
build. Linux, macOS, Android and mods come later.

## What Pinyon already has and Lavender reuses

| Piece | Pinyon | Lavender |
| --- | --- | --- |
| Pinned tools | `config/release-toolchain.json`, `tools/provision-toolchain.ps1` | same pins, Lavender's own copy |
| Shared script helpers | `tools/release-common.ps1` (downloads with SHA-256, events, local paths) | trimmed copy |
| Setup | `tools/setup-preview.ps1`: verify, extract, codegen, build, JSON events | `tools/setup-release.ps1` around `lavender_setup.py` and `build.ps1` |
| Launch | `tools/launch-preview.ps1` with a state root | `tools/launch.ps1 -UserRoot` |
| Launcher | WPF (`launcher/PinyonShift.Launcher`), 2.8k lines of window code | a new, small WPF window with Pinyon's payload extractor; no Android, wheel, DLC, radio |
| Packaging | `tools/package-launcher.ps1/.py`, `release.yml` | `tools/package-release.ps1`, Windows only |

## Items

| ID | Item | Done when | Status |
| --- | --- | --- | --- |
| RL-1 | **Release toolchain**: `config/release-toolchain.json` (CMake, Ninja, LLVM, Python, MinGit, Build Tools, ShiftGlue pin) and `tools/provision-toolchain.ps1` | a fresh folder provisions every tool from pinned, hashed downloads; `build.ps1` reads the manifest instead of fixed folder names | done: a fresh folder (`D:\lavender-rl-test`, no `.local`) provisions every tool under Windows PowerShell 5.1; `build.ps1` reads the manifest |
| RL-2 | **Setup script**: `tools/setup-release.ps1 -IsoPath -UpdatePath -JsonEvents`, checks Windows, disk space, running game; disc and update verify; extract; generator; codegen; build; `setup-error.json` on failure | a source archive plus the two player files reach a built exe with no checkout and no Pinyon toolchain | open |
| RL-3 | **State root**: saves, config and logs under one player folder (`%LOCALAPPDATA%\LavenderShift\...` or portable `data\`), passed to the game | launch from the launcher and from `launch.ps1` share one save | done: portable layout, `user\` beside the launcher (see Decisions); `launch.ps1 -UserRoot` and the launcher use the same folder |
| RL-4 | **Launcher**: WPF, Lavender branding; first-run screen (choose disc image and title update, install folder, progress per stage, failure report); play screen (Play, settings that exist in the game's own menu stay there, open logs and saves folders, check for updates) | setup and play work from the packaged archive on multivac and on blindvac | first version: `launcher/LavenderShift.Launcher` (setup, progress, failure report, play, saves and logs folders); no update check yet; packaged run pending |
| RL-5 | **Packaging**: `tools/package-launcher.ps1` builds `LavenderShift-Launcher.zip` with the launcher, a source payload (git archive of the tag plus the ShiftGlue pin) and SHA-256 | the zip installs and plays on a machine that never had the repository | script written: `tools/package-release.ps1`; packaged install not yet tested |
| RL-6 | **Repository readiness**: README for players, `docs/LEGAL.md`, LICENSE, THIRD_PARTY_NOTICES, CONTRIBUTING, SECURITY, issue templates; audit that no game data, disassembly or keys are tracked | an audit script finds nothing; the docs say what a player needs | open |
| RL-7 | **CI and release workflow**: build check on push; tag `v*` builds the launcher zip and drafts a release with notes and the hash | a test tag produces a draft release | open |
| RL-8 | **Release candidate**: clean-machine run (blindvac, a fresh Windows user), a 30-minute play session, known issues written down | the user approves publishing | open |
| RL-9 | **Publish**: create the public GitHub repository, push, publish 0.1.0 | user's call; nothing is published before it | open |

## Decisions taken

- WPF launcher first: the Windows-only release does not need Avalonia;
  Pinyon's Desktop (Avalonia) launcher is the template when Linux comes.
- The player provides both the disc image and the title update; the
  launcher checks them against the supported hashes before extracting.
- Settings live in the game's F6 menu; the launcher keeps only what must
  be set before start (none yet).
- Portable install (RL-3): everything lives under the folder the release
  ZIP was extracted to. `source\` holds the unpacked payload, toolchain,
  extracted game files and build; `user\` holds saves, settings, cache and
  game logs; `launcher.json` remembers the chosen files. The source folder
  keeps its name across releases, so an update extracts over it (removing
  files the old payload listed and the new one drops), rebuilds only what
  changed and keeps the extracted disc. Run from a checkout, the launcher
  uses the checkout and `.local\user`.
- The disc and update are checked against the supported module hashes
  (`SUPPORTED_DISC_MODULES`, `SUPPORTED_UPDATE_PATCHES` in
  `tools/lavender_setup.py`) before anything is written.
- Progress: setup emits `::lavender::{stage, percent, message}` lines;
  the two Ninja builds report their `[done/total]` counter, so the bar
  moves during the hour-long first build.
