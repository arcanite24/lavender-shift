#!/usr/bin/env python3
"""Copy guest-named shader replacements to the exact names of a discovery
frame.

ShiftGlue takes a replacement under its exact identity,
shader_<guest>_<modification>_<translated hash>.spv, or under a guest name,
shader_<guest>_<modification>.spv or shader_<guest>.spv, which serves every
translation of that guest shader it fits (docs/SHADER_REPLACEMENT_RESEARCH.md).
The translated hash changes with the resolution scale, the platform and the
translator, so guest names are the ones to ship. This writes the exact names
for authors who want one file per translation, for example to give one
resolution scale its own version.

    shader-override-names.py --discovery <dir> --overrides <dir> --out <dir>

--discovery is a vulkan_shader_discovery_frame folder (its
shader_<guest>_<modification>_<hash>.spv translations name the identities
the frame used). Nothing is read from inside the files, and the game still
checks each replacement's interface when it loads it.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

EXACT = re.compile(r"^shader_([0-9A-F]{16})_([0-9A-F]{16})_([0-9A-F]{16})\.spv$", re.IGNORECASE)
GUEST = re.compile(r"^shader_([0-9A-F]{16})(?:_([0-9A-F]{16}))?\.spv$", re.IGNORECASE)


def identities(discovery: Path) -> list[tuple[str, str, str]]:
    found = []
    for path in sorted(discovery.glob("shader_*.spv")):
        match = EXACT.match(path.name)
        if match:
            found.append(tuple(part.upper() for part in match.groups()))
    return found


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--discovery", type=Path, required=True)
    parser.add_argument("--overrides", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true", help="only list what would be written")
    args = parser.parse_args(argv)

    known = identities(args.discovery)
    if not known:
        print(f"no shader_<guest>_<modification>_<hash>.spv files in {args.discovery}",
              file=sys.stderr)
        return 1
    if not args.dry_run:
        args.out.mkdir(parents=True, exist_ok=True)
    written = 0
    for source in sorted(args.overrides.glob("shader_*.spv")):
        match = GUEST.match(source.name)
        if not match:
            continue
        guest = match.group(1).upper()
        modification = match.group(2).upper() if match.group(2) else None
        targets = [f"shader_{g}_{m}_{h}.spv" for g, m, h in known
                   if g == guest and (modification is None or m == modification)]
        if not targets:
            print(f"{source.name}: not in this discovery frame")
            continue
        for name in targets:
            print(f"{source.name} -> {name}")
            if not args.dry_run:
                shutil.copyfile(source, args.out / name)
            written += 1
    print(f"{written} file(s){' would be' if args.dry_run else ''} written")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
