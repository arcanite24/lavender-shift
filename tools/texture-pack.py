#!/usr/bin/env python3
"""Build texture packs from texture dumps (issue #5 SR-13), with no game data
in the pack's source.

DUMP TEXTURES (settings, MODS) or --texture_dump_dir writes each texture the
game loads, once, as <hash>.dds in the dumped format: the guest base level
(and its mips) untiled. A pack is a mod whose textures/ folder holds
replacements under the same names, in the same format, at the same size or
2x, 4x or 8x it, with a full mip chain. ShiftGlue also reads an aliases.txt
there ("<hash> <hash or file>" per line) so several hashes can share one
image.

    texture-pack.py census <dump dir>                 formats, sizes, counts
    texture-pack.py export <dump dir> <png dir> [--hash H ...]
                                                      dumps to RGBA PNGs to edit
    texture-pack.py build --dump <dump dir> --images <png dir> --out <mod dir>
                          [--name NAME]               PNGs (<hash>.png) to a pack

build checks each PNG's name against the dump and its size (1x, 2x, 4x or
8x the dump's), makes the mip chain with a box filter and encodes it to the
dumped format (DXT1, DXT3, DXT5, BC4, BC5 or 8_8_8_8) with a simple encoder.
It writes <out>/textures/<hash>.dds and a mod.toml if there is none. The
encoder is plain Python, so a 4,096 x 4,096 texture takes minutes; a
texture artist's own BCn encoder gives better quality, and build --raw-dds
accepts its DDS files instead of PNGs (the size and format are still
checked). Dumps and images made from them hold game data: keep them in
.local/ or your user folder, never in the repository (docs/LEGAL.md).
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path

DDS_MAGIC = 0x20534444
DX10 = 0x30315844

# name: (fourccs, DXGI formats, block side, block bytes)
FORMATS = {
    "DXT1": ((0x31545844,), (71, 72), 4, 8),
    "DXT3": ((0x33545844,), (74, 75), 4, 16),
    "DXT5": ((0x35545844,), (77, 78), 4, 16),
    "BC4": ((0x31495441, 0x55344342), (80,), 4, 8),
    "BC5": ((0x32495441, 0x55354342), (83,), 4, 16),
    "RGBA8": ((), (28, 29), 1, 4),
}


class Texture:
    def __init__(self, kind: str, width: int, height: int, levels: list[bytes]):
        self.kind, self.width, self.height, self.levels = kind, width, height, levels


def level_bytes(kind: str, width: int, height: int, level: int) -> int:
    side, block = FORMATS[kind][2], FORMATS[kind][3]
    w, h = max(width >> level, 1), max(height >> level, 1)
    return ((w + side - 1) // side) * ((h + side - 1) // side) * block


def read_dds(path: Path) -> Texture:
    data = path.read_bytes()
    if len(data) < 128:
        raise ValueError(f"{path.name}: not a DDS")
    words = struct.unpack("<32I", data[:128])
    if words[0] != DDS_MAGIC or words[1] != 124:
        raise ValueError(f"{path.name}: not a DDS")
    height, width, levels = words[3], words[4], max(words[7], 1)
    offset, kind = 128, None
    if words[20] & 0x4 and words[21] == DX10:
        dxgi = struct.unpack("<I", data[128:132])[0]
        kind = next((k for k, v in FORMATS.items() if dxgi in v[1]), None)
        offset = 148
    elif words[20] & 0x4:
        kind = next((k for k, v in FORMATS.items() if words[21] in v[0]), None)
    elif words[20] & 0x40 and words[22] == 32 and words[23:26] == (0xFF, 0xFF00, 0xFF0000):
        kind = "RGBA8"
    if not kind:
        raise ValueError(f"{path.name}: unsupported DDS format")
    out = []
    for level in range(levels):
        size = level_bytes(kind, width, height, level)
        if offset + size > len(data):
            raise ValueError(f"{path.name}: truncated at level {level}")
        out.append(data[offset:offset + size])
        offset += size
    return Texture(kind, width, height, out)


def write_dds(path: Path, texture: Texture) -> None:
    header = [0] * 32
    header[0], header[1] = DDS_MAGIC, 124
    header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
    header[3], header[4] = texture.height, texture.width
    header[5] = level_bytes(texture.kind, texture.width, texture.height, 0)
    header[7] = len(texture.levels)
    header[19] = 32
    if texture.kind == "RGBA8":
        header[20], header[22] = 0x40 | 0x1, 32
        header[23:27] = [0xFF, 0xFF00, 0xFF0000, 0xFF000000]
    else:
        header[20], header[21] = 0x4, FORMATS[texture.kind][0][0]
    header[27] = 0x1000 | (0x400000 | 0x8 if len(texture.levels) > 1 else 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<32I", *header) + b"".join(texture.levels))


# --- PNG, RGBA --------------------------------------------------------------

def _chunk(kind: bytes, body: bytes) -> bytes:
    return (struct.pack(">I", len(body)) + kind + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    stride = width * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + _chunk(b"IDAT", zlib.compress(raw, 6)) + _chunk(b"IEND", b""))


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path.name}: not a PNG")
    offset, idat, width, height, channels = 8, b"", 0, 0, 4
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour not in (2, 6) or interlace:
                raise ValueError(f"{path.name}: only 8-bit RGB or RGBA PNGs without interlacing")
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat += body
        offset += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    previous = bytearray(stride)
    out = bytearray()
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
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                row[x] = (row[x] + (left if pa <= pb and pa <= pc else up if pb <= pc
                                    else corner)) & 0xFF
        previous = row
        if channels == 4:
            out += row
        else:
            for x in range(width):
                out += row[x * 3:x * 3 + 3] + b"\xff"
    return width, height, bytes(out)


# --- BCn decode -------------------------------------------------------------

def _565(value: int) -> tuple[int, int, int]:
    r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _colour_palette(c0: int, c1: int, four: bool) -> list[tuple[int, int, int, int]]:
    a, b = _565(c0), _565(c1)
    if four:
        return [a + (255,), b + (255,),
                tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
    return [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,),
            (0, 0, 0, 0)]


def _alpha_palette(a0: int, a1: int) -> list[int]:
    if a0 > a1:
        return [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    return [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]


def _decode_alpha(block: bytes) -> list[int]:
    palette = _alpha_palette(block[0], block[1])
    bits = int.from_bytes(block[2:8], "little")
    return [palette[(bits >> (3 * i)) & 7] for i in range(16)]


def _decode_colour(block: bytes, force_four: bool) -> list[tuple[int, int, int, int]]:
    c0, c1, bits = struct.unpack("<HHI", block[:8])
    palette = _colour_palette(c0, c1, force_four or c0 > c1)
    return [palette[(bits >> (2 * i)) & 3] for i in range(16)]


def decode(kind: str, width: int, height: int, data: bytes) -> bytes:
    """Level data to RGBA bytes."""
    if kind == "RGBA8":
        return data
    out = bytearray(width * height * 4)
    block_size = FORMATS[kind][3]
    columns = (width + 3) // 4
    for index in range(len(data) // block_size):
        block = data[index * block_size:(index + 1) * block_size]
        bx, by = index % columns, index // columns
        if kind == "DXT1":
            texels = _decode_colour(block, False)
        elif kind == "DXT3":
            alpha = int.from_bytes(block[:8], "little")
            texels = [c[:3] + ((((alpha >> (4 * i)) & 15) * 17),)
                      for i, c in enumerate(_decode_colour(block[8:], True))]
        elif kind == "DXT5":
            texels = [c[:3] + (a,) for c, a in zip(_decode_colour(block[8:], True),
                                                   _decode_alpha(block[:8]))]
        elif kind == "BC4":
            texels = [(r, r, r, 255) for r in _decode_alpha(block)]
        else:  # BC5
            texels = [(r, g, 0, 255) for r, g in zip(_decode_alpha(block[:8]),
                                                    _decode_alpha(block[8:]))]
        for i, texel in enumerate(texels):
            x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
            if x < width and y < height:
                out[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes(texel)
    return bytes(out)


# --- BCn encode (bounding box with the diagonal of the covariance) ----------

def _to565(r: float, g: float, b: float) -> int:
    clamp = lambda v, top: max(0, min(top, int(round(v))))
    return (clamp(r * 31 / 255, 31) << 11) | (clamp(g * 63 / 255, 63) << 5) | clamp(b * 31 / 255, 31)


def _encode_colour(texels: list[tuple[int, ...]], allow_transparent: bool) -> bytes:
    transparent = allow_transparent and any(t[3] < 128 for t in texels)
    opaque = [t for t in texels if not transparent or t[3] >= 128] or texels
    lo = [min(t[c] for t in opaque) for c in range(3)]
    hi = [max(t[c] for t in opaque) for c in range(3)]
    mean = [sum(t[c] for t in opaque) / len(opaque) for c in range(3)]
    # Flip green and blue against red where they fall as red rises.
    for c in (1, 2):
        covariance = sum((t[0] - mean[0]) * (t[c] - mean[c]) for t in opaque)
        if covariance < 0:
            lo[c], hi[c] = hi[c], lo[c]
    c0, c1 = _to565(*hi), _to565(*lo)
    if transparent:
        if c0 > c1:
            c0, c1 = c1, c0
    elif c0 < c1:
        c0, c1 = c1, c0
    palette = _colour_palette(c0, c1, not transparent)
    choices = range(3) if transparent else range(4)
    if c0 == c1 and not transparent:
        bits = 0
    else:
        bits = 0
        for i, t in enumerate(texels):
            if transparent and t[3] < 128:
                index = 3
            else:
                index = min(choices, key=lambda k: sum((t[c] - palette[k][c]) ** 2
                                                       for c in range(3)))
            bits |= index << (2 * i)
    return struct.pack("<HHI", c0, c1, bits)


def _encode_alpha(values: list[int]) -> bytes:
    a0, a1 = max(values), min(values)
    if a0 == a1:
        return bytes([a0, a1]) + bytes(6)
    palette = _alpha_palette(a0, a1)
    bits = 0
    for i, v in enumerate(values):
        bits |= min(range(8), key=lambda k: abs(palette[k] - v)) << (3 * i)
    return bytes([a0, a1]) + bits.to_bytes(6, "little")


def encode(kind: str, width: int, height: int, rgba: bytes) -> bytes:
    if kind == "RGBA8":
        return rgba
    out = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            texels = []
            for i in range(16):
                x, y = min(bx + (i & 3), width - 1), min(by + (i >> 2), height - 1)
                texels.append(tuple(rgba[(y * width + x) * 4:(y * width + x) * 4 + 4]))
            if kind == "DXT1":
                out += _encode_colour(texels, True)
            elif kind == "DXT3":
                alpha = 0
                for i, t in enumerate(texels):
                    alpha |= ((t[3] * 15 + 127) // 255) << (4 * i)
                out += alpha.to_bytes(8, "little") + _encode_colour(texels, False)
            elif kind == "DXT5":
                out += _encode_alpha([t[3] for t in texels]) + _encode_colour(texels, False)
            elif kind == "BC4":
                out += _encode_alpha([t[0] for t in texels])
            else:
                out += _encode_alpha([t[0] for t in texels]) + _encode_alpha([t[1] for t in texels])
    return bytes(out)


def half(width: int, height: int, rgba: bytes) -> tuple[int, int, bytes]:
    w, h = max(width >> 1, 1), max(height >> 1, 1)
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            for c in range(4):
                total = 0
                for dy in (0, 1):
                    for dx in (0, 1):
                        sx, sy = min(2 * x + dx, width - 1), min(2 * y + dy, height - 1)
                        total += rgba[(sy * width + sx) * 4 + c]
                out[(y * w + x) * 4 + c] = (total + 2) // 4
    return w, h, bytes(out)


def hash_name(path: Path) -> str | None:
    stem = path.stem.upper()
    return stem if len(stem) == 16 and all(c in "0123456789ABCDEF" for c in stem) else None


# --- commands ---------------------------------------------------------------

def census(args: argparse.Namespace) -> int:
    kinds, sizes, problems = Counter(), Counter(), 0
    for path in sorted(args.dump.glob("*.dds")):
        if not hash_name(path):
            continue
        try:
            texture = read_dds(path)
        except ValueError as error:
            print(error, file=sys.stderr)
            problems += 1
            continue
        kinds[texture.kind] += 1
        sizes[(texture.kind, texture.width, texture.height, len(texture.levels))] += 1
    print(f"{sum(kinds.values())} textures, {problems} unreadable")
    for kind, count in kinds.most_common():
        print(f"  {kind}: {count}")
    print("by format, size and levels:")
    for (kind, width, height, levels), count in sorted(sizes.items(), key=lambda i: -i[1]):
        print(f"  {count:6}  {kind:5} {width}x{height}, {levels} levels")
    return 0


def export(args: argparse.Namespace) -> int:
    wanted = {h.upper() for h in args.hash or []}
    count = 0
    for path in sorted(args.dump.glob("*.dds")):
        name = hash_name(path)
        if not name or (wanted and name not in wanted):
            continue
        texture = read_dds(path)
        write_png(args.png_dir / f"{name}.png", texture.width, texture.height,
                  decode(texture.kind, texture.width, texture.height, texture.levels[0]))
        count += 1
    print(f"{count} PNG(s) written to {args.png_dir}")
    return 0


def build(args: argparse.Namespace) -> int:
    textures = args.out / "textures"
    built, failed = 0, 0
    sources = sorted(args.images.glob("*.dds" if args.raw_dds else "*.png"))
    for path in sources:
        name = hash_name(path)
        dump_path = args.dump / f"{name}.dds" if name else None
        if not name or not dump_path.is_file():
            print(f"{path.name}: no dumped texture of that name")
            failed += 1
            continue
        dumped = read_dds(dump_path)
        if args.raw_dds:
            texture = read_dds(path)
            width, height = texture.width, texture.height
        else:
            width, height, rgba = read_png(path)
        scale = next((s for s in range(4) if (dumped.width << s, dumped.height << s)
                      == (width, height)), None)
        if scale is None:
            print(f"{path.name}: {width}x{height} is not 1x, 2x, 4x or 8x the dump's "
                  f"{dumped.width}x{dumped.height}")
            failed += 1
            continue
        if args.raw_dds:
            if texture.kind != dumped.kind or len(texture.levels) < len(dumped.levels):
                print(f"{path.name}: needs {dumped.kind} with at least {len(dumped.levels)} levels")
                failed += 1
                continue
        else:
            levels = []
            w, h, pixels = width, height, rgba
            while True:
                levels.append(encode(dumped.kind, w, h, pixels))
                if w == 1 and h == 1:
                    break
                w, h, pixels = half(w, h, pixels)
            texture = Texture(dumped.kind, width, height, levels)
        write_dds(textures / f"{name}.dds", texture)
        print(f"{name}: {dumped.kind} {width}x{height} ({1 << scale}x), {len(texture.levels)} levels")
        built += 1
    manifest = args.out / "mod.toml"
    if built and not manifest.exists():
        manifest.write_text(f'name = "{args.name or args.out.name}"\nversion = "1"\n'
                            "shares_save = true\n", encoding="utf-8")
    print(f"{built} built, {failed} skipped")
    return 0 if built and not failed else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("census")
    command.add_argument("dump", type=Path)
    command = commands.add_parser("export")
    command.add_argument("dump", type=Path)
    command.add_argument("png_dir", type=Path)
    command.add_argument("--hash", action="append")
    command = commands.add_parser("build")
    command.add_argument("--dump", type=Path, required=True)
    command.add_argument("--images", type=Path, required=True)
    command.add_argument("--out", type=Path, required=True)
    command.add_argument("--name")
    command.add_argument("--raw-dds", action="store_true",
                         help="take finished <hash>.dds files instead of PNGs")
    args = parser.parse_args(argv)
    try:
        return {"census": census, "export": export, "build": build}[args.command](args)
    except (OSError, ValueError) as error:
        print(f"texture-pack: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
