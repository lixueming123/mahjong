#!/usr/bin/env bash
# 只启停 Web 前端：静态站点 + WebSocket→TCP 桥（game/web/serve.py）。
# 游戏服务端不归这里管，自己起：./game/start_server.sh
#
#   ./game/start_web.sh                  # 不带子命令 = start
#   ./game/start_web.sh start            # 站点 + 桥，默认 8080 -> 127.0.0.1:9527
#   ./game/start_web.sh stop             # 只停桥，绝不碰你自己起的 mahjong_server
#   ./game/start_web.sh restart
#   ./game/start_web.sh status           # 桥的 pid/端口 + GET / 自检 + 目标服务端是否在线
#   ./game/start_web.sh logs             # 最近 20 行日志（不跟随）
#
# 端口（两种写法等价）：
#   WEB_HTTP_PORT=9000 WEB_GAME_PORT=19527 ./game/start_web.sh
#   ./game/start_web.sh --http-port 9000 --game-port 19527
#   --game-port 要和 start_server.sh 的 --port 一致，桥只按这个端口转发。
#
# 服务端没起也能把站点拉起来（桥是懒连接），start / status 会提示那个端口上没人。
# 产物：.run/web-bridge.pid 与 logs/web-bridge.log，都不进版本库，删掉不影响任何东西。
#
# 与 start_server.sh 同样的 bash 3.2 约定：变量展开一律写 ${VAR}（macOS 自带 bash 在
# UTF-8 locale 下会把紧跟 $VAR 的中文标点吃进变量名）。
set -euo pipefail

GAME_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "${GAME_DIR}")"

HTTP_PORT="${WEB_HTTP_PORT:-8080}"
GAME_PORT="${WEB_GAME_PORT:-9527}"
HOST="127.0.0.1"
PYTHON="${PYTHON:-python3}"

RUN_DIR="${REPO_ROOT}/.run"
LOG_DIR="${REPO_ROOT}/logs"
BRIDGE_PID_FILE="${RUN_DIR}/web-bridge.pid"
BRIDGE_LOG="${LOG_DIR}/web-bridge.log"

usage() {
  # 文件头注释就是帮助文本：从第 2 行起，连续以 # 开头的行原样打印。
  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"
}

log() {
  printf '[web] %s\n' "$*"
}

err() {
  printf '[web] %s\n' "$*" >&2
}

# ------------------------------------------------------------------ 进程/端口探测

alive() {
  local pid="${1:-}"
  [[ -n "${pid}" ]] || return 1
  kill -0 "${pid}" 2>/dev/null || return 1
  # 僵尸也算死：进程已退出，只等父进程回收（ps 不可用时这层判断自动跳过）。
  local stat
  stat="$(ps -p "${pid}" -o stat= 2>/dev/null | tr -d '[:space:]')" || return 0
  [[ "${stat}" == "Z"* ]] && return 1
  return 0
}

# 读 pid 文件；文件不存在/为空/内容不是数字都算「没有」。
pid_of() {
  local file="${1}" pid
  [[ -f "${file}" ]] || return 1
  pid="$(cat "${file}" 2>/dev/null || true)"
  case "${pid}" in
    '' | *[!0-9]*) return 1 ;;
  esac
  printf '%s\n' "${pid}"
}

port_busy() {
  [[ -n "$(lsof -nP -iTCP:"${1}" -sTCP:LISTEN -t 2>/dev/null || true)" ]]
}

# 等端口进入 LISTEN；tries 是 0.2s 的倍数。
wait_port() {
  local port="${1}" tries="${2}" i=0
  while [[ ${i} -lt ${tries} ]]; do
    port_busy "${port}" && return 0
    sleep 0.2
    i=$((i + 1))
  done
  return 1
}

# 端口被别的进程占了：把占用者打出来，交给用户决定（绝不替用户 kill 陌生进程）。
complain_port() {
  local port="${1}"
  err "端口 ${port} 已被占用，但不是本脚本记录的进程："
  lsof -nP -iTCP:"${port}" -sTCP:LISTEN >&2 2>/dev/null || true
  err "先收掉它（kill <pid>），或用 --http-port 换端口。"
}

# 只看不动：目标端口上有没有服务端在听，供 start / status 提示。
game_server_note() {
  if port_busy "${GAME_PORT}"; then
    printf '在线（%s:%s）\n' "${HOST}" "${GAME_PORT}"
  else
    printf '未监听（%s:%s 上没人；自己起 ./game/start_server.sh）\n' "${HOST}" "${GAME_PORT}"
  fi
}

# ------------------------------------------------------------------ start

start_bridge() {
  local pid
  if pid="$(pid_of "${BRIDGE_PID_FILE}")" && alive "${pid}"; then
    if port_busy "${HTTP_PORT}"; then
      log "站点桥已在运行（pid ${pid}，${HOST}:${HTTP_PORT}）"
    else
      log "站点桥已在运行（pid ${pid}），但不在 ${HTTP_PORT} 上；端口以 ./game/start_web.sh status 为准"
    fi
    return 0
  fi
  rm -f "${BRIDGE_PID_FILE}"

  if port_busy "${HTTP_PORT}"; then
    complain_port "${HTTP_PORT}"
    return 1
  fi

  log "启动站点 + WS 桥 ${HOST}:${HTTP_PORT} -> ${HOST}:${GAME_PORT}"
  mkdir -p "${RUN_DIR}" "${LOG_DIR}"
  nohup "${PYTHON}" "${GAME_DIR}/web/serve.py" \
    --http-port "${HTTP_PORT}" --game-host "${HOST}" --game-port "${GAME_PORT}" \
    >"${BRIDGE_LOG}" 2>&1 &
  printf '%s\n' "$!" >"${BRIDGE_PID_FILE}"

  if ! wait_port "${HTTP_PORT}" 50; then
    err "站点没能在 ${HOST}:${HTTP_PORT} 上监听，最近日志："
    tail -n 20 "${BRIDGE_LOG}" >&2 2>/dev/null || true
    stop_one "${BRIDGE_PID_FILE}"
    return 1
  fi
  log "站点就绪（pid $(pid_of "${BRIDGE_PID_FILE}" || echo '?')，日志 ${BRIDGE_LOG#"${REPO_ROOT}"/}）"
}

do_start() {
  start_bridge
  log "打开 http://${HOST}:${HTTP_PORT}/ 即可游戏；停止用 ./game/start_web.sh stop"
  if ! port_busy "${GAME_PORT}"; then
    err "提醒：游戏服务端 ${HOST}:${GAME_PORT} 还没监听，页面能打开但进局会连不上。"
  fi
}

# ------------------------------------------------------------------ stop

# 优雅停桥：TERM -> 等 10s -> KILL。pid 文件无论如何都清掉。
stop_one() {
  local file="${1}" pid i=0
  if ! pid="$(pid_of "${file}")"; then
    log "站点桥：没有 pid 文件，视为未运行"
    return 0
  fi
  if ! alive "${pid}"; then
    log "站点桥：pid ${pid} 已不在，清掉残留 pid 文件"
    rm -f "${file}"
    return 0
  fi

  log "停止站点桥（pid ${pid}）"
  kill -TERM "${pid}" 2>/dev/null || true
  while [[ ${i} -lt 50 ]]; do
    alive "${pid}" || break
    sleep 0.2
    i=$((i + 1))
  done
  if alive "${pid}"; then
    err "站点桥 10s 内没有退出，发 KILL"
    kill -KILL "${pid}" 2>/dev/null || true
    sleep 0.3
  fi
  rm -f "${file}"
  log "站点桥已停止（游戏服务端没动）"
}

do_stop() {
  stop_one "${BRIDGE_PID_FILE}"
}

# ------------------------------------------------------------------ status

do_status() {
  local pid code
  log "状态："
  if pid="$(pid_of "${BRIDGE_PID_FILE}")" && alive "${pid}"; then
    if port_busy "${HTTP_PORT}"; then
      printf '  站点桥 运行中  pid=%s  端口 %s LISTEN\n' "${pid}" "${HTTP_PORT}"
    else
      printf '  站点桥 运行中  pid=%s  但端口 %s 未监听（正在启动或已异常）\n' "${pid}" "${HTTP_PORT}"
    fi
  else
    printf '  站点桥 未运行\n'
  fi
  if port_busy "${HTTP_PORT}"; then
    code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://${HOST}:${HTTP_PORT}/" || true)"
    printf '  GET /  ->  %s\n' "${code:-无响应}"
  fi
  printf '  游戏服务端 %s\n' "$(game_server_note)"
}

do_logs() {
  if [[ -f "${BRIDGE_LOG}" ]]; then
    printf '=== %s ===\n' "${BRIDGE_LOG#"${REPO_ROOT}"/}"
    tail -n 20 "${BRIDGE_LOG}"
  else
    printf '=== %s ===\n（还没有日志）\n' "${BRIDGE_LOG#"${REPO_ROOT}"/}"
  fi
}

# ------------------------------------------------------------------ main

CMD=""
while [[ $# -gt 0 ]]; do
  case "${1}" in
    start | stop | restart | status | logs) CMD="${1}"; shift ;;
    --http-port) HTTP_PORT="${2:?--http-port 需要端口号}"; shift 2 ;;
    --http-port=*) HTTP_PORT="${1#--http-port=}"; shift ;;
    --game-port) GAME_PORT="${2:?--game-port 需要端口号}"; shift 2 ;;
    --game-port=*) GAME_PORT="${1#--game-port=}"; shift ;;
    -h | --help) usage; exit 0 ;;
    *)
      err "未知子命令或参数：${1}（可用：start / stop / restart / status / logs）"
      usage >&2
      exit 2
      ;;
  esac
done

# 不带子命令就是 start：`./game/start_web.sh` 直接起站点。
CMD="${CMD:-start}"

mkdir -p "${RUN_DIR}" "${LOG_DIR}"

case "${CMD}" in
  start) do_start ;;
  stop) do_stop ;;
  restart)
    do_stop
    do_start
    ;;
  status) do_status ;;
  logs) do_logs ;;
esac
