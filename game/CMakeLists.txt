add_executable(game_probe server/probe_main.cc)
target_link_libraries(game_probe PRIVATE ex_actor)
add_test(NAME game_probe COMMAND game_probe)

# 番数制规则库：纯函数，不依赖 actor 运行时。
add_library(mahjong_rules STATIC server/mahjong_rules.cc)
target_include_directories(mahjong_rules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)

# 日麻规则库：复用 mahjong_rules 的牌张/拆法/听牌/副露原语，只重写役种、符与点数表。
add_library(riichi_rules STATIC server/riichi_rules.cc)
target_include_directories(riichi_rules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(riichi_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(riichi_rules PUBLIC mahjong_rules)

# 天凤牌谱回归：用真实和牌结算校核 riichi::ScoreHand。语料不入库（天凤禁止再配布），
# 用例 JSON 不存在时测试 SKIP。
add_executable(tenhou_rules_check tools/tenhou_rules_check.cc server/json.cc)
target_include_directories(tenhou_rules_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(tenhou_rules_check PRIVATE riichi_rules)

# 冻结的 LegacyRoomActor 测试：不参与 all、不注册 ctest，需要时手动构建。
add_executable(riichi_room_test EXCLUDE_FROM_ALL server/riichi_room_test.cc server/room.cc server/timer.cc
                               server/json.cc server/protocol.cc)
target_link_libraries(riichi_room_test PRIVATE ex_actor mahjong_rules riichi_rules GTest::gtest_main
                                                GTest::gmock)

add_executable(mahjong_room_test EXCLUDE_FROM_ALL server/mahjong_room_test.cc server/room.cc server/timer.cc
                                server/json.cc server/protocol.cc)
target_link_libraries(mahjong_room_test PRIVATE ex_actor mahjong_rules riichi_rules GTest::gmock)

# 对照基线压测：同一份 bench 源码，把房间换回 LegacyRoomActor。bench 自己的 TU 与
# mahjong_room_bench 同口径给 -O2，被测量的房间代码保持 Debug。
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

# 账号存储：用系统装的 libmysqlclient，不走 CPM；找不到库时退化成 db_stub.cc。
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
# DbActor needs ex_actor for ActorRef/SendLocal and actor_scope.h.
target_link_libraries(mahjong_db PRIVATE spdlog::spdlog ex_actor)
if(MAHJONG_MYSQL_FOUND)
  target_include_directories(mahjong_db PUBLIC ${MYSQL_INCLUDE_DIR})
  target_link_libraries(mahjong_db PUBLIC ${MYSQL_LIBRARY})
endif()

# 回放存储：ReplayActor 是唯一碰回放文件的 actor，跑在自己的单线程池上（文件 I/O 不落在游戏线程）。
add_library(mahjong_replay_store STATIC server/replay_store.cc server/gzip.cc)
target_include_directories(mahjong_replay_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_replay_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
# ZLIB::ZLIB comes from ex_actor's zlib-cmake (same target capnproto uses).
target_link_libraries(mahjong_replay_store PUBLIC ex_actor mahjong_room_shell mahjong_db spdlog::spdlog ZLIB::ZLIB)

# 通用房间外壳：座位/端点/分发/定时器/投递/重连，不含玩法知识。玩法是插件（game.h）。
add_library(mahjong_room_shell STATIC server/room_actor.cc server/replay.cc)
target_include_directories(mahjong_room_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_room_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_room_shell PUBLIC ex_actor)

# 玩法插件：classic（番数制）。只依赖规则库与共享契约。
# ex_actor 只为头文件路径：room_contract.h 的 RoomOwner/TimeoutScheduler 是契约的一部分。
add_library(mahjong_classic_game STATIC server/classic/classic_mahjong_game.cc)
target_include_directories(mahjong_classic_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_classic_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_classic_game PUBLIC mahjong_rules spdlog::spdlog ex_actor)

# 玩法插件：riichi（日麻）。与 classic 互不相识，只在规则原语与契约处相接。
add_library(mahjong_riichi_game STATIC server/riichi/riichi_mahjong_game.cc)
target_include_directories(mahjong_riichi_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_riichi_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_riichi_game PUBLIC mahjong_rules riichi_rules spdlog::spdlog ex_actor)

# 服务端核心库：源文件由 glob 发现，新增模块只加一个 .cc。二进制与所有单测都链接它。
# 排除项按文件名：测试、其它进程入口、冻结的 room.cc、归 mahjong_db 的两个存储实现。
# CONFIGURE_DEPENDS 让新增文件在下次构建时自动重新 glob。
file(GLOB MAHJONG_SERVER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/server/*.cc)
list(
  FILTER MAHJONG_SERVER_SOURCES
  EXCLUDE
  REGEX ".*(_test\\.cc|/main\\.cc|/probe_main\\.cc|/room_bench\\.cc|/room\\.cc|/db_mysql\\.cc|/db_stub\\.cc)$")
# 这些源文件各自属于上面的小库，重复编进来会让同一个符号被两个静态库各提供一份。
set(MAHJONG_OWNED_BY_OTHER_TARGETS
    ${CMAKE_CURRENT_SOURCE_DIR}/server/mahjong_rules.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/server/riichi_rules.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/server/room_actor.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/server/replay.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/server/replay_store.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/server/gzip.cc)
list(REMOVE_ITEM MAHJONG_SERVER_SOURCES ${MAHJONG_OWNED_BY_OTHER_TARGETS})

add_library(mahjong_server_core STATIC ${MAHJONG_SERVER_SOURCES})
target_include_directories(mahjong_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_server_core
                      PUBLIC ex_actor mahjong_rules riichi_rules mahjong_db mahjong_room_shell
                             mahjong_replay_store mahjong_classic_game mahjong_riichi_game mahjong_asio)

add_executable(mahjong_server server/main.cc)
target_link_libraries(mahjong_server PRIVATE mahjong_server_core)
if(MAHJONG_MYSQL_FOUND)
  # libmysqlclient's install name is @rpath/..., so without this the binary fails to load.
  set_target_properties(mahjong_server PROPERTIES BUILD_RPATH "${MYSQL_LIBRARY_DIR}"
                                                     INSTALL_RPATH "${MYSQL_LIBRARY_DIR}")
endif()

# 单测自动注册：每个 *_test.cc 是一个 ctest 用例，名字为文件名去掉 .cc，统一链接
# mahjong_server_core。两个 LegacyRoomActor 测试要编进冻结的 room.cc，已在上方显式建立。
file(GLOB MAHJONG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/server/*_test.cc
     ${CMAKE_CURRENT_SOURCE_DIR}/server/*/*_test.cc)
list(FILTER MAHJONG_TEST_SOURCES EXCLUDE REGEX ".*/(mahjong_room_test|riichi_room_test)\\.cc$")

# 文件名与用例名对不上的历史例外（两两成对），新文件不要往这里加。
set(MAHJONG_TEST_NAME_OVERRIDES
    riichi_mahjong_game_test mahjong_riichi_game_test
    classic_mahjong_game_test mahjong_classic_game_test
    riichi_rules_test riichi_rules_test)

# 默认超时 60 秒；整场用例单独放宽（两两成对：名字 秒）。
set(MAHJONG_TEST_TIMEOUT_OVERRIDES
    mahjong_classic_game_test 120
    mahjong_riichi_game_test 120
    mahjong_replay_test 120)

foreach(test_source IN LISTS MAHJONG_TEST_SOURCES)
  get_filename_component(test_base ${test_source} NAME_WE)
  set(test_name "mahjong_${test_base}")
  if(test_base MATCHES "^mahjong_")
    set(test_name ${test_base})
  endif()
  list(FIND MAHJONG_TEST_NAME_OVERRIDES ${test_base} name_index)
  if(NOT name_index EQUAL -1)
    math(EXPR name_value "${name_index} + 1")
    list(GET MAHJONG_TEST_NAME_OVERRIDES ${name_value} test_name)
  endif()

  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE mahjong_server_core GTest::gtest_main GTest::gmock)
  add_test(NAME ${test_name} COMMAND ${test_name})

  set(test_timeout 60)
  list(FIND MAHJONG_TEST_TIMEOUT_OVERRIDES ${test_name} timeout_key)
  if(NOT timeout_key EQUAL -1)
    math(EXPR timeout_value "${timeout_key} + 1")
    list(GET MAHJONG_TEST_TIMEOUT_OVERRIDES ${timeout_value} test_timeout)
  endif()
  set_tests_properties(${test_name} PROPERTIES TIMEOUT ${test_timeout})
endforeach()

# RoomActor 压测：真 actor 运行时、真实座位投递，无 socket。量吞吐而非通过/失败，不进 ctest。
add_executable(mahjong_room_bench server/room_bench.cc)
target_link_libraries(mahjong_room_bench PRIVATE mahjong_server_core)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  # Only this target's sources: a load test at -O0 measures the build type.
  target_compile_options(mahjong_room_bench PRIVATE -O2)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(NODE_EXECUTABLE node)

# tests/*.py 与 tests/web/*.mjs 的自动注册：退出码 0 = 通过，77 = SKIP；需要服务端时统一传
# --server-bin $<TARGET_FILE:mahjong_server>。少数需要其它参数的脚本在下面的排除表里注明。
#
# MAHJONG_SCRIPT_TEST_EXCLUDE：不自动注册的脚本名（不带扩展名）。
set(MAHJONG_SCRIPT_TEST_EXCLUDE
    # 手工工具，不是用例：生成/渲染产物、抓取语料、导出策略证据、压测连接数。
    render_side_artifacts
    discard_policy_evidence
    socket_load
    # 需要默认环境里没有的可选依赖（Pillow / pygame + 项目 .venv），见 docs：
    atlas_centering_test
    login_memory_test
    client_headless_test
    client_riichi_e2e_test
    # 参数是 --binary/--cases，下面显式注册为 tenhou_rules_check。
    tenhou_corpus_test
    # 同一脚本的第二组用例（--slow-tests），下面显式注册为 gateway_smoke_slow_test。
    gateway_smoke_test
    # 整场（4..8 局）的真实定时器验收：要十几分钟且依赖本机 MySQL，手工运行
    # （python3 game/tests/match_full_e2e_test.py --server-bin build/game/mahjong_server）。
    match_full_e2e_test)

# 默认超时 180 秒（e2e 用例要起服务端并打完一局），按用例名覆盖（两两成对：名字 秒）。
set(MAHJONG_SCRIPT_TEST_TIMEOUT_OVERRIDES "")

# 要跑几分钟、平时不想要的用例打上 slow 标签（`ctest -LE slow` 可跳过）。
set(MAHJONG_SCRIPT_TEST_SLOW "")

if(Python3_Interpreter_FOUND)
  file(GLOB MAHJONG_PY_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.py)
  foreach(test_source IN LISTS MAHJONG_PY_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)
    if(test_name IN_LIST MAHJONG_SCRIPT_TEST_EXCLUDE)
      continue()
    endif()
    add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE} ${test_source} --server-bin $<TARGET_FILE:mahjong_server>)
    set(test_timeout 180)
    list(FIND MAHJONG_SCRIPT_TEST_TIMEOUT_OVERRIDES ${test_name} timeout_key)
    if(NOT timeout_key EQUAL -1)
      math(EXPR timeout_value "${timeout_key} + 1")
      list(GET MAHJONG_SCRIPT_TEST_TIMEOUT_OVERRIDES ${timeout_value} test_timeout)
    endif()
    set(test_labels "")
    if(test_name IN_LIST MAHJONG_SCRIPT_TEST_SLOW)
      set(test_labels LABELS slow)
    endif()
    # SKIP_RETURN_CODE 77: the script reports a missing environment (no MySQL, no corpus).
    set_tests_properties(${test_name} PROPERTIES TIMEOUT ${test_timeout} SKIP_RETURN_CODE 77 ${test_labels})
  endforeach()

  # 天凤牌谱回归的参数是两个路径而非 --server-bin，所以单独注册。
  add_test(
    NAME tenhou_rules_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tenhou_corpus_test.py
            --binary $<TARGET_FILE:tenhou_rules_check>
            --cases ${CMAKE_BINARY_DIR}/tenhou_cases.json)
  set_tests_properties(tenhou_rules_check PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

  # 同一脚本的第二组用例：心跳/空闲/托管各要等满 20 秒窗口，脚本用 --slow-tests 做成 opt-in；
  # 它们唯一覆盖网关的时间路径，所以单独注册而不是被默认跑法跳过。
  add_test(
    NAME gateway_smoke_slow_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gateway_smoke_test.py
            --server-bin $<TARGET_FILE:mahjong_server> --slow-tests)
  set_tests_properties(gateway_smoke_slow_test PROPERTIES TIMEOUT 400 LABELS slow)
endif()

# 回放播放器的前端逻辑验收（node，无浏览器）：帧驱动 / 视角过滤 / 透视 / 批量推进。
if(NODE_EXECUTABLE)
  file(GLOB MAHJONG_NODE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/web/*.mjs)
  foreach(test_source IN LISTS MAHJONG_NODE_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_test(NAME web_${test_name}_test COMMAND ${NODE_EXECUTABLE} ${test_source})
    set_tests_properties(web_${test_name}_test PROPERTIES TIMEOUT 60)
  endforeach()
endif()
