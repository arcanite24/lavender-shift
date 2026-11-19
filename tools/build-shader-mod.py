#!/usr/bin/env python3
"""Build a shader mod folder from GLSL sources (SR-1, SR-2 and later).

A shader mod source folder holds a `shaders.toml` like

    name = "bloom-kernels"
    version = "1"

    [[shader]]
    guest = "D6652B8E4866F16B"   # the guest pixel shader it replaces
    source = "src/bloom_half.frag"
    defines = ["AXIS=0"]

and this compiles each entry with glslangValidator to
`<out>/shaders/shader_<guest>.spv` (a guest-named override, SR-9) and
writes `<out>/mod.toml`. Copy or link `<out>` into `<user>/mods/` and add
its name to `enabled_mods`. `--define NAME=VALUE` (repeatable) adds a
define to every shader and `--name` renames the mod, for building one
variant of a source folder (the composite's CURVE, DITHER, BLUR_TAPS).

    python tools/build-shader-mod.py docs/examples/shader-mods/bloom \
        --out .local/mods/bloom-kernels [--glslang path]
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tomllib


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--name")
    parser.add_argument("--glslang", default=shutil.which("glslangValidator") or "glslangValidator")
    args = parser.parse_args()

    spec = tomllib.loads((args.source / "shaders.toml").read_text(encoding="utf-8"))
    shaders = args.out / "shaders"
    shaders.mkdir(parents=True, exist_ok=True)
    failed = 0
    for entry in spec.get("shader", []):
        guest = entry["guest"].upper()
        stage = entry.get("stage", "frag")
        output = shaders / f"shader_{guest}.spv"
        command = [args.glslang, "-V", "-S", stage, "--target-env", "vulkan1.1"]
        defines = entry.get("defines", []) + args.define
        command += [f"-D{d}" for d in defines]
        command += [str(args.source / entry["source"]), "-o", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        status = "ok" if result.returncode == 0 else "FAILED"
        print(f"{guest}: {entry['source']} {' '.join(defines)} {status}")
        if result.returncode != 0:
            print(result.stdout + result.stderr)
            failed += 1
    (args.out / "mod.toml").write_text(
        f'name = "{args.name or spec["name"]}"\nversion = "{spec.get("version", "1")}"\nshares_save = true\n',
        encoding="utf-8",
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
