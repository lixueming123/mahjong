cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
# CMake 4.x errors on cmake_minimum_required(VERSION < 3.5). Clamp to 3.5 for old deps like libzmq
# (pulled in by third_party/ex-actor).
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
project(mahjong VERSION 1.0)
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
