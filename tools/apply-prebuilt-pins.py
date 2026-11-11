"""Pin a release's prebuilt assets in the launcher payload's toolchain config.

RL-10 (docs/RELEASE_BACKLOG.md, after Pinyon Shift's TP-1 to TP-3): the
release workflow builds the trimmed LLVM and the ShiftGlue kit, attaches
them to the release, and tools/package-release.ps1 rewrites the payload's
config/release-toolchain.json with this script so setup downloads them from
that release by SHA-256. The checked-in config keeps the upstream downloads
and no kit, so a developer checkout builds from source.

Each asset has the sidecar <asset>.json that tools/repack-toolchain.py
writes beside it: {"kind": "llvm" | "kit", "platform": "windows-x64",
"version": ... (LLVM) or "revision": ... (kit)}.

    python tools/apply-prebuilt-pins.py --config payload/config/release-toolchain.json \
        --assets .artifacts/prebuilt \
        --base-url https://github.com/<owner>/lavender-shift/releases/download/v0.1.0
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

PLATFORMS = ("windows-x64",)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def apply(config: dict, assets: Path, base_url: str) -> list[str]:
    changes = []
    revision = config["rexglue"]["revision"]
    for sidecar in sorted(assets.glob("*.json")):
        asset = sidecar.with_suffix("")
        if not asset.is_file():
            raise SystemExit(f"{sidecar.name} describes a missing asset")
        meta = json.loads(sidecar.read_text(encoding="utf-8"))
        platform = meta["platform"]
        if platform not in PLATFORMS:
            raise SystemExit(f"{sidecar.name}: unknown platform {platform}")
        url = f"{base_url.rstrip('/')}/{asset.name}"
        digest = sha256(asset)
        kind = meta["kind"]
        if kind == "llvm":
            item = config["llvm"]
            if meta.get("version") != item["version"]:
                raise SystemExit(f"{asset.name} repacks LLVM {meta.get('version')}, "
                                 f"but the config pins {item['version']}")
            item["url"] = url
            item["sha256"] = digest
            item["repacked"] = True
        elif kind == "kit":
            if meta.get("revision") != revision:
                raise SystemExit(f"{asset.name} was built from ShiftGlue {meta.get('revision')}, "
                                 f"but the config pins {revision}")
            config.setdefault("shiftglue_kit", {})[platform] = {
                "revision": revision, "url": url, "sha256": digest}
        else:
            raise SystemExit(f"{sidecar.name}: unknown kind {kind}")
        changes.append(f"{kind} {platform}: {asset.name} {digest}")
    return changes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--base-url", required=True)
    args = parser.parse_args(argv)
    config = json.loads(args.config.read_text(encoding="utf-8"))
    changes = apply(config, args.assets, args.base_url)
    args.config.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    print("\n".join(changes) if changes else "No prebuilt assets to pin.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
