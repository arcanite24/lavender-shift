# Legal and distribution

Lavender Shift distributes independently authored source, build
configuration, patches and its launcher. Releases do not distribute the
original game, extracted assets, executable modules, generated
translations or a linked game binary.

Players supply a lawfully acquired, personally dumped copy of the supported
Forza Horizon 2 USA disc and the title update v2 package. Setup checks them
against the supported module hashes (`tools/lavender_setup.py`) and reads
them locally. It does not upload, share or modify them, bypass any check,
or help locate game content.

Do not commit or redistribute:

- disc images, executable modules, title updates, keys or extracted assets;
- generated translation source or object files derived from the game's
  executable modules (`generated/`, `out/`);
- frame dumps, captures, logs or builds that contain game data; or
- disassembly listings or decompiled code of the game or its libraries.

`tools/check-repository-boundary.ps1` (rules in
`config/repository-policy.json`) and the audit in
`tools/package-release.ps1` refuse the file types these come in.

## Prebuilt archives

A release attaches a trimmed LLVM and the ShiftGlue kit
(`tools/repack-toolchain.py`, `tools/build-shiftglue-kit.ps1`). Neither is
game-derived: the kit is ShiftGlue's generator, runtime and graphics
backend, built from its pinned source with no game input. No vendor DLL is
in it; AMD's and NVIDIA's runtimes are fetched at the game's configure.
`rexruntime.dll` does link NVIDIA's static NGX loader, so publishing the
kit is a binary release of that loader. The maintainer confirmed on
2026-11-10 that NVIDIA's approval covers it, as it does Pinyon Shift's kit;
the repository variable `LAVENDER_SHIFT_PUBLISH_WINDOWS_KIT` is the switch
that holds it back.

The Microsoft C++ libraries and Windows SDK are never in a release. Setup
fetches them from Microsoft with xwin only after the player accepts
Microsoft's license in the launcher.

## Records of our own analysis

`docs/findings` records what was learned about the game by observation:
an address, a field's meaning, a file format's layout, how it was
established and how sure we are. These records describe behaviour in our
own words. They hold no instruction listings, decompiled code or data
copied from the game.

## Native replacements of game and library code

Lavender Shift replaces some of the title's translated code with native
code (`src/guest_native`, hooks, and replacements of middleware such as
FMOD Ex at its API boundary). A replacement is allowed only when it is
independently authored:

- It implements a contract documented from our own analysis: what the
  function takes, returns and changes in guest state, checked against the
  translated original.
- Its algorithm is our own, or comes from a published source under a
  compatible licence listed in `THIRD_PARTY_NOTICES.md`.

The repository must not hold a function rewritten statement by statement
from the executable, its disassembly or a decompiler's output, nor tables or
constants copied from the executable. A replacement that needs such data
reads it from the player's own files at build or run time.

## Relation to Pinyon Shift

Parts of this repository (the release scripts, the archive reader, the
launcher's payload extractor) are adapted from Pinyon Shift, the
Forza Horizon 1 project by the same authors under the same licence.

The BSD 3-Clause License applies only to the independently authored
material in this repository. It grants no rights to *Forza Horizon*, Xbox
software, ShiftGlue, or any user-supplied content. Forza Horizon is a
trademark of Microsoft; this project is not affiliated with or endorsed by
Microsoft, Playground Games or Turn 10. This document describes the
project's distribution policy and is not legal advice.
