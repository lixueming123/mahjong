# Understanding This Project

- Mahjong game (`game/`) built on the ex_actor actor framework, which lives in the
  `third_party/ex-actor` git submodule (pinned; never modify code under `third_party/`).
- Docs: README.md and `game/docs` for the game; `third_party/ex-actor/docs/contents` for the
  framework (API, schedulers, distributed mode).
- Build&test: `git submodule update --init --recursive` once, then
  `python3 scripts/regen_build_dir.py`, `cmake --build build --config Release`, `ctest -C Release`.
  Server binary is `build/game/mahjong_server`.

# Don't Break clangd

The root `compile_commands.json` is not plain CMake output: it is a processed copy, written by
`scripts/clangd_compile_commands.py`, which adds a `-fsyntax-only` entry per header (~100) so
headers don't borrow an unrelated TU's command. Lose it and the editor floods with bogus errors.

- It refreshes itself: the `clangd_compile_commands` target runs on every `cmake --build build`
  (a no-op when nothing changed). So a plain build never leaves it stale — a new source file
  re-globs, reconfigures and repopulates the database in one step.
- Header entries carry `-x c++-header`: without it clangd treats an isolated `.h` as C and reports
  `<atomic>`/`<chrono>` as missing. Dependency *include roots* also get one anchor entry each, so
  jumping into `~/.cache/CPM/...` sources resolves with the full flag set instead of borrowing an
  unrelated TU (stdexec used to inherit spdlog's command and go all red).
- Never copy `build/compile_commands.json` over the root one: 0 header entries vs ~110.
- `build-release/` deliberately does not write it (two trees would overwrite each other's flags).
- Prefer `python3 scripts/regen_build_dir.py --incremental` over the default flow: it reuses
  `build/` (~10s) instead of deleting it. The destructive default is only for a changed generator
  or compiler; it deletes *before* configuring, so a failed configure leaves nothing behind.
- Don't `rm -rf build` or `git clean -xdf`; if you must re-create, back up the database first:
  `cp compile_commands.json /tmp/cc.bak`, and restore it if the configure fails.
- Any manual CMake call needs `-DCMAKE_EXPORT_COMPILE_COMMANDS=1`.
- Editor errors look bogus? Confirm with `clangd --check=<file> --compile-commands-dir=. 2>&1 |
  grep "Compile command"` — `inferred from` a stranger TU means the errors are fake.
- Dependency sources live in `~/.cache/CPM` (`CPM_SOURCE_CACHE` is set in the top-level
  `CMakeLists.txt`), not in `build/_deps`; that is what keeps an IDE reconfigure off the network.

# Editing Guidelines

- Keep original comments unless they are stale.

# Debugging

- Do not simplify test code when debugging unless you are very confident the test is wrong.
- Prefer adding logs over thinking too hard.

# Code Style

Google Style Guide, plus:

- Add `/*arg_name=*/` before args whose meaning isn't clear from the name.
- No one-line if/while/for/switch statements; always use `{}`.
- No std::pair: define a struct. With pairs/iterators use structured bindings
  (`auto& [key, value] = pair`) or a meaningful alias, never first/second.
- No `_` placeholders in bindings, even when unused.
- Annotate opaque map types: `map</*key_meaning*/K, /*value_meaning*/V>`, no `=`.

# Pull Request Description & Commit Message

Short and concise so the reviewer gets it fast, max 50 words, compact structure, no big titles —
usable directly as a squash-merge commit message.
