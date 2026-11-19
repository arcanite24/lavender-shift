#!/usr/bin/env python3
"""Replay a recorded frame offline, and compare two replays (SR-8).

A frame dump comes from a play session with
`--fh1_frame_dump_frame=<frame> --fh1_frame_dump_path=<file>`; dumps hold
guest memory, so they stay in .local/ (docs/LEGAL.md). A replay starts the
game with the title suspended (`--fh1_frame_replay`), restores the dump's
registers, shaders and memory, executes its packets, writes the front buffer
next to the dump and exits. It is one frame on the GPU before the presenter:
temporal effects, upscalers and HDR still need play.

    frame-replay.py replay <dump> --out <dir> [--label A] [--shaders <dir>]
                           [--set cvar=value ...]
    frame-replay.py ab <dump> --out <dir> [--a SPEC] --b SPEC
    frame-replay.py compare <image> <image> [--diff <png>]
    frame-replay.py decode <dump>.replay.bin <png>

`ab` replays the same dump twice and compares them. A SPEC is a list of
`shaders=<dir>` (a shader override folder) and `cvar=value` items separated
by commas; the default A is the game as configured. Each replay writes:

- front.png: the front buffer the game resolved (its own image, before
  ShiftGlue's swap effects), decoded from <dump>.replay.bin;
- output.png: the presenter's guest output (after swap effects such as AO,
  FXAA and the colour LUT, before upscaling and host UI);
- replay.json and replay.log.

`compare` gives the PSNR (dB; identical images give inf), the largest
channel difference and the number of differing pixels, and can write a
difference image (differences amplified 8x). Replays use a fresh user
folder each time, so the player's settings and saves are neither read nor
written.

Ported from Pinyon Shift's replay-fh1-frame.py and
compare-native-renderer-images.py (BSD-3-Clause, same authors).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Xenos TextureFormat values the front buffer may have.
FORMAT_8_8_8_8 = 6
FORMAT_2_10_10_10 = 7
FORMAT_2_10_10_10_AS_16_16_16_16 = 54


# --- images -----------------------------------------------------------------

class Image:
    def __init__(self, width: int, height: int, rgb: bytes):
        if len(rgb) != width * height * 3:
            raise ValueError("image size does not match its pixels")
        self.width, self.height, self.rgb = width, height, rgb


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def write_png(path: Path, image: Image) -> None:
    stride = image.width * 3
    raw = b"".join(b"\x00" + image.rgb[y * stride:(y + 1) * stride] for y in range(image.height))
    header = struct.pack(">IIBBBBB", image.width, image.height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", header)
                     + _png_chunk(b"IDAT", zlib.compress(raw, 6)) + _png_chunk(b"IEND", b""))


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def read_png(path: Path) -> Image:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    offset, idat, width = 8, b"", 0
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour not in (2, 6) or interlace:
                raise ValueError(f"{path}: only 8-bit RGB or RGBA PNGs without interlacing")
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat += body
        offset += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    previous = bytearray(stride)
    rgb = bytearray()
    for y in range(height):
        line = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        kind, row = line[0], bytearray(line[1:])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 1:
                row[x] = (row[x] + left) & 0xFF
            elif kind == 2:
                row[x] = (row[x] + up) & 0xFF
            elif kind == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                row[x] = (row[x] + _paeth(left, up, corner)) & 0xFF
        previous = row
        if channels == 3:
            rgb += row
        else:
            for x in range(width):
                rgb += row[x * 4:x * 4 + 3]
    return Image(width, height, bytes(rgb))


def read_ppm(path: Path) -> Image:
    data = path.read_bytes()
    tokens, offset = [], 0
    while len(tokens) < 4:
        while data[offset:offset + 1].isspace() or data[offset:offset + 1] == b"#":
            if data[offset:offset + 1] == b"#":
                offset = data.index(b"\n", offset)
            offset += 1
        end = offset
        while not data[end:end + 1].isspace():
            end += 1
        tokens.append(data[offset:end])
        offset = end
    if tokens[0] != b"P6" or int(tokens[3]) != 255:
        raise ValueError(f"{path}: only 8-bit binary PPM")
    width, height = int(tokens[1]), int(tokens[2])
    offset += 1
    return Image(width, height, data[offset:offset + width * height * 3])


def read_image(path: Path) -> Image:
    return read_ppm(path) if path.suffix.lower() == ".ppm" else read_png(path)


def tiled_offset_2d(x: int, y: int, pitch: int, bytes_per_block_log2: int) -> int:
    """Xenos 2D tiling (ShiftGlue's texture_util GetTiledOffset2D), in
    blocks of 1 << bytes_per_block_log2 bytes."""
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bytes_per_block_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def decode_front_buffer(data: bytes, info: dict) -> Image:
    """The replayed front buffer as 8-bit RGB: 8888 and 2_10_10_10 formats,
    linear or tiled. The read-back words are little-endian with blue in the
    low bits, as a display's A8R8G8B8 and A2R10G10B10 are."""
    width, height, fmt = int(info["width"]), int(info["height"]), int(info["format"])
    if fmt not in (FORMAT_8_8_8_8, FORMAT_2_10_10_10, FORMAT_2_10_10_10_AS_16_16_16_16):
        raise ValueError(f"front buffer format {fmt} is not decoded")
    tiled = bool(int(info.get("tiled", 0)))
    pitch = (width + 31) & ~31
    words = struct.unpack(f"<{len(data) // 4}I", data[:len(data) // 4 * 4])
    rgb = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            index = tiled_offset_2d(x, y, pitch, 2) >> 2 if tiled else y * pitch + x
            value = words[index] if index < len(words) else 0
            if fmt == FORMAT_8_8_8_8:
                b, g, r = value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF
            else:
                b = ((value & 0x3FF) * 255 + 511) // 1023
                g = (((value >> 10) & 0x3FF) * 255 + 511) // 1023
                r = (((value >> 20) & 0x3FF) * 255 + 511) // 1023
            out = (y * width + x) * 3
            rgb[out:out + 3] = bytes((r, g, b))
    return Image(width, height, bytes(rgb))


def compare(a: Image, b: Image, diff_path: Path | None = None) -> dict:
    if (a.width, a.height) != (b.width, b.height):
        raise ValueError(f"sizes differ: {a.width}x{a.height} and {b.width}x{b.height}")
    squared, largest, differing = 0, 0, 0
    diff = bytearray(len(a.rgb)) if diff_path else None
    for pixel in range(0, len(a.rgb), 3):
        pixel_differs = False
        for c in range(3):
            d = a.rgb[pixel + c] - b.rgb[pixel + c]
            if d:
                pixel_differs = True
                squared += d * d
                largest = max(largest, abs(d))
                if diff is not None:
                    diff[pixel + c] = min(255, abs(d) * 8)
        differing += pixel_differs
    mse = squared / len(a.rgb)
    if diff_path:
        write_png(diff_path, Image(a.width, a.height, bytes(diff)))
    return {"psnr_db": math.inf if mse == 0 else 10 * math.log10(255 * 255 / mse),
            "max_channel_difference": largest, "differing_pixels": differing,
            "pixels": a.width * a.height}


# --- replays ----------------------------------------------------------------

def parse_spec(spec: str) -> tuple[list[Path], list[str]]:
    shader_dirs, cvars = [], []
    for item in filter(None, (part.strip() for part in spec.split(","))):
        key, _, value = item.partition("=")
        if not value:
            raise ValueError(f"{item!r}: expected shaders=<dir> or cvar=value")
        if key == "shaders":
            shader_dirs.append(Path(value).resolve())
        else:
            cvars.append(f"--{key}={value}")
    return shader_dirs, cvars


def replay(dump: Path, out: Path, label: str, shader_dirs: list[Path], cvars: list[str],
           build_dir: Path, timeout: int, root: Path = ROOT) -> dict:
    dump = dump.resolve()
    if not dump.is_file():
        raise ValueError(f"missing frame dump: {dump}")
    build_dir = build_dir.resolve()
    exe = build_dir / ("lavender_shift.exe" if os.name == "nt" else "lavender_shift")
    if not exe.is_file():
        raise ValueError(f"build first: {exe}")
    disc, update = root / ".local/game/disc", root / ".local/game/update"
    for folder in (disc, update):
        if not folder.is_dir():
            raise ValueError(f"run tools/lavender_setup.py first: {folder}")
    work = (out / label).resolve()
    if work.exists():
        shutil.rmtree(work)
    (work / "user").mkdir(parents=True)
    # The replay writes its results next to the dump: give it a link (or a
    # copy) of its own.
    local_dump = work / dump.name
    try:
        os.link(dump, local_dump)
    except OSError:
        shutil.copyfile(dump, local_dump)
    output_ppm = work / "output.ppm"
    arguments = [
        f"--game_data_root={disc}", f"--update_data_root={update}",
        f"--user_data_root={work / 'user'}", "--xex_apply_patches=true", "--gpu_backend=vulkan",
        "--xam_xstudio_request_unsupported=true",
        f"--fh1_frame_replay={local_dump}", f"--fh1_frame_replay_output_path={output_ppm}",
        # An asynchronous pipeline drops the draw that asked for it, which
        # would make replays differ from run to run.
        "--async_shader_compilation=false", "--gpu_record_thread=false",
    ]
    if shader_dirs:
        arguments.append("--vulkan_shader_override_dirs=" + ";".join(map(str, shader_dirs)))
    arguments += cvars
    process = subprocess.run([str(exe), *arguments], cwd=exe.parent, capture_output=True,
                             text=True, timeout=timeout, check=False)
    (work / "replay.log").write_text(process.stdout + process.stderr, encoding="utf-8")
    result_json = Path(f"{local_dump}.replay.json")
    if not result_json.is_file():
        raise RuntimeError(f"{label}: the replay wrote no result (exit {process.returncode}); "
                           f"see {work / 'replay.log'}")
    result = json.loads(result_json.read_text(encoding="utf-8"))
    result.update(label=label, exit_code=process.returncode, arguments=arguments)
    front = decode_front_buffer(Path(f"{local_dump}.replay.bin").read_bytes(), result)
    write_png(work / "front.png", front)
    if output_ppm.is_file():
        write_png(work / "output.png", read_ppm(output_ppm))
        output_ppm.unlink()
    local_dump.unlink()
    (work / "replay.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def ab(args: argparse.Namespace) -> dict:
    report = {"dump": str(args.dump), "variants": {}}
    for label, spec in (("a", args.a), ("b", args.b)):
        shader_dirs, cvars = parse_spec(spec)
        report["variants"][label] = replay(args.dump, args.out, label, shader_dirs, cvars,
                                           args.build_dir, args.timeout, args.root)
    for name in ("front", "output"):
        a, b = args.out / "a" / f"{name}.png", args.out / "b" / f"{name}.png"
        if a.is_file() and b.is_file():
            report[name] = compare(read_png(a), read_png(b), args.out / f"{name}-diff.png")
    (args.out / "ab.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("replay", "ab"):
        command = commands.add_parser(name)
        command.add_argument("dump", type=Path)
        command.add_argument("--out", type=Path, required=True)
        # Another checkout's game data and build, e.g. from a worktree.
        command.add_argument("--root", type=Path, default=ROOT,
                             help="checkout with .local/game and the build (default: this one)")
        command.add_argument("--build-dir", type=Path,
                             help="default: <root>/out/build/win-amd64-release")
        command.add_argument("--timeout", type=int, default=600)
        if name == "replay":
            command.add_argument("--label", default="replay")
            command.add_argument("--shaders", type=Path, action="append", default=[])
            command.add_argument("--set", action="append", default=[], metavar="CVAR=VALUE")
        else:
            command.add_argument("--a", default="")
            command.add_argument("--b", required=True)
    command = commands.add_parser("compare")
    command.add_argument("first", type=Path)
    command.add_argument("second", type=Path)
    command.add_argument("--diff", type=Path)
    command = commands.add_parser("decode")
    command.add_argument("replay_bin", type=Path)
    command.add_argument("png", type=Path)
    args = parser.parse_args(argv)
    if getattr(args, "root", None):
        args.root = args.root.resolve()
    if getattr(args, "root", None) and not args.build_dir:
        args.build_dir = args.root / "out/build/win-amd64-release"
    try:
        if args.command == "replay":
            cvars = [f"--{item}" for item in args.set]
            result = replay(args.dump, args.out, args.label,
                            [path.resolve() for path in args.shaders], cvars, args.build_dir,
                            args.timeout, args.root)
        elif args.command == "ab":
            result = ab(args)
        elif args.command == "compare":
            result = compare(read_image(args.first), read_image(args.second), args.diff)
        else:
            info_path = Path(str(args.replay_bin).removesuffix(".bin") + ".json")
            info = json.loads(info_path.read_text(encoding="utf-8"))
            write_png(args.png, decode_front_buffer(args.replay_bin.read_bytes(), info))
            result = {"png": str(args.png), "width": info["width"], "height": info["height"],
                      "format": info["format"], "tiled": info["tiled"]}
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        print(f"frame-replay: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
