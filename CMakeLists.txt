cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
# CMake 4.x rejects cmake_minimum_required(VERSION < 3.5) used by old deps (libzmq via ex-actor).
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
project(mahjong VERSION 1.0)

# ------------------- dependency source cache -------------------
# Set in the project (not only the environment) because the IDE configures without a shell
# profile: with the cache unset every configure re-clones ~500 MB of sources into <build>/_deps.
# -DCPM_SOURCE_CACHE or the environment still wins.
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

# A cached CPM_DIRECTORY pointing at another copy makes CPM.cmake bail out of its own include,
# leaving CPMAddPackage undefined. Build trees from before the cache lived here still point at
# <build>/cmake, so drop the stale entry.
if(CPM_SOURCE_CACHE)
  get_filename_component(MAHJONG_CPM_EXPECTED_DIR "${CPM_SOURCE_CACHE}/cpm" ABSOLUTE)
else()
  set(MAHJONG_CPM_EXPECTED_DIR "${CMAKE_BINARY_DIR}/cmake")
endif()
if(DEFINED CPM_DIRECTORY AND NOT CPM_DIRECTORY STREQUAL MAHJONG_CPM_EXPECTED_DIR)
  unset(CPM_DIRECTORY CACHE)
endif()

include(cmake/CPM.cmake)

# ------------------- global setups -------------------
# Top level on purpose: add_subdirectory() does not inherit the standard and compile options.
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
  # Match the Linux 8 MB stack: stdexec operation states overflow the 1 MB Windows default.
  add_link_options(/STACK:8388608)
endif()

# ------------------- editor database -------------------
# Forced on: an IDE configure can pass an empty CMAKE_EXPORT_COMPILE_COMMANDS, which would make the
# clangd refresh below write a database without usable translation units.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------- dependencies -------------------
# ex_actor (third_party/ex-actor) pulls stdexec, capnproto, reflect-cpp, libzmq, spdlog and zlib.
# EX_ACTOR_BUILD_TESTS OFF keeps the framework's own tests (and their googletest) out; testing is
# enabled below for ours.
set(EX_ACTOR_BUILD_TESTS OFF CACHE BOOL "Build ex_actor's own tests")
add_subdirectory(third_party/ex-actor)

CPMAddPackage(
  NAME googletest
  GITHUB_REPOSITORY google/googletest
  GIT_TAG b2b9072ecbe874f5937054653ef8f2731eb0f010
  OPTIONS "INSTALL_GTEST OFF"
  OPTIONS "gtest_force_shared_crt ON"
)

# Asio (standalone, header-only, no Boost): upstream has no CMakeLists, so fetch the sources only
# and expose an INTERFACE target.
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
# Keep <repo>/compile_commands.json (headers included, unsupported flags stripped) in sync with the
# last configure: otherwise a rebuilt tree leaves clangd inferring flags from a stranger TU.
# The script is a no-op when nothing changed.
find_package(Python3 COMPONENTS Interpreter QUIET)
# Only build/ owns the database; two trees would overwrite each other's flags.
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
  # The editor may be reading the file: never delete it on `make clean`.
  set_target_properties(clangd_compile_commands PROPERTIES CLEAN_NO_CUSTOM TRUE)

  # `ALL` alone misses single-target builds (IDE build button, `--target mahjong_server`).
  get_property(MAHJONG_GAME_TARGETS DIRECTORY ${CMAKE_SOURCE_DIR}/game PROPERTY BUILDSYSTEM_TARGETS)
  foreach(mahjong_target IN LISTS MAHJONG_GAME_TARGETS)
    get_target_property(mahjong_target_type ${mahjong_target} TYPE)
    if(NOT mahjong_target_type MATCHES "^(INTERFACE_LIBRARY|UTILITY)$")
      add_dependencies(${mahjong_target} clangd_compile_commands)
    endif()
  endforeach()
endif()
