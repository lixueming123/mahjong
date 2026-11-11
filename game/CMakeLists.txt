add_executable(game_probe server/probe_main.cc)
target_link_libraries(game_probe PRIVATE ex_actor)
add_test(NAME game_probe COMMAND game_probe)

# 麻将规则库：纯函数，不依赖 actor 运行时（规格 game/docs/RULESET.md）
add_library(mahjong_rules STATIC server/mahjong_rules.cc)
target_include_directories(mahjong_rules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)

add_executable(mahjong_rules_test server/mahjong_rules_test.cc)
target_link_libraries(mahjong_rules_test PRIVATE mahjong_rules GTest::gtest_main
                                                GTest::gmock)
add_test(NAME mahjong_rules_test COMMAND mahjong_rules_test)
set_tests_properties(mahjong_rules_test PROPERTIES TIMEOUT 60)

# 日麻规则库：复用 mahjong_rules 的牌张/拆法/听牌/副露原语，只重写役种、符与点数表
# （规格 game/docs/GAME_DESIGN_RIICHI_DRAFT.md §2.2/§2.3）
add_library(riichi_rules STATIC server/riichi_rules.cc)
target_include_directories(riichi_rules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(riichi_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(riichi_rules PUBLIC mahjong_rules)

add_executable(riichi_rules_test server/riichi_rules_test.cc)
target_link_libraries(riichi_rules_test PRIVATE riichi_rules GTest::gtest_main
                                                GTest::gmock)
add_test(NAME riichi_rules_test COMMAND riichi_rules_test)
set_tests_properties(riichi_rules_test PROPERTIES TIMEOUT 60)

# ---- 已冻结（FROZEN）：LegacyRoomActor 的两个测试 -------------------------------------------------
# 这两个二进制驱动的是 `server/room.cc`（LegacyRoomActor），也就是插件化之前那座单体。
# 它现在只作历史参照：**不参与 all 构建、不注册 ctest、不再维护**。
# 语义覆盖已经由 mahjong_classic_game_test / mahjong_riichi_game_test 接手（golden 转录比对）。
# 需要时手动构建：cmake --build build --target mahjong_room_test riichi_room_test
add_executable(riichi_room_test EXCLUDE_FROM_ALL server/riichi_room_test.cc server/room.cc server/timer.cc
                               server/json.cc server/protocol.cc)
target_link_libraries(riichi_room_test PRIVATE ex_actor mahjong_rules riichi_rules GTest::gtest_main
                                                GTest::gmock)

add_executable(mahjong_room_test EXCLUDE_FROM_ALL server/mahjong_room_test.cc server/room.cc server/timer.cc
                                server/json.cc server/protocol.cc)
target_link_libraries(mahjong_room_test PRIVATE ex_actor mahjong_rules riichi_rules GTest::gmock)
# ---- 冻结区结束 --------------------------------------------------------------------------------

# 对照基线压测（冻结）：**同一份** bench 源码（-DMAHJONG_BENCH_LEGACY_ROOM 选房间），
# 只把房间换回 LegacyRoomActor，用来量「外壳化之后到底快了多少」。必须和
# mahjong_room_bench 同口径：
#   * bench 自己的 TU（驱动、AutoClient、帧解析）给 -O2，两边一致；
#   * 被测量的房间代码保持 Debug（room.cc 走静态库继承全局 -O0，新外壳的 room_actor.cc
#     也在 -O0 的 mahjong_room_shell 里）。否则比的就不是房间而是编译选项。
add_library(legacy_room_only STATIC EXCLUDE_FROM_ALL server/room.cc)
target_link_libraries(legacy_room_only PUBLIC ex_actor mahjong_rules riichi_rules spdlog::spdlog)
target_include_directories(legacy_room_only PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
add_executable(room_bench_legacy EXCLUDE_FROM_ALL server/room_bench.cc server/timer.cc
                                 server/json.cc server/protocol.cc)
target_link_libraries(room_bench_legacy PRIVATE legacy_room_only ex_actor mahjong_rules riichi_rules)
target_compile_definitions(room_bench_legacy PRIVATE MAHJONG_BENCH_LEGACY_ROOM=1)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(room_bench_legacy PRIVATE -O2)
endif()


# 账号存储（ACCOUNT_DESIGN.md §2）：用系统装的 libmysqlclient，不走 CPM
# （CPM 在配置期要联网拉源码，而 MySQL 客户端本来就是本机的一个库）。
# 找不到库时不报错，只退化成 db_stub.cc —— 与服务端「MySQL 连不上」是同一种降级行为（§2.3）。
option(MAHJONG_WITH_MYSQL "Build account storage against libmysqlclient" ON)
set(MAHJONG_DB_SOURCES server/db_stub.cc)
set(MAHJONG_MYSQL_FOUND OFF)
if(MAHJONG_WITH_MYSQL)
  find_program(MYSQL_CONFIG mysql_config HINTS /usr/local/mysql/bin /opt/homebrew/bin /usr/bin)
  find_path(
    MYSQL_INCLUDE_DIR mysql.h
    HINTS /usr/local/mysql/include /opt/homebrew/include /usr/local/include/mysql
          /usr/include/mysql)
  find_library(MYSQL_LIBRARY NAMES mysqlclient HINTS /usr/local/mysql/lib /opt/homebrew/lib /usr/lib)
  if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
    set(MAHJONG_DB_SOURCES server/db_mysql.cc)
    set(MAHJONG_MYSQL_FOUND ON)
    get_filename_component(MYSQL_LIBRARY_DIR "${MYSQL_LIBRARY}" DIRECTORY)
    message(STATUS "mahjong: account storage enabled (${MYSQL_LIBRARY})")
  else()
    message(
      STATUS
        "mahjong: libmysqlclient not found (hint: -DMYSQL_INCLUDE_DIR=... -DMYSQL_LIBRARY=...); "
        "building without account storage")
  endif()
endif()

add_library(mahjong_db STATIC ${MAHJONG_DB_SOURCES})
target_include_directories(mahjong_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The storage layer is an actor (DbActor): it needs ex_actor for ActorRef/SendLocal and for
# actor_scope.h, which every reply goes through.
target_link_libraries(mahjong_db PRIVATE spdlog::spdlog ex_actor)
if(MAHJONG_MYSQL_FOUND)
  target_include_directories(mahjong_db PUBLIC ${MYSQL_INCLUDE_DIR})
  target_link_libraries(mahjong_db PUBLIC ${MYSQL_LIBRARY})
endif()

# 通用房间外壳：座位/端点/分发/一次性截止/投递/结算回传/断线重连，不含任何玩法知识
# （room_actor.h 顶部写清了边界）。玩法是插件（game.h），绑定见 room_actor_for.h。
# 实现只有 room_actor.cc；JSON/协议/定时器的实现由使用方提供（mahjong_server 已经这样链接，
# 不重复一份 json.cc/protocol.cc）。
add_library(mahjong_room_shell STATIC server/room_actor.cc)
target_include_directories(mahjong_room_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_room_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_room_shell PUBLIC ex_actor)

# 玩法插件：classic（番数制）。只依赖规则库与共享契约——不认识 actor、端点、定时器
# （边界写在 game.h 顶部）。现在还没有调用方，先单独编成静态库做类型检查；大厅切到
# RoomActorFor<ClassicMahjongGame> 之后由 mahjong_server 直接链接。
# ex_actor 只为了头文件路径：room_contract.h 里的 RoomOwner/TimeoutScheduler 是契约的
# 一部分，玩法本身不 include 任何 actor 头。
add_library(mahjong_classic_game STATIC server/classic/classic_mahjong_game.cc)
target_include_directories(mahjong_classic_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_classic_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_classic_game PUBLIC mahjong_rules spdlog::spdlog ex_actor)

# 玩法单测：直接驱动 ClassicMahjongGame，不建房间、不建 actor、不开 socket。
# 它同时是「端口逐字节兼容」的硬门槛：三个 hash 是拆分前 room.cc 冻结的那三个 classic 值。
add_executable(mahjong_classic_game_test server/classic/classic_mahjong_game_test.cc server/json.cc
                                       server/protocol.cc)
target_include_directories(mahjong_classic_game_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_classic_game_test PRIVATE mahjong_classic_game GTest::gtest_main
                                                     GTest::gmock)
add_test(NAME mahjong_classic_game_test COMMAND mahjong_classic_game_test)
set_tests_properties(mahjong_classic_game_test PROPERTIES TIMEOUT 120)

# 玩法插件：riichi（日麻）。与 classic 是**两份互不相识的代码**——不共享房间、不共享帧，
# 只在规则原语（mahjong_rules 的牌张/拆法/听牌）与契约（game.h / room_contract.h）处相接。
add_library(mahjong_riichi_game STATIC server/riichi/riichi_mahjong_game.cc)
target_include_directories(mahjong_riichi_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_riichi_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_riichi_game PUBLIC mahjong_rules riichi_rules spdlog::spdlog ex_actor)

# 玩法单测：直接驱动 RiichiMahjongGame，不建房间、不建 actor、不开 socket。
# 三个 hash 是拆分前 room.cc 冻结的那三个日麻值（tonpu）。
add_executable(mahjong_riichi_game_test server/riichi/riichi_mahjong_game_test.cc server/json.cc
                                       server/protocol.cc)
target_include_directories(mahjong_riichi_game_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_riichi_game_test PRIVATE mahjong_riichi_game GTest::gtest_main
                                                     GTest::gmock)
add_test(NAME mahjong_riichi_game_test COMMAND mahjong_riichi_game_test)
set_tests_properties(mahjong_riichi_game_test PROPERTIES TIMEOUT 120)

# 麻将服务端（t3：TCP JSON 网关 + 服务器骨架；t4：RoomActor 状态机 / 回合驱动 / 超时 / 结算；
# 账号与战绩：注册/登录 + 战绩存档，见 game/docs/ACCOUNT_DESIGN.md）
add_executable(
  mahjong_server
  server/main.cc
  server/json.cc
  server/protocol.cc
  server/net.cc
  server/connection.cc
  server/player_actor.cc
  server/player_session_agent.cc
  server/player_lobby_agent.cc
  server/player_room_agent.cc
  server/frame_parser.cc
  server/lobby.cc
  server/game_registry.cc
  server/timer.cc
  server/gateway.cc)
target_link_libraries(mahjong_server PRIVATE ex_actor mahjong_rules riichi_rules mahjong_db mahjong_room_shell
                                          mahjong_classic_game mahjong_riichi_game)
if(MAHJONG_MYSQL_FOUND)
  # libmysqlclient's install name is @rpath/libmysqlclient.<n>.dylib, so without this the
  # binary links but fails to launch with "Library not loaded". Its own OpenSSL is resolved
  # through @loader_path next to it, so no -lssl/-lcrypto is needed (that is only for the
  # static archive).
  set_target_properties(mahjong_server PROPERTIES BUILD_RPATH "${MYSQL_LIBRARY_DIR}"
                                                     INSTALL_RPATH "${MYSQL_LIBRARY_DIR}")
endif()

# NDJSON 网关的 JSON 解析/序列化单测
add_executable(mahjong_json_test server/mahjong_json_test.cc server/json.cc)
target_link_libraries(mahjong_json_test PRIVATE GTest::gtest_main GTest::gmock)
add_test(NAME mahjong_json_test COMMAND mahjong_json_test)
set_tests_properties(mahjong_json_test PROPERTIES TIMEOUT 60)

# 账号功能的纯逻辑单测（校验规则 / player_id 派生 / stats 帧形状）。
# 刻意不链接 mahjong_db：这样它在有没有 MySQL 的机器上都恒常可跑。
add_executable(mahjong_account_test server/account_test.cc server/protocol.cc server/json.cc)
target_include_directories(mahjong_account_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_account_test PRIVATE mahjong_rules GTest::gtest_main GTest::gmock)
add_test(NAME mahjong_account_test COMMAND mahjong_account_test)
set_tests_properties(mahjong_account_test PROPERTIES TIMEOUT 60)

# 帧解码层的纯逻辑单测（PROTOCOL §4 的字段规则 / 错误码 / §7.2 计量归属）。
# 与 mahjong_account_test 同理：不链接 actor、不链接 socket，任何机器上恒常可跑。
add_executable(mahjong_frame_parser_test server/frame_parser_test.cc server/frame_parser.cc
                                          server/protocol.cc server/json.cc)
target_include_directories(mahjong_frame_parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_frame_parser_test PRIVATE mahjong_rules GTest::gtest_main GTest::gmock)
add_test(NAME mahjong_frame_parser_test COMMAND mahjong_frame_parser_test)
set_tests_properties(mahjong_frame_parser_test PROPERTIES TIMEOUT 60)

# 会话身份状态机单测（session.h，纯头文件）：五种状态的合法转移与 Closed 吸收。
add_executable(mahjong_session_test server/session_test.cc)
target_include_directories(mahjong_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_session_test PRIVATE GTest::gtest_main GTest::gmock)
add_test(NAME mahjong_session_test COMMAND mahjong_session_test)
set_tests_properties(mahjong_session_test PROPERTIES TIMEOUT 60)

# TCP JSON 网关协议冒烟测试（Python 标准库 socket，无第三方依赖）
# Frozen wire enums (PROTOCOL v2.0): header-only behaviour test + cross-language guard.
add_executable(mahjong_protocol_enums_test server/protocol_enums_test.cc)
target_include_directories(mahjong_protocol_enums_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_protocol_enums_test PRIVATE GTest::gtest_main)
add_test(NAME mahjong_protocol_enums_test COMMAND mahjong_protocol_enums_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME gateway_smoke_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gateway_smoke_test.py
            --server-bin $<TARGET_FILE:mahjong_server>)
  set_tests_properties(gateway_smoke_test PROPERTIES TIMEOUT 120)

  add_test(
    NAME protocol_enums_consistency
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_enums_consistency_test.py)
endif()

# RoomActor 压测：真 actor 运行时、真实座位投递（每个座位是一个真实端点 actor），无 socket。
# 量的是「一局游戏要多少 CPU」和「一个核能带几张桌」，不是通过/失败，所以不注册成 ctest。
add_executable(mahjong_room_bench server/room_bench.cc server/timer.cc server/json.cc server/protocol.cc)
target_link_libraries(mahjong_room_bench PRIVATE ex_actor mahjong_rules riichi_rules mahjong_room_shell
                                               mahjong_classic_game)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  # The tree is configured Debug; a load test at -O0 measures the build type, not the room. This
  # optimizes only this target's own sources (the framework's static lib stays as configured).
  target_compile_options(mahjong_room_bench PRIVATE -O2)
endif()

# t3：LobbyActor 的房间登记与 game_end 后回收（PROTOCOL.md §1.4.5）
add_executable(mahjong_lobby_test server/lobby_test.cc server/lobby.cc server/game_registry.cc server/connection.cc
                                  server/player_actor.cc server/player_session_agent.cc server/player_lobby_agent.cc server/player_room_agent.cc server/frame_parser.cc server/net.cc server/timer.cc
                                  server/json.cc server/protocol.cc)
target_link_libraries(mahjong_lobby_test PRIVATE ex_actor mahjong_rules riichi_rules mahjong_db mahjong_room_shell
                                                mahjong_classic_game mahjong_riichi_game GTest::gtest_main GTest::gmock)
add_test(NAME mahjong_lobby_test COMMAND mahjong_lobby_test)
set_tests_properties(mahjong_lobby_test PROPERTIES TIMEOUT 60)

# AsyncGate：协程互斥锁 + 临界区重叠检测。它是"让 handler 能按协程写、又保持单 actor 一次只跑
# 一个 handler"的外部扩展原型（不改框架代码）；见 async_gate.h 顶部说明。
add_executable(mahjong_async_gate_test server/async_gate_test.cc)
target_link_libraries(mahjong_async_gate_test PRIVATE ex_actor GTest::gtest_main GTest::gmock)
add_test(NAME mahjong_async_gate_test COMMAND mahjong_async_gate_test)
set_tests_properties(mahjong_async_gate_test PROPERTIES TIMEOUT 60)

# t4：一局完整对局的端到端验证（真服务端 + 标准库 socket 客户端，t6 可直接复用）
if(Python3_Interpreter_FOUND)
  add_test(
    NAME room_full_game_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/room_full_game_test.py
            --server-bin $<TARGET_FILE:mahjong_server>)
  set_tests_properties(room_full_game_test PROPERTIES TIMEOUT 180)
endif()

# 账号与战绩的端到端验证（真服务端 + 真 MySQL，用独立测试库 mahjong_test）。
# MySQL 不可用（或服务端编译时就没带账号存储）时以 77 退出 = ctest 显示为 SKIP：
# 宁可让人看见「没跑」，也不要静默变绿。
if(Python3_Interpreter_FOUND)
  add_test(
    NAME account_e2e_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/account_e2e_test.py
            --server-bin $<TARGET_FILE:mahjong_server>)
  set_tests_properties(account_e2e_test PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

  # PROTOCOL v2.2 §8.5：账号刷新重连（登录时查房 + 同座位快照 / resume:false 脱离）
  add_test(
    NAME reconnect_e2e_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/reconnect_e2e_test.py
            --server-bin $<TARGET_FILE:mahjong_server>)
  set_tests_properties(reconnect_e2e_test PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()

# 日麻端到端：真服务端 + create_room{"mode":"riichi"} 打完一局，验牌山切分/持点/和牌载荷
if(Python3_Interpreter_FOUND)
  add_test(
    NAME riichi_e2e_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/riichi_e2e_test.py
            --server-bin $<TARGET_FILE:mahjong_server>)
  set_tests_properties(riichi_e2e_test PROPERTIES TIMEOUT 180)
endif()
