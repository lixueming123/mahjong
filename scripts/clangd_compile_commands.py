#!/usr/bin/env python3
"""Turn a CMake compile_commands.json into the one the editor actually reads.

CMake emits an entry per translation unit only, and only into the build directory. Two things are
missing from that file for clangd:

  * headers. When one is opened there is no exact match, so the flags of the *nearest* TU by path
    get inferred. That neighbour is arbitrary: actor_scope.h picked up account_test.cc, which did
    not have ex_actor/stdexec on its include path, so <exec/async_scope.hpp> looked missing even
    though the real build finds it. Headers are never compiled standalone, so reusing a TU's flags
    with -fsyntax-only is enough, and it keeps the third-party include paths exactly in sync with
    the real build (no hardcoded paths into the CPM cache, which changes per checkout).

  * a location under the workspace root, which is what .clangd / clangd.arguments point at.

Run it after any configure. scripts/regen_build_dir.py calls refresh() directly, and the build
itself runs it (see the clangd_compile_commands target in the top-level CMakeLists.txt), because a
`cmake --build` that picks up a new source file re-globs and reconfigures without going through the
regen script. Editing this file is therefore not enough: the entry point must stay importable and
must never fail the build (see main()).
"""

import argparse
import json
import os
import re
import shlex
import sys
import tempfile
from pathlib import Path

project_root = Path(__file__).resolve().parent.parent
third_party = project_root / "third_party" / "ex-actor"


def _template_entry(entries):
    """The entry whose flags are the richest, used as the base for header entries.

    The server links everything (ex_actor, stdexec, asio, capnp, spdlog), so its TU covers every
    include path a header could need. Falls back to the first entry for a stripped-down tree.
    """
    return next(
        (e for e in entries if e["file"].endswith("/game/server/main.cc")),
        entries[0],
    )


def _header_entry(template, header):
    command = template.get("command")
    if command is None:
        args = list(template["arguments"])
    else:
        args = shlex.split(command)
    # Drop the parts that name the template's own source and output: -o <obj> and -c <src>.
    # Only the exact forms are matched — a prefix test on "-c"/"-o" would also eat legitimate
    # flags like -cxx-isystem or -objc-arc.
    cleaned = []
    skip_next = False
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg in ("-o", "-c"):
            skip_next = True
            continue
        cleaned.append(arg)
    # Pin the language. Without it clangd treats an isolated .h as a C header (-x c-header) and
    # then reports every C++ standard header in it (<atomic>, <chrono>, ...) as not found; the
    # headers that do work today do so only because a sibling .cc lets clangd infer C++.
    cleaned += ["-x", "c++-header", "-fsyntax-only", header]

    entry = {"directory": template["directory"], "file": header}
    if command is None:
        entry["arguments"] = cleaned
    else:
        entry["command"] = shlex.join(cleaned)
    return entry


def add_header_entries(entries, extra_templates=()):
    """Return entries plus one -fsyntax-only entry per project and framework header."""
    if not entries:
        return entries

    templates = [_template_entry(entries)]
    known_files = {e["file"] for e in entries}
    for path in extra_templates:
        if str(path) in known_files:
            templates.append(next(e for e in entries if e["file"] == str(path)))

    headers = []
    for pattern in ("game/**/*.h", "game/**/*.hpp"):
        headers += sorted(project_root.glob(pattern))
    # Framework headers matter too: they are reached through -isystem, so an exact entry keeps
    # them from inheriting some unrelated TU's flags.
    framework_include = third_party / "include"
    for pattern in ("**/*.h", "**/*.hpp"):
        headers += sorted(framework_include.glob(pattern))

    added = []
    for header in headers:
        resolved = str(header)
        if resolved not in known_files and header.is_file():
            # Rotate over the templates so a header that only the framework compiles (or only a
            # test does) still gets plausible flags when the server TU is absent.
            added.append(_header_entry(templates[len(added) % len(templates)], resolved))
            known_files.add(resolved)

    added += _dependency_root_entries(entries, templates[0], known_files)
    return entries + added


def _include_paths(command_args):
    """Include directories named on a command line, in the order they appear."""
    paths = []
    index = 0
    while index < len(command_args):
        arg = command_args[index]
        if arg in ("-I", "-isystem") and index + 1 < len(command_args):
            paths.append(command_args[index + 1])
            index += 2
            continue
        if arg.startswith("-isystem"):
            paths.append(arg[len("-isystem"):])
        elif arg.startswith("-I") and len(arg) > 2:
            paths.append(arg[2:])
        index += 1
    return paths


def _dependency_root_entries(entries, template, known_files):
    """One -fsyntax-only entry per dependency include root, not per dependency header.

    Jumping into a dependency's source (Ctrl+click on <exec/task.hpp>, say) used to be all red:
    that header had no entry, so clangd inferred the flags of the *nearest* TU by path — for
    stdexec that landed on spdlog's stdout_sinks.cpp, whose command has no stdexec include path,
    so every stdexec symbol was unresolved.

    Enumerating all ~1600 third-party headers would bloat the database and re-index on every
    configure. Instead each include *root* gets one entry, which is enough because clangd's
    inference walks up from the opened file to the closest entry — any file under that root now
    finds a command with the full flag set. Paths are taken from the real command line (never
    hardcoded), so they follow the CPM cache layout and the build tree automatically.
    """
    command = template.get("command")
    args = list(template["arguments"]) if command is None else shlex.split(command)

    roots = []
    for path in _include_paths(args):
        resolved = Path(path)
        # Only dependencies: the project's own headers get exhaustive entries above, and the CMake
        # build dirs hold nothing worth opening.
        if not resolved.is_dir():
            continue
        if not any(marker in path for marker in (".cache/CPM", "_deps", "third_party")):
            continue
        if path not in roots:
            roots.append(path)

    added = []
    for root in roots:
        # An existing file under the root is what makes clangd match this entry to the directory;
        # the file itself is never compiled (the entry is -fsyntax-only).
        anchor = _first_header_under(root)
        if anchor is None or anchor in known_files:
            continue
        added.append(_header_entry(template, anchor))
        known_files.add(anchor)
    return added


def _first_header_under(root):
    """Some header inside `root`, or None. Walks instead of globbing: asio alone has ~600."""
    for directory, _, filenames in os.walk(root):
        for filename in sorted(filenames):
            if filename.endswith((".h", ".hpp", ".hxx")):
                return str(Path(directory) / filename)
    return None


def _strip_unsupported_flags(text):
    """Drop args clangd does not understand (it warns on every command line otherwise)."""
    return re.sub(r"-fconcepts-diagnostics-depth=\d*", "", text)


def build_database(build_dir):
    """Post-processed database for `build_dir`, or None when CMake has not written one yet."""
    source = Path(build_dir) / "compile_commands.json"
    if not source.exists():
        return None
    entries = json.loads(source.read_text())
    return add_header_entries(entries)


def _our_translation_units(entries):
    """Entries that compile one of *our* sources (not a dependency's).

    A database with none of them is not a usable source for the editor: it was written by a
    configure that died early, or by one that had the export switched off. Overwriting the good
    root database with header entries derived from it is worse than keeping the stale one — clangd
    then reports every dependency header (<exec/task.hpp>, gtest, ...) as missing.
    """
    markers = (str(project_root / "game"), str(third_party))
    return [
        entry for entry in entries
        if entry["file"].endswith((".cc", ".cpp"))
        and any(entry["file"].startswith(marker) for marker in markers)
    ]


def refresh(build_dir, output=None):
    """Rewrite `output` (default: the workspace-root compile_commands.json) from the build tree.

    The write is atomic and skipped when nothing changed: clangd watches this file, and touching it
    on every build would make it throw away and re-parse all translation units each time.
    """
    output = Path(output) if output is not None else project_root / "compile_commands.json"
    entries = build_database(build_dir)
    if entries is None:
        return False
    if not _our_translation_units(entries):
        print(
            f"clangd_compile_commands: {build_dir}/compile_commands.json has no game/framework "
            "translation units; keeping the existing database (re-run configure with "
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=1).",
            file=sys.stderr,
        )
        return False

    text = _strip_unsupported_flags(json.dumps(entries, indent=1))
    if output.exists() and output.read_text() == text:
        return False

    # Temp file in the same directory: os.replace() is atomic, so clangd never reads a truncated
    # database (that is what makes it fall back to path inference and report dozens of fake errors).
    handle, temp_path = tempfile.mkstemp(dir=output.parent, prefix=".compile_commands.", suffix=".tmp")
    try:
        with os.fdopen(handle, "w") as f:
            f.write(text)
        os.replace(temp_path, output)
    except BaseException:
        Path(temp_path).unlink(missing_ok=True)
        raise
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Refresh the workspace-root compile_commands.json from a CMake build tree.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--build-dir", dest="build_dir", default=str(project_root / "build"),
        help="Build tree holding CMake's compile_commands.json (default: %(default)s)",
    )
    parser.add_argument(
        "--output", dest="output", default=None,
        help="File to write (default: <repo>/compile_commands.json)",
    )
    args = parser.parse_args()
    try:
        changed = refresh(args.build_dir, args.output)
    except Exception as exception:  # noqa: BLE001 - never fail a build over editor metadata
        # This runs from the build (see clangd_compile_commands in CMakeLists.txt), so a broken
        # database must degrade to a stale one, not to a failed compile.
        print(f"clangd_compile_commands: skipped ({exception})", file=sys.stderr)
        return 0
    if changed:
        print("Refreshed compile_commands.json for clangd.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
