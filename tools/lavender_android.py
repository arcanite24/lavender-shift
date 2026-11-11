"""Lavender Shift for Android, driven from the developer's PC (docs/ANDROID.md).

After Pinyon Shift's tools/pinyon_android.py. The package holds the game
translated from the player's own disc, so, like lavender_shift.exe, it is
built on their PC and installed on their own device; it is never published.
The extracted game files travel from the PC to the device over adb.

  lavender_android.py doctor [--install --accept-licenses]
                                            check (and fetch) what a build needs
  lavender_android.py build                 cross-compile and package the APK
  lavender_android.py package               package already built libraries
  lavender_android.py shell --output FILE   the prebuilt APK shell a release attaches
  lavender_android.py install               adb install -r the package
  lavender_android.py push-data [--device-root DIR]
                                            copy the extracted game to the device
  lavender_android.py push-driver ZIP [--name NAME]
                                            an adrenotools driver into user/drivers
  lavender_android.py run [-- args]         start the game (args are game flags)
  lavender_android.py stop                  stop the game

A release pins a prebuilt APK shell (ShiftGlue built for Android, the
adrenotools hooks, and the app's linked resources and classes.dex), as
Pinyon Shift's TP-5 does. With it the PC compiles only the game's code
(libmain.so and the facades) and needs just the NDK, which doctor --install
downloads from Google with platform-tools (adb): no JDK, no SDK manager.
Without a shell (a checkout) it needs the Android SDK's build-tools and
platform pinned in config/android-toolchain.json and JDK 17 or newer too.
Either way the game must be built on this PC first (tools/build.ps1), whose
translated code and generator it reuses. Every command takes --serial for
adb.
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
import time
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

# The launcher's Python is the embeddable build, whose path holds only its
# own folder: the script's folder is added for apk_sign.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import apk_sign  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
WINDOWS = os.name == "nt"
CONFIG = json.loads((ROOT / "config" / "android-toolchain.json").read_text(encoding="utf-8"))
PACKAGE = CONFIG["package"]
ACTIVITY = CONFIG["activity"]
WORK = ROOT / ".local" / "android"
DOWNLOADS = ROOT / ".local" / "downloads"
# Where doctor --install puts the NDK and platform-tools when no Android SDK
# is installed.
LOCAL_SDK = ROOT / ".local" / "toolchain" / "android-sdk"
# The release's prebuilt APK shell, unpacked (install_shell).
SHELL = WORK / "shell"
# As the SDK compiles inside the game's Android build (android-base preset,
# cmake/rexglue.cmake): the shell's libraries must match the game's headers.
SHELL_FLAGS = "-ffp-contract=off -march=armv8.2-a -DREX_PHYSICAL_HEAP_4K_PAGES=1"
SHELL_LDFLAGS = "-Wl,-z,max-page-size=16384 -Wl,-Bsymbolic-functions"
# The runtime libraries the shell carries; libmain.so and the facades are
# built on the PC.
SHELL_LIBRARIES = ("librexruntime.so", "librexgpu-fh1.so")
# The SDK files the game compiles against, as tools/build-shiftglue-kit.ps1
# mirrors them for Windows.
KIT_SOURCES = (("thirdparty/crypto", "sha256.*"), ("thirdparty/imgui", "*.h"),
               ("thirdparty/renderdoc", "*.h"), ("thirdparty/tomlplusplus/include", "**/*"),
               (".", "LICENSE*"))
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
# The Mesa Turnip builds the package bundles (config/android-toolchain.json
# drivers, notices in android/drivers/<name>). The
# activity recommends turnip-gen8-v37 on Adreno 7xx and 8xx.
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
    candidates += [str(home / "Library" / "Android" / "sdk"), str(home / "Android" / "Sdk"),
                   str(LOCAL_SDK)]
    for candidate in candidates:
        if candidate and (Path(candidate) / "platform-tools").is_dir():
            return Path(candidate)
    # None yet: doctor --install puts the NDK and platform-tools here.
    return LOCAL_SDK


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

        self.sdkmanager = self.sdk / "cmdline-tools" / "latest" / "bin" / _script("sdkmanager")

    def missing(self, shell: bool = False) -> list[str]:
        """What a build still needs; with the prebuilt shell only the NDK,
        CMake and Ninja (and adb to install)."""
        required = {
            "NDK " + CONFIG["android_sdk"]["ndk"]: self.ndk / "build" / "cmake" / "android.toolchain.cmake",
            "platform-tools (adb)": self.adb,
            "CMake": self.cmake,
            "Ninja": self.ninja,
        }
        if not shell:
            required.update({
                "build-tools " + CONFIG["android_sdk"]["build_tools"]: self.aapt2,
                CONFIG["android_sdk"]["platform"]: self.android_jar,
                f"JDK {CONFIG['jdk_major']} or newer (javac)": self.javac,
            })
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
    """The release's version: the launcher project's in a checkout. A
    release payload holds no launcher sources, so the launcher passes its
    own version in LAVENDER_SHIFT_VERSION."""
    project = ROOT / "launcher" / "LavenderShift.Launcher" / "LavenderShift.Launcher.csproj"
    if project.is_file():
        text = project.read_text(encoding="utf-8")
        name = text.split("<Version>", 1)[1].split("</Version>", 1)[0].strip()
    elif os.environ.get("LAVENDER_SHIFT_VERSION"):
        name = os.environ["LAVENDER_SHIFT_VERSION"]
    else:
        raise AndroidError("no release version (run from the launcher, or set "
                           "LAVENDER_SHIFT_VERSION)")
    major, minor, patch = (int(part) for part in name.split("-")[0].split(".")[:3])
    return name, major * 10000 + minor * 100 + patch


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def download(entry: dict, label: str) -> Path:
    """A pinned download into .local/downloads, checked by SHA-256. A file
    already there with the right hash is reused; a url may be a local path
    (a release asset staged for testing)."""
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    url = entry["url"]
    name = url.replace("\\", "/").rsplit("/", 1)[1].replace("%2B", "+")
    target = DOWNLOADS / name
    want = entry["sha256"].upper()
    if target.is_file() and _sha256(target) == want:
        return target
    local = Path(url)
    if "://" not in url and local.is_file():
        shutil.copy2(local, target)
    else:
        partial = target.with_name(target.name + ".partial")
        request = urllib.request.Request(url, headers={"User-Agent": "lavender-shift-setup"})
        for attempt in range(1, 4):
            try:
                with urllib.request.urlopen(request, timeout=60) as response, \
                        partial.open("wb") as stream:
                    total = int(response.headers.get("Content-Length") or 0)
                    done, last = 0, 0.0
                    while block := response.read(1 << 20):
                        stream.write(block)
                        done += len(block)
                        if total and time.monotonic() - last > 5:
                            last = time.monotonic()
                            print(f"Downloading {label}: {done >> 20} of {total >> 20} MB",
                                  flush=True)
                break
            except OSError as error:
                if attempt == 3:
                    raise AndroidError(f"downloading {label} from {url} failed ({error}); "
                                       "try again, or another network") from error
                print(f"downloading {label} failed ({error}); retrying", flush=True)
                time.sleep(5 * attempt)
        partial.replace(target)
    if _sha256(target) != want:
        target.unlink()
        raise AndroidError(f"{label} from {url} does not match its pinned SHA-256")
    return target


def unpack(archive: Path, install: Path) -> None:
    """Unpacks into install, dropping the archive's single top folder."""
    staging = install.with_name(install.name + ".staging")
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    if archive.name.endswith(".zip"):
        prefix = "\\\\?\\" if WINDOWS else ""  # NDK paths pass 260 characters
        with zipfile.ZipFile(archive) as zipped:
            zipped.extractall(prefix + str(staging.resolve()))
    else:
        import tarfile
        with tarfile.open(archive) as tarred:
            tarred.extractall(staging, filter="data")
    entries = list(staging.iterdir())
    source = entries[0] if len(entries) == 1 and entries[0].is_dir() else staging
    if install.exists():
        shutil.rmtree(install)
    install.parent.mkdir(parents=True, exist_ok=True)
    source.rename(install)
    if staging.exists():
        shutil.rmtree(staging)


def direct_downloads() -> dict | None:
    """The pinned NDK and platform-tools archives for this host, the ones
    sdkmanager would fetch from the same place."""
    host = "windows" if WINDOWS else ("linux" if platform.system() == "Linux" else None)
    direct = CONFIG.get("direct", {})
    if host and all(host in direct.get(name, {}) for name in ("ndk", "platform_tools")):
        return {name: direct[name][host] for name in ("ndk", "platform_tools")}
    return None


def install_direct(tools: Tools, args: argparse.Namespace) -> None:
    if not args.accept_licenses:
        raise AndroidError("the Android SDK license was not accepted: run "
                           "lavender_android.py doctor --install --accept-licenses")
    targets = {"ndk": (tools.ndk, tools.ndk / "build" / "cmake" / "android.toolchain.cmake"),
               "platform_tools": (tools.sdk / "platform-tools", tools.adb)}
    for name, entry in direct_downloads().items():
        install, have = targets[name]
        if have.is_file():
            continue
        label = "the Android NDK" if name == "ndk" else "Android platform-tools"
        print(f"Downloading {label} ({entry['bytes'] // 1_000_000} MB)", flush=True)
        archive = download(entry, label)
        print(f"Unpacking {label}", flush=True)
        unpack(archive, install)
        archive.unlink()


def sdkmanager_install(tools: Tools, args: argparse.Namespace) -> None:
    """The source build's packages (build-tools, the platform) with the SDK's
    own sdkmanager, which needs a JDK."""
    if not tools.sdkmanager.is_file():
        raise AndroidError("the Android SDK's command-line tools are needed to install "
                           + ", ".join(CONFIG["android_sdk"]["packages"]))
    answers = "y\n" * 16 if args.accept_licenses else None
    completed = subprocess.run([str(tools.sdkmanager), f"--sdk_root={tools.sdk}", "--install",
                                *CONFIG["android_sdk"]["packages"]], input=answers, text=True)
    if completed.returncode:
        raise AndroidError(f"sdkmanager failed ({completed.returncode})")


def doctor(args: argparse.Namespace) -> int:
    tools = Tools()
    # The shell holds no Android SDK files (the C++ runtime comes from the
    # NDK), so it is fetched before any license is accepted.
    shell = install_shell()
    missing = tools.missing(shell=shell is not None)
    if missing and args.install:
        if direct_downloads():
            install_direct(tools, args)
            tools = Tools()
            missing = tools.missing(shell=shell is not None)
        if missing and not shell:
            sdkmanager_install(tools, args)
            tools = Tools()
            missing = tools.missing()
    for line in (f"sdk: {tools.sdk}", f"ndk: {tools.ndk}", f"shell: {shell}",
                 f"javac: {tools.javac}", f"cmake: {tools.cmake}", f"ninja: {tools.ninja}"):
        print(line)
    if missing:
        print("missing: " + ", ".join(missing), file=sys.stderr)
        return 1
    print("ready")
    return 0


def _toolchain() -> dict:
    return json.loads((ROOT / "config" / "release-toolchain.json").read_text(encoding="utf-8"))


def shell_ready() -> Path | None:
    """The unpacked shell when it was built from the pinned ShiftGlue, the
    pinned NDK and this release's version (its manifest carries the
    version)."""
    if os.environ.get("LAVENDER_SHIFT_SOURCE_SDK") == "1":
        return None
    try:
        kit = json.loads((SHELL / "kit.json").read_text(encoding="utf-8"))
        pinned = _toolchain()["rexglue"]["revision"]
    except (OSError, ValueError, KeyError):
        return None
    if kit.get("platform") != "android-arm64" or kit.get("shiftglue_revision") != pinned \
            or kit.get("ndk") != CONFIG["android_sdk"]["ndk"] \
            or kit.get("version") != list(_version()):
        return None
    return SHELL


def install_shell() -> Path | None:
    """The release's shell (tools/apply-prebuilt-pins.py pins it), unpacked
    into .local/android/shell; None builds everything from source."""
    if shell_ready():
        return SHELL
    try:
        config = _toolchain()
        entry = config.get("android_shell", {}).get("android-arm64")
        if not entry or entry.get("revision") != config["rexglue"]["revision"]:
            return None
        print("Downloading the prebuilt Android shell", flush=True)
        archive = download(entry, "the Android shell")
        unpack(archive, SHELL)
        archive.unlink()
    except Exception as error:  # noqa: BLE001 - any failure falls back to source
        print(f"warning: the prebuilt Android shell could not be installed ({error}); "
              "building everything from source")
        return None
    return shell_ready()


def host_generator() -> Path | None:
    """The generator the PC build uses: the kit's or the source tree's."""
    suffix = ".exe" if WINDOWS else ""
    kit = ROOT / ".local" / "shiftglue-kit" / "bin" / f"rexglue{suffix}"
    if kit.is_file():
        return kit
    host = "win" if WINDOWS else ("mac" if platform.system() == "Darwin" else "linux")
    arch = "amd64" if platform.machine().lower() in ("amd64", "x86_64") else "arm64"
    source = ROOT / "thirdparty" / "shiftglue-sdk" / "out" / f"{host}-{arch}" / "Release" \
        / f"rexglue{suffix}"
    return source if source.is_file() else None


def _cached(directory: Path, name: str) -> str | None:
    try:
        for line in (directory / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
            if line.startswith(name + ":"):
                return line.split("=", 1)[1]
    except OSError:
        pass
    return None


def configured(directory: Path) -> bool:
    """A configure that failed or was stopped leaves CMakeCache.txt without
    build.ninja, and every later build would fail to load it."""
    return (directory / "CMakeCache.txt").is_file() and (directory / "build.ninja").is_file()


def build(args: argparse.Namespace) -> int:
    tools = Tools()
    shell = install_shell()
    missing = tools.missing(shell=shell is not None)
    if missing:
        raise AndroidError("missing " + ", ".join(missing) + " (lavender_android.py doctor --install)")
    if not (ROOT / ".local" / "generated" / "default" / "sources.cmake").is_file():
        raise AndroidError("the generated game code is missing; build the game on this PC "
                           "first (the launcher or tools/build.ps1)")
    generator = host_generator()
    if not generator:
        raise AndroidError("the PC's ReXGlue generator is missing; build the game on this PC "
                           "first (the launcher or tools/build.ps1)")
    environment = dict(os.environ, ANDROID_NDK_HOME=str(tools.ndk))
    directory = build_directory()
    if shell:
        sdk = ["-DREXSDK_DIR=", f"-DLAVENDER_SHIFT_SHIFTGLUE_KIT={shell.as_posix()}"]
    else:
        sdk = [f"-DREXSDK_DIR={(ROOT / 'thirdparty' / 'shiftglue-sdk').as_posix()}",
               "-DLAVENDER_SHIFT_SHIFTGLUE_KIT="]
    # A folder configured against the other SDK, or with another NDK (the
    # cache keeps its first toolchain file), is configured again from a new
    # cache.
    toolchain = (tools.ndk / "build" / "cmake" / "android.toolchain.cmake").as_posix()
    stale = configured(directory) and (
        (_cached(directory, "LAVENDER_SHIFT_SHIFTGLUE_KIT") or "") != (shell.as_posix() if shell else "")
        or Path(_cached(directory, "CMAKE_TOOLCHAIN_FILE") or toolchain) != Path(toolchain))
    if stale:
        (directory / "CMakeCache.txt").unlink()
    if stale or not configured(directory):
        run([tools.cmake, "--preset", CONFIG["configure_preset"], *sdk,
             f"-DLAVENDER_SHIFT_HOST_CODEGEN={generator.as_posix()}",
             f"-DCMAKE_MAKE_PROGRAM={tools.ninja}", f"-DPython3_EXECUTABLE={tools.python}",
             f"-DPYTHON_EXECUTABLE={tools.python}"], cwd=ROOT, env=environment)
    targets = ["lavender_shift", "lavender_shift_SpeechFacade_default",
               "lavender_shift_XMediaFacade_default"]
    if not shell:
        # libadrenotools' hooks, which nothing links; the shell carries them.
        targets += [Path(name).stem.removeprefix("lib") for name in ADRENOTOOLS_HOOKS]
    command = [tools.cmake, "--build", directory, "--target", *targets]
    if args.jobs:
        command += ["-j", str(args.jobs)]
    run(command, cwd=ROOT, env=environment)
    return package(args, tools)


def _git(*command: str, cwd: Path = ROOT) -> str:
    return subprocess.run(["git", *command], cwd=cwd, capture_output=True, text=True,
                          check=True).stdout.strip()


def build_shell(args: argparse.Namespace) -> int:
    """The prebuilt APK shell the release workflow attaches: ShiftGlue built
    for Android and installed as a kit with libadrenotools' hooks, and the
    app's linked manifest and resources and classes.dex. Needs the full
    toolchain, as a source build does. Nothing game-derived."""
    tools = Tools()
    missing = tools.missing()
    if missing:
        raise AndroidError("missing " + ", ".join(missing))
    sdk = ROOT / "thirdparty" / "shiftglue-sdk"
    revision = _git("rev-parse", "HEAD", cwd=sdk)
    pinned = _toolchain()["rexglue"]["revision"]
    if revision != pinned:
        raise AndroidError(f"ShiftGlue is at {revision}, but the config pins {pinned}")
    work = WORK / "shell-build"
    tree, prefix = work / "build", work / "shell"
    if prefix.exists():
        shutil.rmtree(prefix)
    environment = dict(os.environ, ANDROID_NDK_HOME=str(tools.ndk), LDFLAGS=SHELL_LDFLAGS)
    flags = f"{SHELL_FLAGS} -ffile-prefix-map={sdk.as_posix()}=shiftglue"
    toolchain = (tools.ndk / "build" / "cmake" / "android.toolchain.cmake").as_posix()
    jobs = ["-j", str(args.jobs)] if args.jobs else []
    run([tools.cmake, "-S", sdk, "-B", tree, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={tools.ninja}",
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
         f"-DANDROID_ABI={CONFIG['abi']}", f"-DANDROID_PLATFORM=android-{CONFIG['min_sdk']}",
         "-DANDROID_STL=c++_shared", "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384",
         f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}", "-DREXGLUE_ARM64_MARCH=armv8.2-a",
         "-DREXGLUE_USE_VULKAN=ON", "-DREXGLUE_ENABLE_TRACY=OFF",
         "-DREXGLUE_INSTALL_REGISTER_PACKAGE=OFF", f"-DPYTHON_EXECUTABLE={tools.python}",
         # Outputs stay in this tree, not the checkout's shared out/android-arm64.
         f"-DREXGLUE_OUTPUT_DIRECTORY={(tree / 'out').as_posix()}",
         f"-DCMAKE_INSTALL_PREFIX={prefix.as_posix()}"], cwd=ROOT, env=environment)
    run([tools.cmake, "--build", tree, "--target", "install", *jobs], cwd=ROOT, env=environment)
    hooks = [Path(name).stem.removeprefix("lib") for name in ADRENOTOOLS_HOOKS]
    run([tools.cmake, "--build", tree, "--target", *hooks, *jobs], cwd=ROOT, env=environment)
    for name in ADRENOTOOLS_HOOKS:
        found = [path for path in tree.glob(f"**/{name}") if "CMakeFiles" not in path.parts]
        if not found:
            raise AndroidError(f"{name} was not built in {tree}")
        shutil.copy2(found[0], prefix / "lib" / name)
    # The generator in the kit is an Android program; the PC uses its own
    # (host_generator), so it goes, with the exported targets' check for it.
    shutil.rmtree(prefix / "bin", ignore_errors=True)
    for targets in (prefix / "lib" / "cmake" / "rexglue").glob("rexglueTargets-*.cmake"):
        lines = targets.read_text(encoding="utf-8").splitlines(keepends=True)
        targets.write_text("".join(line for line in lines if not (
            line.startswith("list(APPEND _cmake_import_check")
            and "rex::rexglue " in line)), encoding="utf-8")
    for folder, pattern in KIT_SOURCES:
        for path in sorted((sdk / folder).glob(pattern)):
            if path.is_file():
                target = prefix / "share" / "rexglue" / "source" / path.relative_to(sdk)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
    # The app's code and resources for this release's version.
    staging = work / "java"
    if staging.exists():
        shutil.rmtree(staging)
    version_name, version_code = _version()
    base, dex = _java_parts(tools, staging, version_name, version_code)
    (prefix / "android").mkdir()
    shutil.copy2(base, prefix / "android" / "base.apk")
    shutil.copy2(dex, prefix / "android" / "classes.dex")
    kit = {"schema_version": 1, "platform": "android-arm64", "shiftglue_revision": revision,
           "configuration": "Release", "flags": SHELL_FLAGS, "ndk": CONFIG["android_sdk"]["ndk"],
           "min_sdk": CONFIG["min_sdk"], "version": [version_name, version_code]}
    (prefix / "kit.json").write_text(json.dumps(kit, indent=2) + "\n", encoding="utf-8")
    run([tools.python, ROOT / "tools" / "repack-toolchain.py", "kit", "--kind", "apk-shell",
         "--top", "android-shell", "--source", prefix, "--output", args.output.resolve()],
        cwd=ROOT)
    return 0


def bundled_driver_files() -> list[tuple[Path, str]]:
    """The bundled drivers (config/android-toolchain.json drivers): the
    adrenotools package from its author's release, pinned by SHA-256 and
    kept unpacked in .local/android/drivers/<name>, with its notice from
    android/drivers/<name>."""
    files = []
    for name in BUNDLED_DRIVERS:
        binary = WORK / "drivers" / name
        found = sorted(binary.glob("*")) if binary.is_dir() else []
        entry = CONFIG.get("drivers", {}).get(name)
        if not any(path.suffix == ".so" for path in found) and entry:
            try:
                archive = download(entry, f"the {name} driver")
                unpack(archive, binary)
                archive.unlink()
                found = sorted(binary.glob("*"))
            except AndroidError as error:
                print(f"warning: {error}")
        if not any(path.suffix == ".so" for path in found):
            print(f"warning: bundled driver {name} is missing from {binary}; not packaged")
            continue
        notice = ROOT / "android" / "drivers" / name
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
    # Built against the shell (the configured kit), the runtime and hooks
    # are the shell's, never a stale copy a source build left behind.
    kit = _cached(directory, "LAVENDER_SHIFT_SHIFTGLUE_KIT") or ""
    shell = SHELL if kit and Path(kit) == SHELL and shell_ready() else None
    libraries = []
    # The SDK's own libraries land in its output folder (out/android-arm64).
    places = [directory, ROOT / "thirdparty" / "shiftglue-sdk" / "out" / "android-arm64"]
    for name in NATIVE_LIBRARIES + ADRENOTOOLS_HOOKS:
        prebuilt = shell and (name in SHELL_LIBRARIES or name in ADRENOTOOLS_HOOKS)
        found = [path for place in ([shell / "lib"] if prebuilt else places)
                 for path in place.glob(f"**/{name}") if "CMakeFiles" not in path.parts]
        if not found:
            raise AndroidError(f"{name} is not built in {directory}")
        libraries.append(max(found, key=lambda path: path.stat().st_mtime))
    libraries.append(tools.libcxx)

    staging = WORK / "package"
    if staging.exists():
        shutil.rmtree(staging)
    (staging / "lib").mkdir(parents=True)
    version_name, version_code = _version()
    if shell:
        unsigned = staging / "unsigned.apk"
        shutil.copy2(shell / "android" / "base.apk", unsigned)
        dex = shell / "android" / "classes.dex"
    else:
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

    # A release payload is no repository and a player's PC may have no Git:
    # the package then names its version alone.
    try:
        commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                capture_output=True, text=True).stdout.strip()
    except OSError:
        commit = ""
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
    command = sub.add_parser("doctor")
    command.add_argument("--install", action="store_true",
                         help="download the NDK and platform-tools that are missing")
    command.add_argument("--accept-licenses", action="store_true",
                         help="the player accepted the Android SDK license")
    command.set_defaults(handler=doctor)
    command = sub.add_parser("build")
    command.add_argument("--jobs", type=int)
    command.set_defaults(handler=build)
    sub.add_parser("package").set_defaults(handler=package)
    command = sub.add_parser("shell")
    command.add_argument("--output", type=Path, required=True)
    command.add_argument("--jobs", type=int)
    command.set_defaults(handler=build_shell)
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
