"""Lavender Shift for Android, driven from the developer's PC (docs/ANDROID.md).

After Pinyon Shift's tools/pinyon_android.py. The package holds the game
translated from the player's own disc, so, like lavender_shift.exe, it is
built on their PC and installed on their own device; it is never published.
The extracted game files travel from the PC to the device over adb.

  lavender_android.py build                 cross-compile and package the APK
  lavender_android.py package               package already built libraries
  lavender_android.py install               adb install -r the package
  lavender_android.py push-data [--device-root DIR]
                                            copy the extracted game to the device
  lavender_android.py push-driver ZIP [--name NAME]
                                            an adrenotools driver into user/drivers
  lavender_android.py run [-- args]         start the game (args are game flags)
  lavender_android.py stop                  stop the game

It needs the Android SDK (platform-tools, build-tools, the platform and the
NDK pinned in config/android-toolchain.json) and JDK 17 or newer, and the
game built on this PC first (tools/build.ps1), whose translated code and
generator it reuses. Every command takes --serial for adb.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

import apk_sign

ROOT = Path(__file__).resolve().parents[1]
WINDOWS = os.name == "nt"
CONFIG = json.loads((ROOT / "config" / "android-toolchain.json").read_text(encoding="utf-8"))
PACKAGE = CONFIG["package"]
ACTIVITY = CONFIG["activity"]
WORK = ROOT / ".local" / "android"
NATIVE_LIBRARIES = (
    "librexruntime.so",
    "librexgpu-fh1.so",
    "liblavender_shift_SpeechFacade_default.so",
    "liblavender_shift_XMediaFacade_default.so",
    "libmain.so",
)
# libadrenotools' hooks: nothing links them; the driver loader opens them
# from the native library folder for a custom GPU driver.
ADRENOTOOLS_HOOKS = (
    "libhook_impl.so",
    "libmain_hook.so",
    "libfile_redirect_hook.so",
    "libgsl_alloc_hook.so",
)
# The Mesa Turnip builds Pinyon Shift ships (its android/drivers/<name>, with
# the notice), read from Pinyon's .local/android/drivers/<name> or this
# checkout's. The activity recommends turnip-gen8-v37 on Adreno 7xx and 8xx.
BUNDLED_DRIVERS = ["turnip-gen8-v37"]


class AndroidError(RuntimeError):
    pass


def _exe(name: str) -> str:
    return name + (".exe" if WINDOWS else "")


def _script(name: str) -> str:
    return name + (".bat" if WINDOWS else "")


def sdk_root() -> Path:
    candidates = [os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT")]
    if WINDOWS and os.environ.get("LOCALAPPDATA"):
        candidates.append(str(Path(os.environ["LOCALAPPDATA"]) / "Android" / "Sdk"))
    home = Path.home()
    candidates += [str(home / "Library" / "Android" / "sdk"), str(home / "Android" / "Sdk")]
    for candidate in candidates:
        if candidate and (Path(candidate) / "platform-tools").is_dir():
            return Path(candidate)
    raise AndroidError("no Android SDK found (set ANDROID_HOME)")


def _pinned_tool(name: str) -> Path | None:
    """CMake or Ninja from the PC build's pinned toolchain."""
    try:
        entry = json.loads((ROOT / "config" / "release-toolchain.json")
                           .read_text(encoding="utf-8"))[name]
    except (OSError, KeyError, json.JSONDecodeError):
        return None
    path = ROOT / entry["install_path"] / entry["executable"]
    return path if path.is_file() else None


class Tools:
    def __init__(self) -> None:
        self.sdk = sdk_root()
        pins = CONFIG["android_sdk"]
        self.ndk = self.sdk / "ndk" / pins["ndk"]
        build_tools = self.sdk / "build-tools" / pins["build_tools"]
        self.android_jar = self.sdk / "platforms" / pins["platform"] / "android.jar"
        self.adb = self.sdk / "platform-tools" / _exe("adb")
        self.aapt2 = build_tools / _exe("aapt2")
        self.d8 = build_tools / _script("d8")
        java_home = os.environ.get("JAVA_HOME")
        javac = Path(java_home) / "bin" / _exe("javac") if java_home else None
        self.javac = javac if javac and javac.is_file() else shutil.which("javac")
        host = "windows-x86_64" if WINDOWS else f"{platform.system().lower()}-x86_64"
        prebuilt = self.ndk / "toolchains" / "llvm" / "prebuilt" / host
        self.llvm_bin = prebuilt / "bin"
        self.strip = self.llvm_bin / _exe("llvm-strip")
        self.libcxx = prebuilt / "sysroot" / "usr" / "lib" / "aarch64-linux-android" / "libc++_shared.so"
        self.cmake = _pinned_tool("cmake") or shutil.which("cmake")
        self.ninja = _pinned_tool("ninja") or shutil.which("ninja")
        self.python = _pinned_tool("python") or Path(sys.executable)

    def missing(self) -> list[str]:
        required = {
            "NDK " + CONFIG["android_sdk"]["ndk"]: self.ndk / "build" / "cmake" / "android.toolchain.cmake",
            "build-tools " + CONFIG["android_sdk"]["build_tools"]: self.aapt2,
            CONFIG["android_sdk"]["platform"]: self.android_jar,
            "platform-tools (adb)": self.adb,
            f"JDK {CONFIG['jdk_major']} or newer (javac)": self.javac,
            "CMake": self.cmake,
            "Ninja": self.ninja,
        }
        return [name for name, path in required.items() if not path or not Path(path).exists()]


def run(command: list, **kwargs) -> subprocess.CompletedProcess:
    completed = subprocess.run([str(part) for part in command], **kwargs)
    if completed.returncode:
        raise AndroidError(f"{Path(str(command[0])).name} failed ({completed.returncode})")
    return completed


def adb(tools: Tools, args: argparse.Namespace, *command, **kwargs):
    prefix = [tools.adb] + (["-s", args.serial] if args.serial else [])
    return run(prefix + list(command), **kwargs)


def adb_output(tools: Tools, args: argparse.Namespace, command: str) -> str:
    prefix = [str(tools.adb)] + (["-s", args.serial] if args.serial else [])
    return subprocess.run(prefix + ["shell", command], capture_output=True, text=True).stdout.strip()


def device_root(tools: Tools, args: argparse.Namespace) -> str:
    """The app's external files folder the game goes in: --device-root, else
    the first one (internal storage, then SD cards) that already holds the
    game, else internal storage. The activity looks in the same order."""
    if getattr(args, "device_root", None):
        return args.device_root.rstrip("/")
    roots = [f"/sdcard/Android/data/{PACKAGE}/files"] + [
        f"/storage/{volume}/Android/data/{PACKAGE}/files"
        for volume in adb_output(tools, args, "ls /storage").split()
        if volume not in ("emulated", "self")]
    for root in roots:
        if adb_output(tools, args, f"[ -f '{root}/game/disc/default.xex' ] && echo yes") == "yes":
            return root
    return roots[0]


def share_with_app(tools: Tools, args: argparse.Namespace, *remote: str) -> None:
    """What adb pushes into the app's folder belongs to the shell user; the
    app's folder is closed to other apps, so opening these to everyone
    reaches the app alone."""
    for path in remote:
        adb(tools, args, "shell", f"find '{path}' -user shell -exec chmod a+rwX {{}} +")


def build_directory() -> Path:
    return ROOT / "out" / "build" / CONFIG["configure_preset"]


def _version() -> tuple[str, int]:
    text = (ROOT / "launcher" / "LavenderShift.Launcher" / "LavenderShift.Launcher.csproj") \
        .read_text(encoding="utf-8")
    name = text.split("<Version>", 1)[1].split("</Version>", 1)[0].strip()
    major, minor, patch = (int(part) for part in name.split("-")[0].split(".")[:3])
    return name, major * 10000 + minor * 100 + patch


def build(args: argparse.Namespace) -> int:
    tools = Tools()
    missing = tools.missing()
    if missing:
        raise AndroidError("missing " + ", ".join(missing))
    if not (ROOT / ".local" / "generated" / "default" / "sources.cmake").is_file():
        raise AndroidError("the generated game code is missing; build the game on this PC "
                           "first (tools/build.ps1)")
    environment = dict(os.environ, ANDROID_NDK_HOME=str(tools.ndk))
    directory = build_directory()
    if not (directory / "CMakeCache.txt").is_file():
        run([tools.cmake, "--preset", CONFIG["configure_preset"],
             f"-DCMAKE_MAKE_PROGRAM={tools.ninja}", f"-DPython3_EXECUTABLE={tools.python}",
             f"-DPYTHON_EXECUTABLE={tools.python}"], cwd=ROOT, env=environment)
    targets = ["lavender_shift", "lavender_shift_SpeechFacade_default",
               "lavender_shift_XMediaFacade_default"]
    targets += [Path(name).stem.removeprefix("lib") for name in ADRENOTOOLS_HOOKS]
    command = [tools.cmake, "--build", directory, "--target", *targets]
    if args.jobs:
        command += ["-j", str(args.jobs)]
    run(command, cwd=ROOT, env=environment)
    return package(args, tools)


def bundled_driver_files() -> list[tuple[Path, str]]:
    files = []
    pinyon = ROOT.parent / "horizon1-recomp"
    for name in BUNDLED_DRIVERS:
        for binary in (WORK / "drivers" / name, pinyon / ".local" / "android" / "drivers" / name):
            found = sorted(binary.glob("*")) if binary.is_dir() else []
            if any(path.suffix == ".so" for path in found):
                break
        else:
            print(f"warning: bundled driver {name} is missing from {WORK / 'drivers' / name}; "
                  "not packaged")
            continue
        notice = pinyon / "android" / "drivers" / name
        for path in found + sorted(notice.glob("*")):
            if path.is_file():
                files.append((path, f"assets/drivers/{name}/{path.name}"))
    return files


def _java_parts(tools: Tools, staging: Path, version_name: str, version_code: int) -> tuple[Path, Path]:
    """The linked manifest and resources (an APK without code) and
    classes.dex, made with aapt2, javac and d8."""
    (staging / "classes").mkdir(parents=True, exist_ok=True)
    (staging / "dex").mkdir(exist_ok=True)
    manifest = (ROOT / "android" / "AndroidManifest.xml").read_text(encoding="utf-8")
    manifest = manifest.replace("<manifest ", f'<manifest package="{PACKAGE}" ', 1)
    (staging / "AndroidManifest.xml").write_text(manifest, encoding="utf-8")
    unsigned = staging / "unsigned.apk"
    resources = staging / "resources.zip"
    run([tools.aapt2, "compile", "--dir", ROOT / "android" / "res", "-o", resources])
    run([tools.aapt2, "link", "-o", unsigned, "-I", tools.android_jar,
         "--manifest", staging / "AndroidManifest.xml", resources,
         "--min-sdk-version", str(CONFIG["min_sdk"]),
         "--target-sdk-version", str(CONFIG["target_sdk"]),
         "--version-name", version_name, "--version-code", str(version_code)])
    sdl_java = ROOT / "thirdparty" / "shiftglue-sdk" / "thirdparty" / "sdl3" / "android-project" \
        / "app" / "src" / "main" / "java"
    sources = sorted(sdl_java.rglob("*.java")) + sorted((ROOT / "android" / "java").rglob("*.java"))
    run([tools.javac, "-source", "1.8", "-target", "1.8", "-nowarn", "-Xlint:-options",
         "-bootclasspath", tools.android_jar, "-classpath", tools.android_jar,
         "-encoding", "UTF-8", "-d", staging / "classes", *sources])
    # One jar rather than a path per class: d8 is a .bat on Windows, and
    # cmd.exe refuses a command line longer than 8191 characters.
    classes = staging / "classes.jar"
    with zipfile.ZipFile(classes, "w", zipfile.ZIP_DEFLATED) as jar:
        for path in sorted((staging / "classes").rglob("*.class")):
            jar.write(path, path.relative_to(staging / "classes").as_posix())
    run([tools.d8, "--release", "--min-api", str(CONFIG["min_sdk"]), "--lib", tools.android_jar,
         "--output", staging / "dex", classes])
    return unsigned, staging / "dex" / "classes.dex"


def package(args: argparse.Namespace, tools: Tools | None = None) -> int:
    tools = tools or Tools()
    directory = build_directory()
    libraries = []
    # The SDK's own libraries land in its output folder (out/android-arm64).
    places = [directory, ROOT / "thirdparty" / "shiftglue-sdk" / "out" / "android-arm64"]
    for name in NATIVE_LIBRARIES + ADRENOTOOLS_HOOKS:
        found = [path for place in places for path in place.glob(f"**/{name}")
                 if "CMakeFiles" not in path.parts]
        if not found:
            raise AndroidError(f"{name} is not built in {directory}")
        libraries.append(max(found, key=lambda path: path.stat().st_mtime))
    libraries.append(tools.libcxx)

    staging = WORK / "package"
    if staging.exists():
        shutil.rmtree(staging)
    (staging / "lib").mkdir(parents=True)
    version_name, version_code = _version()
    unsigned, dex = _java_parts(tools, staging, version_name, version_code)

    # The device gets libraries without debug sections; the PC keeps the
    # originals for symbolizing crash reports.
    stripped = []
    for library in libraries:
        target = staging / "lib" / library.name
        run([tools.strip, "--strip-debug", "-o", target, library])
        stripped.append(target)
    # Android 15 devices with 16 KiB pages refuse libraries whose load
    # segments are aligned to less.
    readelf = tools.llvm_bin / _exe("llvm-readelf")
    for library in stripped:
        segments = subprocess.run([str(readelf), "-lW", str(library)], capture_output=True,
                                  text=True).stdout.splitlines()
        aligns = {int(line.split()[-1], 16) for line in segments if line.strip().startswith("LOAD")}
        if not aligns or min(aligns) < 0x4000:
            raise AndroidError(f"{library.name} has load segments aligned below 16 KiB")

    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True,
                            text=True).stdout.strip()
    (staging / "lavender_shift_build.json").write_text(json.dumps({
        "schema_version": 1, "version": version_name, "commit": commit,
        "built_utc": datetime.now(timezone.utc).isoformat()}, indent=2) + "\n", encoding="utf-8")
    with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_DEFLATED,
                         strict_timestamps=False) as apk:
        apk.write(dex, "classes.dex")
        apk.write(staging / "lavender_shift_build.json", "assets/lavender_shift_build.json")
        for library in stripped:
            apk.write(library, f"lib/{CONFIG['abi']}/{library.name}")
        for path, name in bundled_driver_files():
            apk.write(path, name)

    output = WORK / "lavender-shift.apk"
    signing = WORK / "signing"
    key, certificate = apk_sign.signing_key(signing)
    output.write_bytes(apk_sign.sign(unsigned.read_bytes(), key, certificate))
    print(json.dumps({"apk": str(output), "bytes": output.stat().st_size, "version": version_name,
                      "libraries": [path.name for path in stripped]}))
    return 0


def install(args: argparse.Namespace) -> int:
    tools = Tools()
    apk = WORK / "lavender-shift.apk"
    if not apk.is_file():
        raise AndroidError("build the package first (lavender_android.py build)")
    adb(tools, args, "install", "-r", apk)
    return 0


def push_data(args: argparse.Namespace) -> int:
    """The extracted disc and title update, pushed with --sync so an
    interrupted copy resumes and an unchanged file is not sent again, and
    the PC's ahead-of-time pipelines (tools/prepare_shaders.ps1)."""
    tools = Tools()
    game = ROOT / ".local" / "game"
    if not (game / "disc" / "default.xex").is_file():
        raise AndroidError(f"the extracted game is not at {game / 'disc'}")
    remote = device_root(tools, args)
    adb(tools, args, "shell", "mkdir", "-p", f"{remote}/game/disc", f"{remote}/game/update",
        f"{remote}/user")
    for folder in ("disc", "update"):
        adb(tools, args, "push", "--sync", f"{game / folder}{os.sep}.", f"{remote}/game/{folder}")
    pipelines = (args.user_root or ROOT / ".local" / "user") / "cache" / "shaders" / "shareable" \
        / "4D530AA4.fbo.aot.xpso"
    if pipelines.is_file():
        shareable = f"{remote}/user/cache/shaders/shareable"
        adb(tools, args, "shell", "mkdir", "-p", shareable)
        adb(tools, args, "push", pipelines, f"{shareable}/{pipelines.name}", stdout=subprocess.DEVNULL)
    share_with_app(tools, args, f"{remote}/game", f"{remote}/user")
    print(f"game files in {remote}/game")
    return 0


def push_driver(args: argparse.Namespace) -> int:
    """A custom Vulkan driver packaged for adrenotools (a .zip with meta.json
    and the .so, or a folder of them) into user/drivers/<name>; the game
    loads it with --android_gpu_driver=<name>."""
    tools = Tools()
    source = args.driver.resolve()
    name = args.name or source.stem.lower()
    staging = WORK / "driver-staging" / name
    if staging.exists():
        shutil.rmtree(staging)
    if source.is_dir():
        shutil.copytree(source, staging)
    else:
        with zipfile.ZipFile(source) as archive:
            archive.extractall(staging)
    if not any(staging.glob("*.so")):
        raise AndroidError(f"{source} holds no driver library (.so)")
    root = device_root(tools, args)
    remote = f"{root}/user/drivers/{name}"
    adb(tools, args, "shell", "rm", "-rf", remote)
    adb(tools, args, "shell", "mkdir", "-p", remote)
    adb(tools, args, "push", f"{staging}{os.sep}.", remote, stdout=subprocess.DEVNULL)
    share_with_app(tools, args, f"{root}/user/drivers")
    print(f"driver {name}: run with --android_gpu_driver={name}")
    return 0


def run_game(args: argparse.Namespace) -> int:
    tools = Tools()
    extras: list[str] = []
    if args.game_arguments:
        extras += ["--esa", "args", ",".join(args.game_arguments)]
    adb(tools, args, "shell", "am", "start", "-S", "-W", "-n", f"{PACKAGE}/{ACTIVITY}", *extras,
        stdout=subprocess.DEVNULL)
    return 0


def stop(args: argparse.Namespace) -> int:
    adb(Tools(), args, "shell", "am", "force-stop", PACKAGE)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", help="adb device serial")
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("build")
    command.add_argument("--jobs", type=int)
    command.set_defaults(handler=build)
    sub.add_parser("package").set_defaults(handler=package)
    sub.add_parser("install").set_defaults(handler=install)
    command = sub.add_parser("push-data")
    command.add_argument("--device-root", help="the app's files folder on the device")
    command.add_argument("--user-root", type=Path, help="PC user folder with the AOT pipelines")
    command.set_defaults(handler=push_data)
    command = sub.add_parser("push-driver")
    command.add_argument("driver", type=Path)
    command.add_argument("--name")
    command.add_argument("--device-root")
    command.set_defaults(handler=push_driver)
    command = sub.add_parser("run")
    command.add_argument("game_arguments", nargs="*")
    command.set_defaults(handler=run_game)
    sub.add_parser("stop").set_defaults(handler=stop)
    args = parser.parse_args()
    try:
        return args.handler(args)
    except AndroidError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
