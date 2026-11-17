# Understanding This Project

- Mahjong game (`game/`) built on the ex_actor actor framework, which lives in the
  `third_party/ex-actor` git submodule (pinned; never modify code under `third_party/`).
- Docs: README.md and `game/docs` for the game; `third_party/ex-actor/docs/contents` for the
  framework (API, schedulers, distributed mode).
- Build&test: `git submodule update --init --recursive` once, then
  `python3 scripts/regen_build_dir.py`, `cmake --build build --config Release`, `ctest -C Release`.
  Server binary is `build/game/mahjong_server`.

# Don't Break clangd

The root `compile_commands.json` is not plain CMake output: `regen_build_dir.py` adds a
`-fsyntax-only` entry per header (~96) so headers don't borrow an unrelated TU's command. Lose it
and the editor floods with bogus errors.

- Build/test freely: `cmake --build build [--target <t>]`, `ctest`. Neither touches the database.
- Avoid `rm -rf build`, `git clean -xdf`, and gratuitous `regen_build_dir.py`: they delete both
  `build/` and the root database, and the script deletes *before* configuring — a failed configure
  (usually network, e.g. `status_code: 35` on the rapids-cmake download) leaves nothing behind.
- Never copy `build/compile_commands.json` over the root one: 0 header entries vs ~96.
- Reconfigure only after `cp compile_commands.json /tmp/cc.bak`; restore the backup on failure.
  Any manual CMake call needs `-DCMAKE_EXPORT_COMPILE_COMMANDS=1`.
- Editor errors look bogus? Confirm with `clangd --check=<file> --compile-commands-dir=. 2>&1 |
  grep "Compile command"` — `inferred from` a stranger TU means the errors are fake.

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
