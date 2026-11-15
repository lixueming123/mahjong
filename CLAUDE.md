本仓库是麻将游戏工程（`game/`），actor 框架 ex_actor 作为 git submodule 放在 `third_party/ex-actor`。

- 首次克隆后必须先 `git submodule update --init --recursive`，否则 configure 会失败。
- 框架文档在 `third_party/ex-actor/docs/contents`（框架 API、调度器、分布式模式）；游戏文档在 `game/docs`。
- 构建&测试：`python3 scripts/regen_build_dir.py` 生成 `build/`，然后 `cmake --build build --config Release`、`ctest -C Release`。服务端二进制是 `build/game/mahjong_server`。
- 不要在 `third_party/ex-actor` 里改代码：框架升级是「切子模块到新 commit + 提交子模块指针」，本地修改会丢失。

When work finished, you should fix lint errors, and run `python3 scripts/format.py` to format the code.

When I ask to you to write test, if your test fails and you think your test is correct, you should ask me to check, instead of just modify the test code to make it pass.
