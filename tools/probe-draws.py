"""Find what each guest pixel shader draws, by replay (docs/GRAPHICS_GATES.md).

Every pixel shader in a shader-discovery draw log that writes a colour target
is replaced in turn with a one-colour shader (it fits every translation: one
output, set 1 binding 0 only), the frame dump is replayed, and the share of
pixels that changed, their bounding box and a thumbnail of the frame are
written to the output folder. Thumbnails are game frames: keep them under
.local/.

    python tools/probe-draws.py <discovery>/draws.jsonl <dump> --out .local/ab/probe \
        [--hash A,B] [--color 0,0,0,0] [--glslang <path>]

The base replay is made first if <out>/base/output.png is missing. Results go
to <out>/probe.json and are resumed on a rerun. About 25 s a shader with warm
pipeline caches; it stops if the game is running.
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

PROBE_SOURCE = """#version 460
layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;
layout(location = 0) out vec4 xe_out_fragment_data_0;
void main() { xe_out_fragment_data_0 = vec4(COLOR) * xe_system.color_exp_bias.x; }
"""


def game_running() -> bool:
    if sys.platform != "win32":
        return False
    tasks = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    return "lavender_shift.exe" in tasks


def colour_hashes(draws: Path) -> list[str]:
    order, seen = [], set()
    for line in draws.open():
        row = json.loads(line)
        shader = row["draw_pixel"]
        if shader in seen or shader == "0" * 16 or row.get("hud"):
            continue
        if not any(not s["depth"] for s in row["surfaces"]):
            continue
        seen.add(shader)
        order.append(shader)
    return order


def replay(dump: Path, out: Path, label: str, extra: list[str]) -> Path | None:
    command = [sys.executable, "-I", str(ROOT / "tools" / "frame-replay.py"), "replay", str(dump),
               "--out", str(out), "--label", label, "--timeout", "900", *extra]
    subprocess.run(command, capture_output=True, text=True, cwd=ROOT)
    image = out / label / "output.png"
    return image if image.exists() else None


def main() -> int:
    from PIL import Image, ImageChops

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("draws", type=Path)
    parser.add_argument("dump", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--hash", default="", help="comma-separated shaders (default: all)")
    parser.add_argument("--color", default="1,0,1,1")
    parser.add_argument("--glslang", default="glslangValidator")
    args = parser.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    probe = out / "probe.spv"
    source = out / "probe.frag"
    source.write_text(PROBE_SOURCE)
    subprocess.run([args.glslang, "-V", "-S", "frag", f"-DCOLOR=vec4({args.color})", str(source),
                    "-o", str(probe)], check=True, capture_output=True)

    base_image = out / "base" / "output.png"
    if not base_image.exists():
        if game_running() or not replay(args.dump, out, "base", []):
            print("no base replay", file=sys.stderr)
            return 1
    base = Image.open(base_image).convert("RGB")
    pixels = base.size[0] * base.size[1]

    hashes = [h.strip().upper() for h in args.hash.split(",") if h.strip()] or colour_hashes(args.draws)
    results_path = out / "probe.json"
    results = json.loads(results_path.read_text()) if results_path.exists() else {}
    thumbs = out / "thumbs"
    thumbs.mkdir(exist_ok=True)
    for shader in hashes:
        if shader in results:
            continue
        if game_running():
            print("the game is running; stopping", file=sys.stderr)
            break
        folder = out / "shaders"
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir()
        shutil.copy(probe, folder / f"shader_{shader}.spv")
        image = replay(args.dump, out, "probe", ["--shaders", str(folder)])
        if not image:
            results[shader] = {"error": "replay failed"}
            continue
        frame = Image.open(image).convert("RGB")
        mask = ImageChops.difference(base, frame).convert("L").point(lambda v: 255 if v > 8 else 0)
        changed = mask.histogram()[255] / pixels
        results[shader] = {"changed": round(changed, 5), "box": mask.getbbox()}
        frame.resize((320, 180)).save(thumbs / f"{shader}.png")
        image.unlink()
        results_path.write_text(json.dumps(results, indent=1))
        print(f"{shader} {changed:.4f} {results[shader]['box']}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
