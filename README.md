# 麻将（mahjong）

四人麻将游戏：C++20 服务端 + Python/pygame 客户端 + Web 前端。服务端基于
[ex_actor](https://github.com/ex-actor/ex-actor) actor 框架，框架作为 git submodule 放在
`third_party/ex-actor`。

```
.
├── third_party/ex-actor/   # ex_actor 框架（submodule，固定 commit，勿改）
├── game/
│   ├── server/             # C++20 服务端：TCP JSON 网关 + RoomActor/PlayerActor/大厅
│   ├── client/             # pygame 客户端
│   ├── web/                # Web 前端 + WebSocket→TCP 桥（serve.py）
│   ├── protocol/           # 线协议枚举（跨语言一致性测试的源头）
│   ├── tests/              # 端到端脚本（Python / node）
│   └── docs/               # 玩法、协议、UI 规格
├── cmake/CPM.cmake         # 依赖管理（根上这份带 curl 回退补丁）
├── scripts/                # regen_build_dir.py / format.py
└── CMakeLists.txt          # 麻将工程入口：全局编译选项 + 依赖 + add_subdirectory(game)
```

## 构建与测试

需要 C++20 编译器（GCC 11+/Clang 16+/MSVC 2022）、CMake 3.25+、python3；有 Ninja 会用多配置生成器，没有就退回 Unix Makefiles。

两个构建目录，按用途分开：

| 目录 | 用途 | 怎么生成 |
|---|---|---|
| `build/` | 日常开发（不指定 build type） | `python3 scripts/regen_build_dir.py` |
| `build-release/` | Release（性能测试、压测、对外跑） | `python3 scripts/regen_build_dir.py --release`，或 `./game/start_server.sh --release --build` |

```bash
# 首次克隆后必须先拉子模块，否则 configure 找不到 ex_actor
git submodule update --init --recursive

python3 scripts/regen_build_dir.py            # 配置 build/，并生成根目录 compile_commands.json
cmake --build build -j 4
ctest --test-dir build --output-on-failure

# Release（配置 build-release/）
python3 scripts/regen_build_dir.py --release
cmake --build build-release -j 4
ctest --test-dir build-release --output-on-failure
```

两份 build 都能直接跑 ctest，不会互相打断。`ctest --test-dir <dir>` 需要 CMake 3.20+。

依赖源码（stdexec / capnproto / reflect-cpp / libzmq / spdlog ...）放在 `~/.cache/CPM`，由根
`CMakeLists.txt` 写进 `CPM_SOURCE_CACHE`（环境变量和 `-DCPM_SOURCE_CACHE` 仍优先），所以源码不进
`build/_deps`，configure 也不用重新联网。首次会自动下载，约几分钟。

### 日常

| 场景 | 命令 |
|---|---|
| 改了已有文件 | `cmake --build build -j 4` |
| 新增/删除 `.cc` | `cmake --build build -j 4`（自动重 glob） |
| 新增测试，要单独编 | `python3 scripts/regen_build_dir.py --incremental`，再 `cmake --build build --target mahjong_xxx_test` |
| 换生成器/编译器 | `python3 scripts/regen_build_dir.py`（重建目录） |

新增源文件不用改 `game/CMakeLists.txt`：它用 `file(GLOB ... CONFIGURE_DEPENDS)`，`*_test.cc` 还会
自动注册成 ctest 用例。

### clangd

编辑器读根目录 `compile_commands.json`，它由 `scripts/clangd_compile_commands.py` 从
`build/compile_commands.json` 加工而来：给工程/框架头文件和每个第三方 include 根补 `-fsyntax-only`
条目（带 `-x c++-header`）。否则打开头文件或依赖源码时 clangd 只能借邻近 TU 的参数，满屏假报错。
`clangd_compile_commands` 目标在每次 `cmake --build build` 时自动刷新它。

- 不要 `rm -rf build` / `git clean -xdf`：根数据库会一起丢
- 不要用 `build/compile_commands.json` 覆盖根目录那份
- 不用手工改 `CMAKE_EXPORT_COMPILE_COMMANDS`（已强制 ON）
- 假报错排查：`clangd --check=<file> --compile-commands-dir=. 2>&1 | grep "Compile command"`，
  出现 `inferred from` 别处就是数据库缺条目
- 手动刷新：`python3 scripts/clangd_compile_commands.py --build-dir build`（`build-release/` 不写这份文件）

服务端二进制在 `build/game/mahjong_server`（Release 是 `build-release/game/mahjong_server`；多配置生成器下
两者都多一层 `Release/`）。`game/start_*.sh` 与 `game/tests/*.py` 都按这些路径找它，找不到会回退到另一个目录。

## 运行

```bash
./game/start_server.sh --seed 199        # 服务端，默认 127.0.0.1:9527
./game/start_client.sh                   # pygame 客户端（用 ./.venv/bin/python）
./game/start_web.sh                      # Web 站点 + 桥，默认 8080 -> 127.0.0.1:9527
```

各脚本文件头注释就是完整用法（`--help` 同义）。服务端账号存储走本机 MySQL（`libmysqlclient`），
连不上时自动退化成 `db_stub.cc`，整局游戏不受影响。

## 升级 ex_actor

框架不在本仓库维护，改动只发生在 `third_party/ex-actor` 的指针上：

```bash
cd third_party/ex-actor
git fetch origin && git checkout <new-commit>
cd ../.. && git add third_party/ex-actor
python3 scripts/regen_build_dir.py && cmake --build build --config Release && ctest -C Release
```

当前固定在 `db08716`（上游 main 上的提交），升级时按上面前四步走，别在子模块里直接改代码。

## 文档

- `game/docs/GAME_DESIGN.md` 玩法源头（数值与字段是冻结的，协议与规则库必须与它逐字一致）
- `game/docs/PROTOCOL.md` 线协议；`game/docs/RULESET.md` 规则数值与算法
- `game/docs/UI_UX_SPEC.md` 界面与交互；`game/docs/ACCOUNT_DESIGN.md` 账号与战绩
- 框架文档（API、调度器、分布式模式）在 `third_party/ex-actor/docs/contents`

## 许可证

Apache-2.0，见 [LICENSE](LICENSE)。
