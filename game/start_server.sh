#!/usr/bin/env bash
# 启动麻将服务端（game/server/main.cc：TCP JSON 网关 + actor 世界）。
#
#   ./game/start_server.sh                        # 默认 127.0.0.1:9527
#   ./game/start_server.sh --seed 199             # 固定牌山种子（PROTOCOL.md §1.8 冻结参数）
#   ./game/start_server.sh --port 19527            # 换端口（客户端要用 --port 对齐）
#   ./game/start_server.sh --build                 # 先重新编译再启动
#   ./game/start_server.sh --release               # 启动 release 版（build-release/，必要时先编译）
#   ./game/start_server.sh --port 19527 -- --help  # `--` 之后的参数原样交给服务端
#
# 本脚本只做三件事：找到（必要时编译）mahjong_server、按本机开发习惯填好 MySQL 连接、
# 然后把剩下的参数原样透传。--port / --seed / --action-timeout-ms / --ai-delay-ms / --mysql-*
# 都是服务端自己的参数（`main.cc --help`）。
#
# 注意：本脚本要能在 macOS 自带的 bash 3.2 上跑（`#!/usr/bin/env bash` 拿到的就是它），
# 所以有两处刻意的写法：
#   1) 变量展开一律写 ${VAR}——3.2 在 UTF-8 locale 下会把紧跟 $VAR 的多字节字符（中文标点）
#      吃进变量名，`set -u` 下报 unbound variable；
#   2) 空数组用 ${ARGS[@]+"${ARGS[@]}"} 展开——3.2 连空数组的 "${ARGS[@]}" 也当 unbound
#      variable（4.4 才修），而「不带参数启动」正是最常见的那条路径。
set -euo pipefail

GAME_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "${GAME_DIR}")"

# 账号存储（ACCOUNT_DESIGN.md §2）。默认值是本机开发实例；MySQL 没起时服务端照常启动，
# 只是账号帧报错、整局游戏不受影响，所以这里不做「必须先起 MySQL」的前置检查。
MYSQL_HOST="${MAHJONG_MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MAHJONG_MYSQL_PORT:-3306}"
MYSQL_USER="${MAHJONG_MYSQL_USER:-root}"
MYSQL_DB="${MAHJONG_MYSQL_DB:-mahjong}"
# §6.5：口令不进 argv（进程表里看得见），只走环境变量；服务端自己读 MAHJONG_MYSQL_PASSWORD。
export MAHJONG_MYSQL_PASSWORD="${MAHJONG_MYSQL_PASSWORD:-12345678}"

usage() {
  # 文件头注释就是帮助文本：从第 2 行起，连续以 # 开头的行原样打印。
  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"
}

find_binary() {
  local rel
  # 默认优先 build/（就是「刚编出来的那份」），再回退 build-release/ 与多配置生成器的子目录。
  # --release 时把两个顺序对调，优先取 build-release/ 里的那份。
  if [[ "${RELEASE}" == "1" ]]; then
    for rel in \
      build-release/game/mahjong_server \
      build-release/game/Release/mahjong_server \
      build-release/game/Debug/mahjong_server \
      build/game/mahjong_server \
      build/game/Release/mahjong_server \
      build/game/Debug/mahjong_server; do
      if [[ -x "${REPO_ROOT}/${rel}" ]]; then
        printf '%s\n' "${REPO_ROOT}/${rel}"
        return 0
      fi
    done
  else
    for rel in \
      build/game/mahjong_server \
      build-release/game/mahjong_server \
      build/game/Release/mahjong_server \
      build/game/Debug/mahjong_server \
      build-release/game/Release/mahjong_server \
      build-release/game/Debug/mahjong_server; do
      if [[ -x "${REPO_ROOT}/${rel}" ]]; then
        printf '%s\n' "${REPO_ROOT}/${rel}"
        return 0
      fi
    done
  fi
  return 1
}

# --release 时默认编译到 build-release/，否则用常规的 build/；两者都能被 MAHJONG_* 环境变量覆盖。
default_build_dir() {
  if [[ "${RELEASE}" == "1" ]]; then
    printf '%s\n' "${MAHJONG_BUILD_DIR:-${REPO_ROOT}/build-release}"
  else
    printf '%s\n' "${MAHJONG_BUILD_DIR:-${REPO_ROOT}/build}"
  fi
}

build_binary() {
  local build_dir
  build_dir="$(default_build_dir)"
  local config="${MAHJONG_BUILD_CONFIG:-Release}"
  if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    # 首次配置会拉 CPM 依赖（要联网）。有 ninja 就用多配置生成器（与
    # third_party/ex-actor/docs/contents/installation.md 一致），没有就退回 Unix Makefiles：
    # macOS/Homebrew 常常没装 ninja，不该让「跑起来」卡在一个纯构建工具上。
    local generator="Ninja Multi-Config"
    if ! command -v ninja > /dev/null 2>&1; then
      generator="Unix Makefiles"
    fi
    echo "[server] 配置 ${build_dir}（${generator}）"
    cmake -S "${REPO_ROOT}" -B "${build_dir}" -G "${generator}"
  fi
  # 单配置生成器（Unix Makefiles 等）不认 --config，也不该在提示里假装有配置。
  if grep -q '^CMAKE_CONFIGURATION_TYPES' "${build_dir}/CMakeCache.txt" 2>/dev/null; then
    echo "[server] 编译 mahjong_server（${build_dir}，配置 ${config}）"
    cmake --build "${build_dir}" --target mahjong_server --config "${config}"
  else
    echo "[server] 编译 mahjong_server（${build_dir}，单配置）"
    cmake --build "${build_dir}" --target mahjong_server
  fi
}

FORCE_BUILD=0
RELEASE=0
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) FORCE_BUILD=1; shift ;;
    --release) RELEASE=1; shift ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      ARGS+=("$@")
      break
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ -n "${MAHJONG_SERVER_BIN:-}" ]]; then
  # 显式指定就只认这一个：不再自动编译，也不悄悄回退到别的目录里的旧二进制。
  [[ "${FORCE_BUILD}" == "1" ]] && build_binary
  if [[ ! -x "${MAHJONG_SERVER_BIN}" ]]; then
    echo "[server] MAHJONG_SERVER_BIN 不存在或不可执行：${MAHJONG_SERVER_BIN}" >&2
    exit 1
  fi
  BIN="${MAHJONG_SERVER_BIN}"
else
  BIN="$(find_binary || true)"
  # --release 却没找到 build-release/ 里的那份时，先编一次，别悄悄退回 Debug。
  if [[ "${RELEASE}" == "1" && -n "${BIN}" && "${BIN}" != *"/build-release/"* ]]; then
    echo "[server] --release 但没找到 build-release/ 下的二进制，先编译"
    build_binary
    BIN=""
  fi
  if [[ -z "${BIN}" || "${FORCE_BUILD}" == "1" ]]; then
    build_binary
    BIN="$(find_binary || true)"
  fi
  if [[ -z "${BIN}" ]]; then
    echo "[server] 编译后仍未找到 mahjong_server；可用 MAHJONG_SERVER_BIN=<路径> 指定。" >&2
    exit 1
  fi
fi

# 只为了把连接地址说清楚：服务端参数直接透传，这里仅扫一眼 --port。
LISTEN_PORT=9527
for ((i = 0; i < ${#ARGS[@]}; i++)); do
  case "${ARGS[$i]}" in
    --port) [[ $((i + 1)) -lt ${#ARGS[@]} ]] && LISTEN_PORT="${ARGS[$((i + 1))]}" ;;
    --port=*) LISTEN_PORT="${ARGS[$i]#--port=}" ;;
  esac
done

echo "[server] 二进制  ${BIN}"
echo "[server] 账号库  ${MYSQL_HOST}:${MYSQL_PORT}（user=${MYSQL_USER} db=${MYSQL_DB}）"
echo "[server] 监听    127.0.0.1:${LISTEN_PORT} —— 客户端：./game/start_client.sh --port ${LISTEN_PORT}"
echo "[server] 启动中…（Ctrl-C 停止）"
# `${ARGS[@]+"${ARGS[@]}"}`：bash 3.2/4.0–4.3 在 `set -u` 下把**空数组**的 "${ARGS[@]}" 当作
# unbound variable（4.4 才修），所以必须先判断「已定义」再展开——不带参数启动时就会走到这里。
exec "${BIN}" \
  --mysql-host "${MYSQL_HOST}" --mysql-port "${MYSQL_PORT}" \
  --mysql-user "${MYSQL_USER}" --mysql-db "${MYSQL_DB}" \
  ${ARGS[@]+"${ARGS[@]}"}
