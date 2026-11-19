"""Lavender Shift on a Mac with Apple silicon (docs/MACOS.md).

The macOS counterpart of tools/build.ps1, tools/prepare_shaders.ps1 and
tools/launch.ps1, after Pinyon Shift's macOS setup. The game is translated
from the player's own disc and built on their Mac; nothing here downloads
game data or publishes a build.

  python3 tools/lavender_mac.py setup --iso DISC.iso --update TU.zip
                                                     everything below, from the player's files
  python3 tools/lavender_mac.py doctor                check what a build needs
  python3 tools/lavender_mac.py build [--step STEP]   generator, codegen, game (all)
  python3 tools/lavender_mac.py shaders [--force]     the ahead-of-time pipelines
  python3 tools/lavender_mac.py run [-- game flags]   prepare the pipelines and start

setup fetches ShiftGlue at the pinned revision (config/release-toolchain.json)
when this folder is not a git checkout, extracts the disc and title update
with tools/lavender_setup.py (only the supported files), translates, builds
and prepares the pipelines; each step skips work already done, so it also
updates a folder after a newer release is extracted over it. Logs go to
.local/logs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "thirdparty" / "shiftglue-sdk"
GAME = ROOT / ".local" / "game"
LOGS = ROOT / ".local" / "logs"
MANIFEST = ROOT / "lavender_shift_manifest.toml"
GENERATED = ROOT / ".local" / "generated"
GENERATOR = SDK / "out" / "mac-arm64" / "Release" / "rexglue"
PRESET = "mac-arm64-release"
BUILD = ROOT / "out" / "build" / PRESET
EXE = BUILD / "lavender_shift"
ARCHIVE_EXTRACT = BUILD / "lavender_shift_archive_extract"
USER = ROOT / ".local" / "user"
CATALOG = ROOT / "config" / "aot-pipelines.json"
# Homebrew's tools, which a login shell over ssh may not have on PATH.
HOMEBREW_BIN = ("/opt/homebrew/bin", "/usr/local/bin")


class MacError(RuntimeError):
    pass


def environment() -> dict:
    env = dict(os.environ)
    path = env.get("PATH", "").split(os.pathsep)
    env["PATH"] = os.pathsep.join([p for p in HOMEBREW_BIN if p not in path] + path)
    return env


def tool(name: str) -> str | None:
    return shutil.which(name, path=environment()["PATH"])


def run_logged(step: str, command: list, cwd: Path = ROOT) -> None:
    LOGS.mkdir(parents=True, exist_ok=True)
    log = LOGS / f"{step}.log"
    print(f"{step}: {' '.join(str(c) for c in command)}", flush=True)
    start = time.monotonic()
    with open(log, "wb") as output:
        # caffeinate keeps the Mac awake through the long compile.
        prefix = ["caffeinate", "-i"] if tool("caffeinate") else []
        result = subprocess.run(prefix + [str(c) for c in command], cwd=cwd, env=environment(),
                                stdout=output, stderr=subprocess.STDOUT)
    seconds = time.monotonic() - start
    if result.returncode:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-20:]
        print("\n".join(tail), file=sys.stderr)
        raise MacError(f"{step} failed (exit {result.returncode}); see {log}")
    print(f"{step}: done in {seconds:.0f} s", flush=True)


def doctor_problems(require_sdk: bool = True) -> list[str]:
    problems = []
    if platform.system() != "Darwin":
        problems.append("this script is for macOS; on Windows use tools/build.ps1")
    elif platform.machine() != "arm64":
        problems.append(f"Apple silicon only (this Mac is {platform.machine()})")
    else:
        major = int(platform.mac_ver()[0].split(".")[0] or 0)
        if major and major < 13:
            problems.append(f"macOS 13.3 or newer is needed (this is {platform.mac_ver()[0]})")
    if subprocess.run(["xcode-select", "-p"], capture_output=True).returncode:
        problems.append("the Command Line Tools are missing: run xcode-select --install")
    for name in ("cmake", "ninja", "git"):
        if not tool(name):
            problems.append(f"{name} is missing: brew install {name}")
    # 3.9 is the Command Line Tools' python3.
    if sys.version_info < (3, 9):
        problems.append(f"Python 3.9 or newer is needed (this is {platform.python_version()})")
    if require_sdk and not (SDK / "CMakeLists.txt").is_file():
        problems.append("thirdparty/shiftglue-sdk is empty: python3 tools/lavender_mac.py setup")
    return problems


def doctor(_: argparse.Namespace) -> int:
    problems = doctor_problems()
    for problem in problems:
        print(f"missing: {problem}")
    for name in ("disc/default.xex", "update/default.xexp", "codegen/default.xex"):
        state = "ok" if (GAME / name).is_file() else "missing (tools/lavender_setup.py)"
        print(f".local/game/{name}: {state}")
    print(f"generator: {'built' if GENERATOR.is_file() else 'not built'}")
    print(f"game: {'built' if EXE.is_file() else 'not built'}")
    if not problems:
        print("ready to build")
    return 1 if problems else 0


def parallel() -> int:
    return max(1, os.cpu_count() or 4)


def forget_moved_cache(build_dir: Path) -> None:
    """CMake refuses a build folder configured at another path, as after the
    folder was moved or renamed: start that build over."""
    cache = build_dir / "CMakeCache.txt"
    try:
        lines = cache.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return
    recorded = next((line.split("=", 1)[1] for line in lines
                     if line.startswith("CMAKE_CACHEFILE_DIR:INTERNAL=")), None)
    if recorded is not None and Path(recorded).resolve() != build_dir.resolve():
        # Nested projects (FidelityFX's) keep caches of their own, so the
        # whole folder goes; a moved build compiles everything again anyway.
        print(f"{build_dir} was configured in {recorded}; building it again", flush=True)
        shutil.rmtree(build_dir)


def build_generator() -> None:
    forget_moved_cache(SDK / "out" / "build" / "mac-arm64")
    run_logged("generator-configure", ["cmake", "--preset", "mac-arm64"], cwd=SDK)
    run_logged("generator-build", ["cmake", "--build", "--preset", "mac-arm64-release",
                                   "--target", "rexglue", "--parallel", parallel()], cwd=SDK)


def codegen() -> None:
    if not (GAME / "codegen" / "default.xex").is_file():
        raise MacError("run python3 -I tools/lavender_setup.py codegen first")
    if not GENERATOR.is_file():
        build_generator()
    run_logged("codegen", [GENERATOR, "--log-level", "info", "--log-file",
                           LOGS / "codegen-detail.log", "codegen", MANIFEST,
                           "--xex_apply_patches=true"])
    CODEGEN_STAMP.write_text(codegen_inputs() + "\n", encoding="utf-8")


DEPFILE = GENERATED / "default" / "codegen.d"
CODEGEN_STAMP = GENERATED / "lavender-mac-inputs.sha256"


def codegen_inputs() -> str:
    """A hash of what the translation is made from: the inputs codegen.d
    lists, the manifest, every analysis file and the generator. By content,
    since lavender_setup.py codegen copies the modules again on every run."""
    files = {MANIFEST, GENERATOR, *(ROOT / "config" / "analysis").glob("*.toml")}
    if DEPFILE.is_file():
        text = DEPFILE.read_text(encoding="utf-8", errors="replace").replace("\\\n", " ")
        files.update(Path(p) for p in text.split(":", 1)[-1].split())
    digest = hashlib.sha256()
    for path in sorted(files):
        digest.update(str(path).encode())
        digest.update(sha256(path).encode() if path.is_file() else b"missing")
    return digest.hexdigest()


def codegen_current() -> bool:
    # The generator writes its inputs' absolute paths into codegen.d; a tree
    # generated on another machine (or from another folder) is regenerated,
    # and so is one whose inputs changed (a newer release over this folder).
    if not DEPFILE.is_file():
        return False
    if str(ROOT.as_posix()) not in DEPFILE.read_text(encoding="utf-8", errors="replace"):
        return False
    try:
        return CODEGEN_STAMP.read_text(encoding="utf-8").strip() == codegen_inputs()
    except OSError:
        return False


def build_game() -> None:
    if not codegen_current():
        codegen()
    forget_moved_cache(BUILD)
    run_logged("game-configure", ["cmake", "--preset", PRESET, f"-DREXSDK_DIR={SDK}",
                                  "-DLAVENDER_SHIFT_SHIFTGLUE_KIT=", "-DREXGLUE_ENABLE_TRACY=OFF",
                                  "-DSDL_HIDAPI_LIBUSB=OFF", f"-DPython3_EXECUTABLE={sys.executable}"])
    run_logged("game-build", ["cmake", "--build", "--preset", PRESET, "--parallel", parallel()])
    run_logged("archive-tool-build", ["cmake", "--build", "--preset", PRESET, "--target",
                                      "lavender_shift_archive_extract", "--parallel", parallel()])


def build(args: argparse.Namespace) -> int:
    problems = doctor_problems()
    if problems:
        for problem in problems:
            print(f"missing: {problem}", file=sys.stderr)
        return 1
    if args.step == "generator":
        build_generator()
    elif args.step == "codegen":
        codegen()
    else:
        if args.step == "all" and not GENERATOR.is_file():
            build_generator()
        build_game()
    return 0


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def prepare_shaders(user_root: Path, force: bool = False) -> None:
    """tools/prepare_shaders.ps1: every pipeline in config/aot-pipelines.json,
    rebuilt from the player's own disc, update and patched executable."""
    cache = user_root / "cache"
    output = cache / "shaders" / "shareable" / "4D530AA4.fbo.aot.xpso"
    record = cache / "aot-pipelines.json"
    key = sha256(CATALOG)
    if not force and output.is_file() and record.is_file():
        try:
            if json.loads(record.read_text(encoding="utf-8")).get("catalog") == key:
                print("AOT pipelines are current.")
                return
        except (OSError, ValueError):
            pass
    if not ARCHIVE_EXTRACT.is_file():
        print(f"AOT pipelines skipped: {ARCHIVE_EXTRACT} is not built", file=sys.stderr)
        return
    (ROOT / ".local" / "aot").mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="work-", dir=ROOT / ".local" / "aot"))
    try:
        image = work / "default.v2.bin"
        run_logged("aot-image", [ARCHIVE_EXTRACT, "--xex-image", GAME / "codegen" / "default.xex",
                                 image, "--apply-patch"])
        python = [sys.executable, "-I"]
        run_logged("aot-corpus", python + [ROOT / "tools" / "extract_shader_corpus.py",
                                           GAME / "disc", "--output", work / "corpus.json",
                                           "--binary-dir", work / "corpus",
                                           "--archive-extractor", ARCHIVE_EXTRACT,
                                           "--content-root", GAME / "update",
                                           "--executable-image", image])
        output.parent.mkdir(parents=True, exist_ok=True)
        run_logged("aot-generate", python + [ROOT / "tools" / "aot_pipelines.py", "generate",
                                             "--corpus", work / "corpus", "--catalog", CATALOG,
                                             "--output", output])
        record.write_text(json.dumps({"schema_version": 1, "catalog": key}), encoding="utf-8")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def shaders(args: argparse.Namespace) -> int:
    prepare_shaders(USER, args.force)
    return 0


def git(*args, cwd: Path = ROOT) -> str:
    result = subprocess.run(["git", *[str(a) for a in args]], cwd=cwd, env=environment(),
                            capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


def prepare_sdk() -> None:
    """tools/prepare-rexglue.ps1: the submodule in a checkout, otherwise a clone
    of the pinned revision with its submodules."""
    if (ROOT / ".git").exists():
        if not (SDK / "CMakeLists.txt").is_file():
            run_logged("rexglue-prepare", ["git", "submodule", "update", "--init", "--recursive",
                                           "--jobs", "8", "--", "thirdparty/shiftglue-sdk"])
        return
    pin = json.loads((ROOT / "config" / "release-toolchain.json").read_text(encoding="utf-8"))
    repository, revision = pin["rexglue"]["repository"], pin["rexglue"]["revision"]
    marker = SDK.parent / (SDK.name + ".prepared")
    if marker.is_file() and marker.read_text(encoding="utf-8").strip() == revision \
            and git("rev-parse", "HEAD", cwd=SDK) == revision:
        print(f"ShiftGlue {revision[:12]} is prepared.")
        return
    marker.unlink(missing_ok=True)
    if not (SDK / ".git").exists():
        if SDK.exists():
            shutil.rmtree(SDK)
        run_logged("rexglue-clone", ["git", "clone", "--no-checkout", "--no-tags", repository, SDK])
    elif git("status", "--porcelain", "--ignore-submodules=dirty", cwd=SDK):
        raise MacError(f"the ShiftGlue folder has local changes: {SDK}")
    run_logged("rexglue-fetch", ["git", "-C", SDK, "fetch", "--depth", "1", "origin", revision])
    run_logged("rexglue-checkout", ["git", "-C", SDK, "checkout", "--detach", revision])
    submodules = ["git", "-C", SDK, "submodule", "update", "--init", "--recursive", "--force"]
    try:
        run_logged("rexglue-submodules", submodules + ["--jobs", "8"])
    except MacError:
        # Parallel clones of the nested submodules have failed on a first
        # run (pack files gone mid-fetch); one at a time picks up the rest.
        print("rexglue-submodules: retrying one at a time", flush=True)
        run_logged("rexglue-submodules-retry", submodules + ["--jobs", "1"])
    if git("rev-parse", "HEAD", cwd=SDK) != revision:
        raise MacError("ShiftGlue is not at the pinned revision after the checkout")
    marker.write_text(revision + "\n", encoding="utf-8")


def extracted_from(name: str, source: Path) -> bool:
    catalog = GAME / f"{name}-catalog.json"
    try:
        return json.loads(catalog.read_text(encoding="utf-8")).get("source") == source.name
    except (OSError, ValueError):
        return False


def setup(args: argparse.Namespace) -> int:
    problems = doctor_problems(require_sdk=False)
    if problems:
        for problem in problems:
            print(f"missing: {problem}", file=sys.stderr)
        return 1
    if running():
        raise MacError("quit lavender_shift first")
    steps = [("disc", args.iso, "disc/default.xex"), ("update", args.update, "update/default.xexp")]
    for name, source, marker in steps:
        if source is None and not (GAME / marker).is_file():
            raise MacError(f"pass --{'iso' if name == 'disc' else 'update'}: the {name} is not extracted yet")
        if source is not None and not source.is_file():
            raise MacError(f"{source} does not exist")
    prepare_sdk()
    setup_script = [sys.executable, "-I", ROOT / "tools" / "lavender_setup.py"]
    for name, source, marker in steps:
        if source is not None and not ((GAME / marker).is_file() and extracted_from(name, source)):
            run_logged(f"setup-{name}", setup_script + [name, source.resolve()])
    run_logged("setup-codegen", setup_script + ["codegen"])
    build_generator()
    build_game()
    prepare_shaders(USER)
    print("Ready: python3 tools/lavender_mac.py run")
    return 0


def running() -> bool:
    # The game claims its user folder only on Windows (src/install_paths.cpp).
    return subprocess.run(["pgrep", "-x", "lavender_shift"], capture_output=True).returncode == 0


def run_game(args: argparse.Namespace) -> int:
    if not EXE.is_file():
        raise MacError("build the game first: python3 tools/lavender_mac.py build")
    if running():
        raise MacError("lavender_shift is already running")
    USER.mkdir(parents=True, exist_ok=True)
    if not args.skip_shaders:
        prepare_shaders(USER)
    flags = [a for a in args.flags if a != "--"]
    # With no --game_data_root the game uses .local/game and .local/user and
    # sets launch.ps1's flags itself (src/install_paths.cpp).
    return subprocess.run([str(EXE)] + flags, cwd=BUILD, env=environment()).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("setup", help="prepare, extract, translate and build")
    command.add_argument("--iso", type=Path, help="the Forza Horizon 2 USA disc image")
    command.add_argument("--update", type=Path, help="the title update v2 package")
    command.set_defaults(handler=setup)
    commands.add_parser("doctor").set_defaults(handler=doctor)
    command = commands.add_parser("build")
    command.add_argument("--step", choices=("generator", "codegen", "game", "all"), default="all")
    command.set_defaults(handler=build)
    command = commands.add_parser("shaders")
    command.add_argument("--force", action="store_true", help="rebuild even when current")
    command.set_defaults(handler=shaders)
    command = commands.add_parser("run")
    command.add_argument("--skip-shaders", action="store_true")
    command.add_argument("flags", nargs=argparse.REMAINDER, help="game flags after --")
    command.set_defaults(handler=run_game)
    args = parser.parse_args()
    try:
        return args.handler(args)
    except MacError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
