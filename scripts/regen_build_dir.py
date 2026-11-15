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
build_dir_name = "build-release" if args.release else "build"
build_dir = project_root / build_dir_name
if build_dir.exists():
    shutil.rmtree(build_dir, onerror=force_remove_readonly)
compile_commands = project_root / "compile_commands.json"
compile_commands.unlink(missing_ok=True)

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

# Copy compile_commands.json to project root (if generated)
src = build_dir / "compile_commands.json"
if src.exists():
    shutil.copy2(src, compile_commands)

    # Remove args not supported by clangd
    with open(compile_commands, "r") as f:
        text = f.read()
    text = re.sub(r"-fconcepts-diagnostics-depth=\d*", "", text)
    with open(compile_commands, "w") as f:
        f.write(text)
