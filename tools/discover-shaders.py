#!/usr/bin/env python3
"""List a Vulkan shader discovery frame, without disassembling game code.

`--vulkan_shader_discovery_frame=<frame>` records one frame's draws
(draws.jsonl), resolves (resolves.jsonl) and the translated shaders
(shader_<guest>_<modification>_<translated hash>.spv) into a folder under
.local/. This lists every shader with its draws, HUD draws and the surfaces
it drew to, and can check a mod's shaders/ folder against the frame:

    discover-shaders.py <discovery dir> [--shader 6145880227] [--surface 00600804]
                        [--check-mod <mod>/shaders] [--json]

A replacement named by exact identity "matches" when the frame used that
translation and is "stale" when the frame translated the same guest shader
differently (another resolution scale, platform or SDK). A guest name
(shader_<guest>.spv, shader_<guest>_<modification>.spv) lists the
translations it would be tried for; the game still checks its interface.

Ported from Pinyon Shift's discover-fh1-shaders.py (BSD-3-Clause, same
authors).
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

IDENTITY = re.compile(r"shader_([0-9A-F]{16})_([0-9A-F]{16})_([0-9A-F]{16})\.spv", re.IGNORECASE)
GUEST = re.compile(r"shader_([0-9A-F]{16})(?:_([0-9A-F]{16}))?\.spv", re.IGNORECASE)


def discover(directory: Path) -> list[dict]:
    entries: dict[str, dict] = {}
    frames = set()
    with (directory / "draws.jsonl").open(encoding="utf-8") as stream:
        for index, line in enumerate(stream, 1):
            draw = json.loads(line)
            frames.add(draw["frame"])
            if len(frames) > 1:
                raise ValueError("discovery must contain exactly one frame")
            for stage in ("vertex", "pixel"):
                name = draw[stage]
                if not name:
                    continue
                match = IDENTITY.fullmatch(name)
                if not match or not (directory / name).is_file():
                    raise ValueError(f"draw {index}: missing or invalid shader identity: {name}")
                entry = entries.setdefault(name, {
                    "file": name, "guest_hash": match[1].upper(),
                    "modification": match[2].upper(), "translated_hash": match[3].upper(),
                    "stages": [], "draws": [], "hud_draws": [], "surfaces": [], "textures": 0,
                })
                if stage not in entry["stages"]:
                    entry["stages"].append(stage)
                entry["draws"].append(index)
                if draw.get("hud"):
                    entry["hud_draws"].append(index)
                entry["textures"] = max(entry["textures"], len(draw.get("textures", [])))
                for surface in draw.get("surfaces", []):
                    if surface not in entry["surfaces"]:
                        entry["surfaces"].append(surface)
    if not entries:
        raise ValueError("no shaders captured; choose a frame that issues draws")
    return sorted(entries.values(), key=lambda entry: entry["file"])


def check_mod(entries: list[dict], shaders: Path) -> list[dict]:
    current = {entry["file"].upper() for entry in entries}
    result = []
    for path in sorted(shaders.glob("*.spv")):
        exact, guest = IDENTITY.fullmatch(path.name), GUEST.fullmatch(path.name)
        if exact:
            same_guest = [entry for entry in entries
                          if (entry["guest_hash"], entry["modification"])
                          == (exact[1].upper(), exact[2].upper())]
            status = ("matches the frame" if path.name.upper() in current
                      else "stale translation" if same_guest else "not seen in this frame")
            result.append({"file": path.name, "status": status})
        elif guest:
            matches = [entry["file"] for entry in entries
                       if entry["guest_hash"] == guest[1].upper()
                       and (not guest[2] or entry["modification"] == guest[2].upper())]
            status = (f"guest name for {len(matches)} translation(s) in this frame"
                      if matches else "not seen in this frame")
            result.append({"file": path.name, "status": status, "translations": matches})
        else:
            result.append({"file": path.name, "status": "invalid filename"})
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", type=Path, help="folder with draws.jsonl and .spv files")
    parser.add_argument("--shader", default="", help="guest hash or file name substring")
    parser.add_argument("--surface", default="", help="packed surface key")
    parser.add_argument("--check-mod", type=Path, help="a mod's shaders/ folder to compare")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        entries = discover(args.directory)
        checks = check_mod(entries, args.check_mod) if args.check_mod else []
        entries = [entry for entry in entries if args.shader.upper() in entry["file"].upper()
                   and (not args.surface or any(surface["key"] == args.surface.upper()
                                               for surface in entry["surfaces"]))]
        if args.json:
            print(json.dumps({"shaders": entries, "mod_files": checks}, indent=2))
            return
        for entry in entries:
            print(f'{entry["file"]}  {"/".join(entry["stages"])}  '
                  f'{len(entry["draws"])} draws ({len(entry["hud_draws"])} HUD), '
                  f'up to {entry["textures"]} textures')
            for surface in entry["surfaces"]:
                kind = "depth" if surface["depth"] else "color"
                print(f'  {surface["key"]} slot {surface["slot"]} {kind} '
                      f'{surface["width"]}x{surface["height"]} '
                      f'format {surface["format"]} samples {surface["samples"]}')
        for check in checks:
            print(f'{check["file"]}: {check["status"]}')
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"shader discovery: {error}\n")


if __name__ == "__main__":
    main()
