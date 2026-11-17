add_executable(game_probe server/probe_main.cc)
target_link_libraries(game_probe PRIVATE ex_actor)
add_test(NAME game_probe COMMAND game_probe)

# 麻将规则库：纯函数，不依赖 actor 运行时（规格 game/docs/RULESET.md）
add_library(mahjong_rules STATIC server/mahjong_rules.cc)
target_include_directories(mahjong_rules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)

# 日麻规则库：复用 mahjong_rules 的牌张/拆法/听牌/副露原语，只重写役种、符与点数表
# （规格 game/docs/GAME_DESIGN_RIICHI_DRAFT.md §2.2/§2.3）
add_library(riichi_rules STATIC server/riichi_rules.cc)
target_include_directories(riichi_rules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(riichi_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(riichi_rules PUBLIC mahjong_rules)

# 天凤牌谱回归（TENHOU_CORPUS.md）：用真实和牌结算校核 riichi::ScoreHand 的符/番/点数/役种。
# 语料是**外部抓取物、不入库**（天凤禁止再配布牌谱），所以用例 JSON 不存在时测试 SKIP（见
# tests/tenhou_corpus_test.py）。测试注册在下面 Python3 找到之后，抓取与转换见
# game/tools/tenhou_fetch.py、tenhou_convert.py。
add_executable(tenhou_rules_check tools/tenhou_rules_check.cc server/json.cc)
target_include_directories(tenhou_rules_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(tenhou_rules_check PRIVATE riichi_rules)

# ---- 已冻结（FROZEN）：LegacyRoomActor 的两个测试 -------------------------------------------------
# 这两个二进制驱动的是 `server/room.cc`（LegacyRoomActor），也就是插件化之前那座单体。
# 它现在只作历史参照：**不参与 all 构建、不注册 ctest、不再维护**。
# 语义覆盖已经由 mahjong_classic_game_test / mahjong_riichi_game_test 接手（golden 转录比对）。
# 需要时手动构建：cmake --build build --target mahjong_room_test riichi_room_test
#
# 它们保留显式源文件列表，因为必须编进冻结的 room.cc；这也正是下面自动注册把它们排除的原因。
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

# 回放存储：ReplayActor 是**唯一**碰回放文件的 actor（写入与读取），跑在自己的单线程池上，
# 理由与 DbActor 完全一致 —— 文件 I/O 会阻塞，不能落在游戏线程（ACCOUNT_DESIGN.md §6.1）。
# 房间只负责序列化（纯 CPU）和投递消息，不碰磁盘。gzip 也在这里做（gzip.cc）：1.5 MB 的
# 回放压一次约 12 ms，那是单桌时间轴的 5 帧，压在房间线程上就是 5 帧延迟。
add_library(mahjong_replay_store STATIC server/replay_store.cc server/gzip.cc)
target_include_directories(mahjong_replay_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_replay_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
# ZLIB::ZLIB 由 third_party/ex-actor 的 CMakeLists 里的 zlib-cmake 提供（find_package(ZLIB)
# 已被它 override），与 capnproto 用的是同一个 target，没有新增依赖。
target_link_libraries(mahjong_replay_store PUBLIC ex_actor mahjong_room_shell mahjong_db spdlog::spdlog ZLIB::ZLIB)

# 通用房间外壳：座位/端点/分发/一次性截止/投递/结算回传/断线重连，不含任何玩法知识
# （room_actor.h 顶部写清了边界）。玩法是插件（game.h），绑定见 room_actor_for.h。
# 实现只有 room_actor.cc；JSON/协议/定时器的实现由使用方提供。replay.cc 是录制器
# （纯数据结构 + 序列化，无 I/O）。
add_library(mahjong_room_shell STATIC server/room_actor.cc server/replay.cc)
target_include_directories(mahjong_room_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_room_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_room_shell PUBLIC ex_actor)

# 玩法插件：classic（番数制）。只依赖规则库与共享契约——不认识 actor、端点、定时器
# （边界写在 game.h 顶部）。
# ex_actor 只为了头文件路径：room_contract.h 里的 RoomOwner/TimeoutScheduler 是契约的
# 一部分，玩法本身不 include 任何 actor 头。
add_library(mahjong_classic_game STATIC server/classic/classic_mahjong_game.cc)
target_include_directories(mahjong_classic_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_classic_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_classic_game PUBLIC mahjong_rules spdlog::spdlog ex_actor)

# 玩法插件：riichi（日麻）。与 classic 是**两份互不相识的代码**——不共享房间、不共享帧，
# 只在规则原语（mahjong_rules 的牌张/拆法/听牌）与契约（game.h / room_contract.h）处相接。
add_library(mahjong_riichi_game STATIC server/riichi/riichi_mahjong_game.cc)
target_include_directories(mahjong_riichi_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mahjong_riichi_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(mahjong_riichi_game PUBLIC mahjong_rules riichi_rules spdlog::spdlog ex_actor)

# 麻将服务端（t3：TCP JSON 网关 + 服务器骨架；t4：RoomActor 状态机 / 回合驱动 / 超时 / 结算；
# 账号与战绩：注册/登录 + 战绩存档，见 game/docs/ACCOUNT_DESIGN.md）
#
# 服务端逻辑编成一个静态库，源文件由 glob 发现：新增一个模块（一个新协议 agent、一个新存储）
# 只加一个 .cc，不改这个文件 —— 二进制和所有单测都链接这个库，于是登记只发生一次。
#
# 排除项完全由文件名决定：
#   * *_test.cc          各自成可执行文件，见下面的自动注册
#   * main.cc            服务端入口，只属于 mahjong_server
#   * probe_main.cc / room_bench.cc   另外两个进程入口
#   * room.cc            LegacyRoomActor，冻结，只给上面两个手动构建的测试用
# db_mysql.cc / db_stub.cc 归 mahjong_db（二选一由 MAHJONG_DB_SOURCES 决定），不在这里重复。
#
# CONFIGURE_DEPENDS 让新增文件在下次构建时自动重新 glob，不必手动重跑 configure。
file(GLOB MAHJONG_SERVER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/server/*.cc)
list(
  FILTER MAHJONG_SERVER_SOURCES
  EXCLUDE
  REGEX ".*(_test\\.cc|/main\\.cc|/probe_main\\.cc|/room_bench\\.cc|/room\\.cc|/db_mysql\\.cc|/db_stub\\.cc)$")
# 下面这些各自属于一个更小的库（它们有自己的依赖与可测试边界，见上文的说明），不能同时编进
# 服务端核心库，否则同一个符号会被两个静态库各自提供一份。
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
  # libmysqlclient's install name is @rpath/libmysqlclient.<n>.dylib, so without this the
  # binary links but fails to launch with "Library not loaded". Its own OpenSSL is resolved
  # through @loader_path next to it, so no -lssl/-lcrypto is needed (that is only for the
  # static archive).
  set_target_properties(mahjong_server PROPERTIES BUILD_RPATH "${MYSQL_LIBRARY_DIR}"
                                                     INSTALL_RPATH "${MYSQL_LIBRARY_DIR}")
endif()

# ---- 单测的自动注册 ------------------------------------------------------------------------------
# 每个 *_test.cc 就是一个 ctest 用例，名字是文件名去掉 .cc：新增一个测试只加一个 .cc，不改这个
# 文件。它们统一链接 mahjong_server_core，所以「这个测试要链接哪些实现」不再是每个
# add_executable 里的一份手工清单，也不会漏。
#
# 例外是上面已经显式建过的两个 LegacyRoomActor 测试（必须编进冻结的 room.cc），所以在这里排除。
#
# 这些测试曾经刻意不链接 mahjong_db，好在没有 MySQL 的机器上恒常可跑。这一点现在由 mahjong_db
# 自己保证：没有 libmysqlclient 时它就是 db_stub.cc，行为与「MySQL 连不上」相同
# （ACCOUNT_DESIGN.md §2.3）。所以统一链接不再让任何测试依赖本机有没有 MySQL。
file(GLOB MAHJONG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/server/*_test.cc
     ${CMAKE_CURRENT_SOURCE_DIR}/server/*/*_test.cc)
list(FILTER MAHJONG_TEST_SOURCES EXCLUDE REGEX ".*/(mahjong_room_test|riichi_room_test)\\.cc$")

# 用例名默认 = 文件名去掉 .cc，并沿用 mahjong_ 前缀（新增文件无需登记）。下面这张表只列出
# 「文件名与用例名对不上」的历史例外，两两成对；新文件不要往这里加。
set(MAHJONG_TEST_NAME_OVERRIDES
    riichi_mahjong_game_test mahjong_riichi_game_test
    classic_mahjong_game_test mahjong_classic_game_test
    riichi_rules_test riichi_rules_test)

# 默认超时 60 秒；要跑完一整场的用例单独放宽（同样两两成对：名字 秒）。
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

# RoomActor 压测：真 actor 运行时、真实座位投递（每个座位是一个真实端点 actor），无 socket。
# 量的是「一局游戏要多少 CPU」和「一个核能带几张桌」，不是通过/失败，所以不注册成 ctest。
add_executable(mahjong_room_bench server/room_bench.cc)
target_link_libraries(mahjong_room_bench PRIVATE mahjong_server_core)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  # The tree is configured Debug; a load test at -O0 measures the build type, not the room. This
  # optimizes only this target's own sources (the framework's static lib stays as configured).
  target_compile_options(mahjong_room_bench PRIVATE -O2)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(NODE_EXECUTABLE node)

# ---- tests/ 目录下脚本测试的自动注册 ---------------------------------------------------------
# tests/*.py 与 tests/web/*.mjs 里凡是名字以 _test.py / .mjs 结尾的都是 ctest 用例：新增一个脚本
# 只加文件，不改这个文件。约定（脚本侧）：
#   * 退出码 0 = 通过，非 0 = 失败；77 = 环境不具备（ctest 显示 SKIP，绝不静默变绿）。
#   * 需要一个真服务端时，声明 --server-bin 选项并接受显式路径；这里统一把 $<TARGET_FILE:mahjong_server>
#     传进去，于是测试跑的一定是刚构建的那个二进制，而不是碰巧躺在 build/ 里的旧文件。
#   * 不需要服务端的脚本一律忽略 --server-bin，所以统一传参是安全的。
#
# 少数用例要非默认的参数，或者根本不该随 `ctest` 跑（要人工看产物、需要外部语料），在下面两张表里
# 说明；表里没有的用例用默认值，两处都不必登记。
#
# MAHJONG_SCRIPT_TEST_EXCLUDE：不自动注册的脚本名（不带扩展名）。
#
# 需要额外参数的脚本在这里排除并由下面显式注册：一个脚本产出多个 ctest 用例时，光看文件名区分不出来。
set(MAHJONG_SCRIPT_TEST_EXCLUDE
    # 手工工具，不是用例：分别生成/渲染产物、抓取语料、导出策略证据、压测连接数。
    render_side_artifacts
    discard_policy_evidence
    socket_load
    # 需要默认环境里没有的可选依赖，或者要求人工看产物。它们都写在 docs 里用手工方式运行
    # （Pillow / pygame + 项目 .venv）：
    #   python3 game/tests/atlas_centering_test.py
    #   ./.venv/bin/python game/tests/login_memory_test.py
    #   ./.venv/bin/python game/tests/client_headless_test.py
    #   ./.venv/bin/python game/tests/client_riichi_e2e_test.py
    atlas_centering_test
    login_memory_test
    client_headless_test
    client_riichi_e2e_test
    # 参数是 --binary/--cases 而不是 --server-bin，下面显式注册为 tenhou_rules_check。
    tenhou_corpus_test
    # 同一脚本的第二组用例（--slow-tests），下面显式注册为 gateway_smoke_slow_test。
    gateway_smoke_test
    # 整场（4..8 局）的真实定时器验收：要十几分钟、依赖本机 MySQL 的测试库与口令，且它的断言
    # 比 room_full_game_test 更苛刻（局间隔下限、M7 探针计数）。手工运行更合适：
    #   python3 game/tests/match_full_e2e_test.py --server-bin build/game/mahjong_server
    # 它此前也没进过 ctest；把它塞进默认跑法只会让每次全量测试多等十几分钟。
    match_full_e2e_test)

# 默认超时 180 秒（一个 e2e 用例要起服务端并打完一局）。
#
# 这里没有 match_full_e2e_test / e2e_full_game 这类整场（4..8 局）脚本：它们跑十几分钟、依赖
# 本机 MySQL 的测试库与口令，此前也没有进过 ctest，所以留在上面的排除表里手工运行。想临时让
# ctest 跑它们，把名字从排除表拿掉并在这里给出超时（脚本自身兜底是 960 秒 = 8 × 120s）。
set(MAHJONG_SCRIPT_TEST_TIMEOUT_OVERRIDES "")

# 已知要跑几分钟、平时不想要的用例打上 slow 标签（`ctest -LE slow` 可跳过）。
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
    # SKIP_RETURN_CODE 77 是脚本自己报「环境不具备」的约定（没有 MySQL、没有语料）。
    set_tests_properties(${test_name} PROPERTIES TIMEOUT ${test_timeout} SKIP_RETURN_CODE 77 ${test_labels})
  endforeach()

  # 天凤牌谱回归的参数不是 --server-bin 而是两个路径，所以单独注册。
  # 用例 JSON 由 game/tools/tenhou_convert.py 生成且不入库（天凤禁止再配布牌谱），
  # 所以不存在的干净 checkout 上以 77 退出 = SKIP。
  add_test(
    NAME tenhou_rules_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tenhou_corpus_test.py
            --binary $<TARGET_FILE:tenhou_rules_check>
            --cases ${CMAKE_BINARY_DIR}/tenhou_cases.json)
  set_tests_properties(tenhou_rules_check PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

  # 同一个脚本的第二组用例：心跳/空闲/托管那几条各要等满 20 秒的空闲窗口，所以脚本本身把它们
  # 做成 opt-in（--slow-tests）。它们是唯一覆盖网关时间路径的用例——I/O 循环被重写时就坏在这里
  # ——因此单独注册，而不是让它们被默认跑法静默跳过。自动注册只按文件名，所以这条显式登记是
  # 必须的：一个脚本产出两个 ctest 用例，靠文件名认不出来。
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
