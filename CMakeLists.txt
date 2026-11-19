cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
# CMake 4.x errors on cmake_minimum_required(VERSION < 3.5). Clamp to 3.5 for old deps like libzmq
# (pulled in by third_party/ex-actor).
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
project(mahjong VERSION 1.0)

# ------------------- dependency source cache -------------------
# Where CPM keeps the *sources* of stdexec/capnproto/reflect-cpp/libzmq/spdlog/... It is set here,
# in the project, and not only via the environment because the IDE is what configures most of the
# time and CMake Tools does not inherit a shell profile: with CPM_SOURCE_CACHE unset every IDE
# configure clones all of them into <build>/_deps again (minutes on the network, ~500 MB of
# duplicate sources). A value from the environment or -DCPM_SOURCE_CACHE still wins.
if(DEFINED ENV{CPM_SOURCE_CACHE})
  set(MAHJONG_CPM_SOURCE_CACHE_DEFAULT "$ENV{CPM_SOURCE_CACHE}")
elseif(WIN32)
  set(MAHJONG_CPM_SOURCE_CACHE_DEFAULT "$ENV{LOCALAPPDATA}/CPM")
else()
  set(MAHJONG_CPM_SOURCE_CACHE_DEFAULT "$ENV{HOME}/.cache/CPM")
endif()
set(CPM_SOURCE_CACHE
    "${MAHJONG_CPM_SOURCE_CACHE_DEFAULT}"
    CACHE PATH "Directory holding the CPM dependency sources (shared by all build dirs)")

# CPM.cmake bails out of its own include when the cached CPM_DIRECTORY names a different copy, and
# then CPMAddPackage is never defined. Build trees configured before the cache lived here still
# point at <build>/cmake, so drop that stale entry instead of failing with "Unknown CMake command".
if(CPM_SOURCE_CACHE)
  get_filename_component(MAHJONG_CPM_EXPECTED_DIR "${CPM_SOURCE_CACHE}/cpm" ABSOLUTE)
else()
  # -DCPM_SOURCE_CACHE=OFF: CPM keeps its own copy inside the build tree.
  set(MAHJONG_CPM_EXPECTED_DIR "${CMAKE_BINARY_DIR}/cmake")
endif()
if(DEFINED CPM_DIRECTORY AND NOT CPM_DIRECTORY STREQUAL MAHJONG_CPM_EXPECTED_DIR)
  unset(CPM_DIRECTORY CACHE)
endif()

include(cmake/CPM.cmake)

# ------------------- global setups -------------------
# These have to be set at the top level: add_subdirectory() does not inherit the C++ standard or
# compile options from the subdirectory, and game/ is built with the same flags as before when the
# mahjong game lived next to the framework.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fdiagnostics-color=always)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  add_compile_options(-fexperimental-library)
endif()
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1 CACHE INTERNAL "")
if(MSVC)
  add_compile_options(/EHsc)
  add_compile_options(/bigobj)
  # Match the Linux default of 8 MB stack per thread. stdexec operation-state objects can be large;
  # the Windows default (1 MB) is too small for deeply composed sender chains.
  add_link_options(/STACK:8388608)
endif()

# ------------------- editor database -------------------
# Force the export on, whatever the caller passed. The IDE's configure can leave
# CMAKE_EXPORT_COMPILE_COMMANDS empty (`BOOL=`), and then CMake writes a partial or missing
# build/compile_commands.json — which the clangd refresh below would process into a root database
# with no usable translation units, making clangd report every dependency header as missing.
# A normal `set()` beats -D on the command line, so this cannot be overridden by accident.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------- dependencies -------------------
# The ex_actor actor framework is a git submodule (third_party/ex-actor) pinned to a known-good
# commit. Its own CMakeLists pulls the rest of the C++ dependencies through CPM: stdexec,
# capnproto, reflect-cpp, libzmq and spdlog, all of which are needed by both the framework and the
# game (the game uses spdlog directly and ZLIB::ZLIB for replay compression).
#
# EX_ACTOR_BUILD_TESTS OFF keeps the framework's own test suite (and its googletest checkout) out
# of the build: we only need the ex_actor library. It also means `enable_testing()` and googletest
# are ours to provide below.
set(EX_ACTOR_BUILD_TESTS OFF CACHE BOOL "Build ex_actor's own tests")
add_subdirectory(third_party/ex-actor)

# The game's tests are googletest based; keep the same pinned revision the framework used.
CPMAddPackage(
  NAME googletest
  GITHUB_REPOSITORY google/googletest
  GIT_TAG b2b9072ecbe874f5937054653ef8f2731eb0f010
  OPTIONS "INSTALL_GTEST OFF"
  OPTIONS "gtest_force_shared_crt ON"
)

# Asio (standalone, header-only) for the gateway's network layer. The upstream repository ships
# autotools rather than CMake, so it is fetched as source only (DOWNLOAD_ONLY) and exposed through
# the INTERFACE target below. No Boost is involved: asio's detail/config.hpp defines ASIO_STANDALONE
# on its own for C++11 and later. ASIO_NO_DEPRECATED keeps the superseded API out of the build.
CPMAddPackage(
  NAME asio
  GITHUB_REPOSITORY chriskohlhoff/asio
  GIT_TAG asio-1-39-0
  DOWNLOAD_ONLY YES
)
add_library(mahjong_asio INTERFACE)
target_include_directories(mahjong_asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(mahjong_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
find_package(Threads REQUIRED)
target_link_libraries(mahjong_asio INTERFACE Threads::Threads)

enable_testing()
add_subdirectory(game)

# ------------------- clangd database -------------------
# Keep <repo>/compile_commands.json in sync with whatever the last configure produced. The editor
# reads that processed copy (headers included, unsupported flags stripped), not CMake's own file,
# and it used to be refreshed only by scripts/regen_build_dir.py — so a plain `cmake --build` after
# adding a source file re-globbed and reconfigured behind the editor's back: the new TU was absent
# from the database, clangd fell back to inferring flags from a neighbouring TU and reported
# headers of unrelated targets (gtest in a test, <exec/...> in a framework header) as missing.
#
# The script is a no-op when the content would not change, so this costs a python3 start per build
# and nothing else; running it on every build is what makes it impossible to forget.
find_package(Python3 COMPONENTS Interpreter QUIET)
# Only the day-to-day build/ tree owns the editor database: it is the one clangd is pointed at.
# Refreshing it from build-release/ as well would make the two trees overwrite each other's flags
# (Release with optimization, Debug without) depending on which one was built last.
get_filename_component(MAHJONG_BUILD_DIR_NAME "${CMAKE_BINARY_DIR}" NAME)
option(MAHJONG_UPDATE_CLANGD_DB
       "Refresh <repo>/compile_commands.json from this build tree" OFF)
if(MAHJONG_BUILD_DIR_NAME STREQUAL "build")
  set(MAHJONG_UPDATE_CLANGD_DB ON)
endif()
if(MAHJONG_UPDATE_CLANGD_DB AND Python3_Interpreter_FOUND)
  add_custom_target(
    clangd_compile_commands ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/clangd_compile_commands.py
            --build-dir ${CMAKE_BINARY_DIR} --output ${CMAKE_SOURCE_DIR}/compile_commands.json
    COMMENT "Refreshing compile_commands.json for clangd"
    VERBATIM)
  # Nothing else produces this file, but the editor may be reading it: never delete it.
  set_target_properties(clangd_compile_commands PROPERTIES CLEAN_NO_CUSTOM TRUE)

  # `ALL` alone is not enough: `cmake --build build --target mahjong_server` (and the IDE's build
  # button for a single target) does not run it. Hooking our own targets makes the refresh happen
  # on every path that can reconfigure, which is the whole point — a stale database is invisible
  # until the editor starts reporting headers of unrelated targets as missing. Dependency sources
  # are left out on purpose: they are not ours and never touch the database.
  get_property(MAHJONG_GAME_TARGETS DIRECTORY ${CMAKE_SOURCE_DIR}/game PROPERTY BUILDSYSTEM_TARGETS)
  foreach(mahjong_target IN LISTS MAHJONG_GAME_TARGETS)
    get_target_property(mahjong_target_type ${mahjong_target} TYPE)
    if(NOT mahjong_target_type MATCHES "^(INTERFACE_LIBRARY|UTILITY)$")
      add_dependencies(${mahjong_target} clangd_compile_commands)
    endif()
  endforeach()
endif()
