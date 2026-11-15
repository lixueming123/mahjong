#!/usr/bin/env python3
"""Run clang-format on files changed against HEAD. Works on both Linux/macOS and Windows.

The ex_actor framework used to live in this repository (include/ + src/ + test/) and is now a git
submodule under third_party/ex-actor, which is not ours to format, so it is filtered out.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

project_root = Path(__file__).resolve().parent.parent
os.chdir(project_root)

# Baseline is the committed tree: formatting covers whatever is changed in the working tree or
# staged, which is exactly the code about to be reviewed/committed.
BASELINE = "HEAD"

clang_format = shutil.which("clang-format-20") or shutil.which("clang-format")
git_clang_format = shutil.which("git-clang-format-20") or shutil.which("git-clang-format")

if git_clang_format and clang_format:
    try:
        changed_files = subprocess.check_output(
            ["git", "diff", "--name-only", BASELINE],
            text=True
        ).splitlines()
    except subprocess.CalledProcessError:
        changed_files = []

    filtered_files = [
        f for f in changed_files
        if not f.replace(os.sep, "/").startswith("third_party/")
    ]

    if filtered_files:
        subprocess.run(
            [git_clang_format, "--binary", clang_format, "-f", BASELINE, "--"] + filtered_files,
        )
    else:
        print("No changed files outside third_party/, nothing to format.")
else:
    print("WARNING: git-clang-format or clang-format not found, skipping", file=sys.stderr)
