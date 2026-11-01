#!/usr/bin/env bash
# 启动麻将 pygame 客户端（game/client/main.py，见 game/client/README.md）。
#
#   ./game/start_client.sh                                  # 连 127.0.0.1:9527
#   ./game/start_client.sh --port 19527                      # 连别的端口（跟服务端一致）
#   ./game/start_client.sh --host 10.0.0.2 --fullscreen      # 参数原样透传给 main.py
#   ./game/start_client.sh --forget-login                    # 先清掉本机记住的账号口令
#
# 解释器默认用仓库根的虚拟环境 ./.venv/bin/python（pygame 2.6.1）；可用 MAHJONG_PYTHON 覆盖。
# 客户端与主循环无关的参数：--host / --port / --fullscreen / --log-level（PROTOCOL.md §1.8），
# 外加登录记忆的两个本地开关 --no-auto-login / --forget-login（UI_UX_SPEC.md §3.5）。
#
# 注意：本文件里所有变量展开都写成 ${VAR} 形式。macOS 自带的是 bash 3.2，在 UTF-8 locale 下
# 它会把紧跟 $VAR 的多字节字符（如中文标点）吃进变量名，`set -u` 下就是 unbound variable。
set -euo pipefail

GAME_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "${GAME_DIR}")"

usage() {
  # 文件头注释就是帮助文本：从第 2 行起，连续以 # 开头的行原样打印。
  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"
}

case "${1:-}" in
  -h | --help)
    usage
    exit 0
    ;;
esac

PYTHON="${MAHJONG_PYTHON:-${REPO_ROOT}/.venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    echo "[client] 没找到 ${PYTHON}，退回系统 python3" >&2
    PYTHON="$(command -v python3)"
  else
    echo "[client] 没找到 python3；先建虚拟环境：python3 -m venv .venv && ./.venv/bin/pip install pygame" >&2
    exit 1
  fi
fi

if ! "${PYTHON}" -c 'import pygame' >/dev/null 2>&1; then
  echo "[client] ${PYTHON} 里没有 pygame；装一下：${PYTHON} -m pip install pygame" >&2
  exit 1
fi

exec "${PYTHON}" "${GAME_DIR}/client/main.py" "$@"
