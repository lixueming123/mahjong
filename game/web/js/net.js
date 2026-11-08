/**
 * 传输层：一条 WebSocket = 一条游戏连接（PROTOCOL.md §1.4）。
 *
 * 浏览器没有裸 TCP，服务端只讲 TCP + NDJSON，中间由 `game/web/serve.py` 转发。
 * 这一层只做「连接 / 收发 JSON / 心跳 / 断线通知」，不认识任何帧语义 ——
 * 帧的处理全在 client.js。
 *
 * 心跳：§1.6 冻结 5000 ms 一次 `ping`，服务端原样回 `pong`。
 */

import { FRAME_TYPE, MAXS } from './protocol_enums.js';

const PING_INTERVAL_MS = 5000;
/** §1.2 第 3 条：单行超过 64 KB 视为协议错误 */
const MAX_LINE = 65536;

export class Net {
  constructor(url) {
    this.url = url;
    this.ws = null;
    this.connected = false;
    this.rtt = null;
    this.handlers = new Map();
    this.pingTimer = null;
    this.sentFrames = 0;
    this.recvFrames = 0;
    this.badFrames = 0;
    this.lastError = null;
    this._pingAt = 0;
    /** 最近一次收到任意入站帧的时刻（performance.now）；用于 §8.2 的静默检测 */
    this.lastInbound = 0;
  }

  on(evt, fn) {
    if (!this.handlers.has(evt)) this.handlers.set(evt, []);
    this.handlers.get(evt).push(fn);
    return this;
  }

  emit(evt, payload) {
    for (const fn of this.handlers.get(evt) || []) fn(payload);
  }

  connect() {
    if (this.ws) this.close('重连');
    this.emit('status', { state: 'connecting', url: this.url });
    return new Promise((resolve, reject) => {
      let settled = false;
      let socket;
      try {
        socket = new WebSocket(this.url);
      } catch (err) {
        this.emit('status', { state: 'error', message: err.message });
        reject(err);
        return;
      }
      this.ws = socket;
      socket.onopen = () => {
        if (this.ws !== socket) return;    // 已被新连接替换
        settled = true;
        this.connected = true;
        this.lastInbound = performance.now();
        this.emit('status', { state: 'open', url: this.url });
        this.emit('open');
        // §4.8 / §1.6：心跳**必须等 login_ok 之后**才开始（Net 不知道登录时机，
        // 由 client 在 login_ok 分支调用 startHeartbeat；此前发 ping 会被判 NOT_LOGGED_IN）。
        resolve();
      };
      socket.onmessage = (event) => {
        if (this.ws !== socket) return;    // 旧连接迟到的帧，丢弃
        this.lastInbound = performance.now();
        // serve.py 每行一帧；服务端可能一包多帧，这里仍按行容错切一次
        for (const line of String(event.data).split('\n')) {
          if (!line.trim()) continue;
          if (line.length > MAX_LINE) {
            this.badFrames += 1;
            console.warn('收到超过 64 KB 的单帧，主动断开');
            this.close('收到超过 64 KB 的单帧');
            return;
          }
          let frame;
          try {
            frame = JSON.parse(line);
          } catch (err) {
            this.badFrames += 1;
            console.warn('丢弃非法帧', line.slice(0, 200), err.message);
            continue;
          }
          if (!frame || !Number.isInteger(frame.type) || frame.type < 1 || frame.type > MAXS.frame_type) {
            this.badFrames += 1;
            continue;
          }
          this.recvFrames += 1;
          if (frame.type === FRAME_TYPE.PONG) {
            this.rtt = this._pingAt ? Math.round(performance.now() - this._pingAt) : this.rtt;
            this.emit('rtt', this.rtt);
          }
          this.emit('frame', frame);
        }
      };
      socket.onerror = (event) => {
        if (this.ws !== socket) return;
        this.lastError = event?.message || 'websocket error';
        if (!settled) {
          settled = true;
          reject(new Error(this.lastError));
        }
      };
      socket.onclose = (event) => {
        if (this.ws !== socket) return;    // 旧连接迟到的 close，不得影响新连接
        this.connected = false;
        this.stopHeartbeat();
        this.ws = null;
        this.emit('status', { state: 'closed', code: event.code, reason: event.reason });
        this.emit('close', { code: event.code, reason: event.reason });
        if (!settled) {
          settled = true;
          reject(new Error(event.reason || '连接已关闭'));
        }
      };
    });
  }

  /** 发一帧；服务端按 `\n` 切帧，这里补上结尾换行更稳 */
  send(frame) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    try {
      this.ws.send(`${JSON.stringify(frame)}\n`);
      this.sentFrames += 1;
      return true;
    } catch (err) {
      console.warn('发送失败', frame?.type, err.message);
      return false;
    }
  }

  /** 登录成功后调用：每 5 s 一帧 ping（§1.6 冻结值），pong 回来算 RTT */
  startHeartbeat() {
    if (this.pingTimer) return;
    this.pingTimer = setInterval(() => {
      this._pingAt = performance.now();
      this.send({ type: FRAME_TYPE.PING });
    }, PING_INTERVAL_MS);
  }

  stopHeartbeat() {
    if (this.pingTimer) clearInterval(this.pingTimer);
    this.pingTimer = null;
  }

  close(reason = '主动断开') {
    this.stopHeartbeat();
    const socket = this.ws;
    this.connected = false;
    // 不在这里把 this.ws 置空：onclose 需要靠「this.ws === socket」判断自己仍是当前连接，
    // 否则主动断开就收不到 close 通知（重连时旧 socket 的迟到事件也会污染新连接）。
    if (socket && socket.readyState <= WebSocket.OPEN) {
      try {
        socket.close(1000, reason);
      } catch {
        /* 已经关了，忽略 */
      }
    } else if (!socket || socket.readyState === WebSocket.CLOSED) {
      this.ws = null;
    }
  }
}
