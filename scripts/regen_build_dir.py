#!/usr/bin/env python3
"""Regenerate the cmake build directory. Works on both Linux/macOS and Windows.

The ex_actor framework comes from the third_party/ex-actor git submodule, so it must be checked out
before configuring: run `git submodule update --init --recursive` once after cloning.
"""

import argparse
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path


def force_remove_readonly(func, path, exc_info):
    """Handle read-only files (e.g. git pack files) on Windows."""
    os.chmod(path, stat.S_IWRITE)
    func(path)


def default_generator():
    if platform.system() == "Windows":
        if shutil.which("cl") and shutil.which("ninja"):
            return "Ninja Multi-Config"
        return "Visual Studio 17 2022"
    # Ninja is not installed everywhere (macOS/Homebrew users often don't have it); fall back to
    # Makefiles so the documented one-command flow works with just CMake + a compiler.
    if shutil.which("ninja"):
        return "Ninja Multi-Config"
    return "Unix Makefiles"


parser = argparse.ArgumentParser(
    description="Regenerate the cmake build directory.",
    allow_abbrev=False,
)
parser.add_argument(
    "-G", dest="generator", default=default_generator(),
    help="CMake generator (default: %(default)s)",
)
parser.add_argument(
    "--compiler", dest="compiler", default=None,
    help="Compiler to use, e.g. 'gcc-11' or 'clang-18'. "
         "Sets CMAKE_C_COMPILER and CMAKE_CXX_COMPILER accordingly.",
)
parser.add_argument(
    "--cxx_flags", dest="cxx_flags", default=None,
    help="Value passed to -DCMAKE_CXX_FLAGS.",
)
parser.add_argument(
    "--release", dest="release", action="store_true",
    help="Configure the Release build in build-release/ instead of the default build/.",
)
parser.add_argument(
    "--incremental", dest="incremental", action="store_true",
    help="Reuse the existing build directory instead of deleting it first, and keep the "
         "configure cache. Much faster; use it after adding/removing a source file or editing a "
         "CMakeLists.txt. It cannot pick up a changed generator or compiler — for that, run "
         "the default (delete-and-recreate) flow.",
)
args, extra_cmake_args = parser.parse_known_args()


def resolve_compiler(compiler):
    """Return (c_compiler, cxx_compiler) for a given --compiler value."""
    match = re.fullmatch(r"(gcc|clang)(-.+)?", compiler)
    if not match:
        raise ValueError(
            f"Unsupported --compiler value: {compiler!r}. "
            "Expected something like 'gcc-11' or 'clang-18'."
        )
    family, suffix = match.group(1), match.group(2) or ""
    cxx_family = "g++" if family == "gcc" else "clang++"
    return f"{family}{suffix}", f"{cxx_family}{suffix}"


project_root = Path(__file__).resolve().parent.parent
os.chdir(project_root)

# The editor database is post-processed by a shared script: the build refreshes it too, because a
# `cmake --build` that picks up a new file re-globs and reconfigures without ever reaching this
# file, and the root compile_commands.json used to go stale exactly then (clangd then guesses the
# flags of a neighbouring TU and reports headers of unrelated targets as missing).
sys.path.insert(0, str(project_root / "scripts"))
import clangd_compile_commands  # noqa: E402 - needs the repo root on sys.path

third_party = project_root / "third_party" / "ex-actor"
if not (third_party / "CMakeLists.txt").exists():
    sys.exit(
        "third_party/ex-actor is empty: run "
        "`git submodule update --init --recursive` first."
    )

# Clean previous build artifacts
# Two build directories, kept apart because they are used for different things: build/ is the
# day-to-day tree (also what --config-less debug runs use), build-release/ is the Release tree that
# game/start_server.sh --release and the load tests pick up.
#
# --incremental skips the wipe: CMake reuses the existing cache, so dependency detection and the
# configured toolchain are not redone (a warm configure is ~5s instead of minutes). The one thing
# it must not clobber is the generator, which CMake refuses to change in place — the cache records
# what created the tree, and a mismatch falls through to the clean path below.
build_dir_name = "build-release" if args.release else "build"
build_dir = project_root / build_dir_name
compile_commands = project_root / "compile_commands.json"

def cached_generator(build_path):
    """Generator recorded in an existing build tree, or None if it is not usable."""
    cache = build_path / "CMakeCache.txt"
    if not cache.exists():
        return None
    match = re.search(r"^CMAKE_GENERATOR:INTERNAL=(.+)$", cache.read_text(), re.MULTILINE)
    return match.group(1).strip() if match else None

reuse_build_dir = False
if args.incremental:
    existing_generator = cached_generator(build_dir)
    if existing_generator == args.generator:
        reuse_build_dir = True
    elif existing_generator is not None:
        print(
            f"{build_dir_name}/ was configured with '{existing_generator}', "
            f"not '{args.generator}': recreating it.",
            flush=True,
        )

if reuse_build_dir:
    print(f"Reusing {build_dir_name}/ (incremental configure).", flush=True)
    # The editor database is deliberately left in place: refresh() below replaces it atomically, and
    # if the configure fails, clangd keeps working with the previous one instead of losing it.
elif build_dir.exists():
    shutil.rmtree(build_dir, onerror=force_remove_readonly)
    compile_commands.unlink(missing_ok=True)

# Set in the environment (not only -D) because third_party/ex-actor includes its own CPM.cmake,
# which resolves the download location from $CPM_SOURCE_CACHE. A shared cache is what keeps the
# dependency sources out of <build>/_deps and out of every reconfigure's network path.
os.environ["CPM_SOURCE_CACHE"] = str(Path.home() / ".cache" / "CPM")

cmake_args = [
    "cmake", "-S", ".", "-B", build_dir_name,
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=1",
    "-G", args.generator,
]

if args.release:
    # Single-config generators (Unix Makefiles) need CMAKE_BUILD_TYPE; multi-config Ignore it.
    cmake_args += ["-DCMAKE_BUILD_TYPE=Release"]

if not args.generator.startswith("Visual Studio") and shutil.which("ccache"):
    cmake_args += [
        "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
    ]

if args.compiler:
    c_compiler, cxx_compiler = resolve_compiler(args.compiler)
    cmake_args += [
        f"-DCMAKE_C_COMPILER={c_compiler}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
    ]

if args.cxx_flags is not None:
    cmake_args += [f"-DCMAKE_CXX_FLAGS={args.cxx_flags}"]

cmake_args += extra_cmake_args

print("+", " ".join(cmake_args), flush=True)
subprocess.run(cmake_args, check=True)

# Generate the editor database at the project root (headers included, unparseable flags removed).
# It is a processed copy, never a plain copy of build/compile_commands.json — see the module
# docstring for why clangd needs the header entries.
#
# Only the default build/ tree owns that file, matching MAHJONG_UPDATE_CLANGD_DB in CMakeLists.txt:
# a Release configure must not overwrite the flags the editor is using.
if args.release:
    print(
        "Left the root compile_commands.json alone (it belongs to build/).",
        flush=True,
    )
else:
    clangd_compile_commands.refresh(build_dir, compile_commands)
