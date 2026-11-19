本仓库是麻将游戏工程（`game/`），actor 框架 ex_actor 作为 git submodule 放在 `third_party/ex-actor`。

- 首次克隆后必须先 `git submodule update --init --recursive`，否则 configure 会失败。
- 框架文档在 `third_party/ex-actor/docs/contents`（框架 API、调度器、分布式模式）；游戏文档在 `game/docs`。
- 构建&测试：`python3 scripts/regen_build_dir.py` 生成 `build/`，然后 `cmake --build build --config Release`、`ctest -C Release`。服务端二进制是 `build/game/mahjong_server`。
- 日常编译只用 `cmake --build build`（它会自动重 glob 并刷新根目录 `compile_commands.json`）；需要显式重配置时用 `python3 scripts/regen_build_dir.py --incremental`（复用 `build/`，约 10 秒，不删东西）。不要随手 `rm -rf build`、`git clean -xdf`，也不要无必要地跑默认（删目录）的 `regen_build_dir.py`。注意根目录那份 db 是加工过的（约 110 条头文件/依赖锚点条目，含 `-x c++-header`），绝不能用 `build/compile_commands.json` 覆盖。详见 AGENTS.md 的 `Don't Break clangd` 一节。
- 依赖源码（stdexec/capnproto/reflect-cpp 等）在 `~/.cache/CPM`，`CPM_SOURCE_CACHE` 由根 `CMakeLists.txt` 设定，所以 configure 不会往 `build/_deps` 拉源码。
- 不要在 `third_party/ex-actor` 里改代码：框架升级是「切子模块到新 commit + 提交子模块指针」，本地修改会丢失。

When work finished, you should fix lint errors.

When I ask to you to write test, if your test fails and you think your test is correct, you should ask me to check, instead of just modify the test code to make it pass.
