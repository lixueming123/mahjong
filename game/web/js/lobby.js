/**
 * 登录 / 大厅。
 *
 * 只做三件事：收集登录凭据、选择玩法（经典 / 日麻東風 / 日麻半庄）、
 * 展示房间列表与战绩。开局后整块隐藏，牌桌由 ui.js 接管。
 *
 * 所有网络动作都转给 client（协议细节在 client.js），这里只负责表单与列表渲染。
 */

const $ = (id) => document.getElementById(id);

const MODE_NAMES = {
  classic: '经典麻将',
  riichi: '日麻',
};

const esc = (text) => String(text ?? '').replace(/[&<>"']/g, (c) => (
  { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
));

const fmt = (value) => (typeof value === 'number' ? value.toLocaleString('en-US') : '—');
const signed = (value) => (typeof value !== 'number' ? '—' : `${value > 0 ? '+' : ''}${value}`);

function ago(unixSeconds) {
  if (!Number.isFinite(unixSeconds)) return '';
  const diff = Math.max(0, Math.floor(Date.now() / 1000 - unixSeconds));
  if (diff < 60) return `${diff} 秒前`;
  if (diff < 3600) return `${Math.floor(diff / 60)} 分钟前`;
  if (diff < 86400) return `${Math.floor(diff / 3600)} 小时前`;
  return `${Math.floor(diff / 86400)} 天前`;
}

/** 房间号格式（PROTOCOL §4.2）：`r-` + 13 位小写 36 进制 */
export const ROOM_CODE_RE = /^r-[0-9a-z]{13}$/;

/** 纯房间号 / 带空白 / 大写 / 完整邀请链接 / `?room=` 片段 → room_id；非法返回 null */
export function parseRoomCode(input) {
  let text = String(input ?? '').trim();
  if (!text) return null;
  const query = text.match(/[?&]room=([^&#\s]*)/i);
  if (query) {
    try { text = decodeURIComponent(query[1]); } catch { text = query[1]; }
  } else if (/^https?:\/\//i.test(text)) {
    // 是链接却没有 room 参数：不是邀请链接，别把域名当成房间号
    return null;
  }
  text = text.trim().toLowerCase();
  return ROOM_CODE_RE.test(text) ? text : null;
}

/**
 * 邀请链接只带房间号与玩法：绝不把昵称/账号/密码带出去。
 * mode/length 必须保留——客户端的 config.mode 决定牌桌按经典还是日麻渲染，
 * 而 join_room 响应里没有玩法字段（PROTOCOL §4.3）。
 */
export function buildInviteUrl(roomId, href, mode = 'classic', length = 'tonpu') {
  const url = new URL(href);
  url.hash = '';
  for (const key of [...url.searchParams.keys()]) url.searchParams.delete(key);
  url.searchParams.set('room', roomId);
  const riichi = mode === 'riichi';
  url.searchParams.set('mode', riichi ? 'riichi' : 'classic');
  if (riichi) url.searchParams.set('length', length === 'hanchan' ? 'hanchan' : 'tonpu');
  return url.toString();
}

export class Lobby {
  constructor({ app, client, auto = false }) {
    this.app = app;
    this.client = client;
    this.auto = auto;               // URL 带昵称/账号：跳过大厅直接进桌
    // 玩法与 client.config 对齐：邀请链接带 mode/length，好友端不能按 classic 渲染日麻房
    const params = new URLSearchParams(globalThis.location?.search || '');
    this.mode = params.get('mode') === 'riichi' ? 'riichi' : 'classic';
    this.length = this.mode === 'riichi' && params.get('length') === 'hanchan' ? 'hanchan' : 'tonpu';
    this.visible = false;
    this.busy = false;
    this._poll = 0;
    // 邀请链接落地：URL 带 room 时，登录成功后一次性自动走加入流程
    this.inviteRoom = params.get('room') || '';

    this.el = {
      root: $('lobby'),
      viewLogin: $('view-login'),
      viewHall: $('view-hall'),
      viewRoom: $('view-room'),
      tabs: [...document.querySelectorAll('.tab')],
      panes: { guest: $('pane-guest'), account: $('pane-account') },
      name: $('in-name'),
      user: $('in-user'),
      pass: $('in-pass'),
      guest: $('btn-guest'),
      login: $('btn-login'),
      register: $('btn-register'),
      msg: $('lobby-msg'),
      lobbyToast: $('lobby-toast'),

      hallName: $('hall-name'),
      hallKind: $('hall-kind'),
      hallConn: $('hall-conn'),
      logout: $('btn-logout'),
      modes: [...document.querySelectorAll('.mode-card')],
      create: $('btn-create'),
      tip: $('hall-tip'),
      refresh: $('btn-refresh'),
      roomList: $('room-list'),
      stats: $('stats'),
      statsKind: $('stats-kind'),
      joinInput: $('in-room'),
      joinButton: $('btn-join-room'),

      roomCode: $('room-code'),
      roomTitle: $('room-title'),
      roomMode: $('room-mode'),
      roomSeats: $('room-seats'),
      roomTip: $('room-tip'),
      ready: $('btn-ready'),
      copyRoom: $('btn-copy-room'),
      copyInvite: $('btn-copy-invite'),
      leaveRoom: $('btn-leave-room'),
    };

    this.bind();
    this.bindClient();
    this.selectMode(this.mode, this.length);
  }

  // ---------------------------------------------------------------- 事件

  bind() {
    this.el.tabs.forEach((tab) => {
      tab.addEventListener('click', () => this.selectTab(tab.dataset.tab));
    });

    this.el.guest?.addEventListener('click', () => this.enterGuest());
    this.el.login?.addEventListener('click', () => this.enterAccount(false));
    this.el.register?.addEventListener('click', () => this.enterAccount(true));

    this.el.name?.addEventListener('keydown', (e) => { if (e.key === 'Enter') this.enterGuest(); });
    this.el.pass?.addEventListener('keydown', (e) => { if (e.key === 'Enter') this.enterAccount(false); });

    this.el.modes.forEach((card) => {
      card.addEventListener('click', () => this.selectMode(card.dataset.mode, card.dataset.length || 'tonpu'));
    });

    this.el.create?.addEventListener('click', () => this.createRoom());
    this.el.joinButton?.addEventListener('click', () => this.joinByCode());
    this.el.joinInput?.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') this.joinByCode();
    });
    this.el.refresh?.addEventListener('click', () => this.client.listRooms());
    this.el.logout?.addEventListener('click', () => this.logout());
    this.el.ready?.addEventListener('click', () => this.startGame());
    this.el.copyRoom?.addEventListener('click', () => this.copyRoomCode());
    this.el.copyInvite?.addEventListener('click', () => this.copyInviteLink());
    this.el.leaveRoom?.addEventListener('click', () => this.leaveRoom());

    this.el.roomList?.addEventListener('click', (e) => {
      const button = e.target.closest('button[data-room]');
      if (button) this.joinRoom(button.dataset.room);
    });
  }

  bindClient() {
    this.app.on('login_ok', ({ name, hasAccount }) => {
      this.busy = false;
      this.el.hallName.textContent = name || '玩家';
      this.el.hallKind.textContent = hasAccount ? '账号' : '游客';
      this.el.statsKind.textContent = hasAccount ? '' : '游客不计战绩';
      this.setMessage('');
      this.setConnected(true);
      // URL 一步到位（auto 且不是大厅发起的登录）：不进大厅，直接让 client 建房
      if (this.auto && !this.client.lobbyDriven) return;
      this.showHall();
      this.client.listRooms();
      this.consumeInvite();
    });
    this.app.on('rooms', (rooms) => {
      this.renderRooms(rooms);
      this.syncRoom(rooms);
    });
    this.app.on('stats', (stats) => this.renderStats(stats));
    this.app.on('room_joined', (room) => {
      this.busy = false;
      this.setButtonsEnabled(true);
      if (!this.visible || this.auto) return;
      this.showRoom(room);
    });
    this.app.on('room_update', (room) => {
      // PROTOCOL §5.17：他人入座/离开的权威座位快照，服务端广播给房内所有真人
      if (!this.visible || !this.el.viewRoom || this.el.viewRoom.hidden) return;
      if (this.roomId && room.roomId !== this.roomId) return;
      this.renderRoomSeats(room.seats || []);
    });
    this.app.on('game_start', () => this.hide());
    this.app.on('session', ({ stage }) => {
      this.setConnected(stage !== 'closed' && stage !== 'idle');
    });
    this.app.on('error', ({ text }) => {
      this.busy = false;
      this.setButtonsEnabled(true);
      if (this.visible) this.notify(text);
      else this.ui?.toast?.(text, 'error');
    });
    this.app.on('close', () => {
      this.setConnected(false);
      if (this.visible) this.notify('连接已断开，请重新登录');
    });
  }

  attachUI(ui) {
    this.ui = ui;
  }

  // ---------------------------------------------------------------- 视图

  showLogin(message = '') {
    this.visible = true;
    this.el.root.hidden = false;
    this.el.viewLogin.hidden = false;
    this.el.viewHall.hidden = true;
    if (this.el.viewRoom) this.el.viewRoom.hidden = true;
    this.setMessage(message);
    this.hideLobbyToast();
    this.stopPolling();
    this.setButtonsEnabled(true);
    this.el.name?.focus();
  }

  showHall() {
    this.visible = true;
    this.el.root.hidden = false;
    this.el.viewLogin.hidden = true;
    this.el.viewHall.hidden = false;
    if (this.el.viewRoom) this.el.viewRoom.hidden = true;
    this.setButtonsEnabled(true);
    this.startPolling();
  }

  /** 建房 / 入房之后：房间号 + 四席位 + 开始按钮（§4.2/§4.3 的等待期） */
  showRoom(room) {
    this.visible = true;
    this.el.root.hidden = false;
    this.el.viewLogin.hidden = true;
    this.el.viewHall.hidden = true;
    if (this.el.viewRoom) this.el.viewRoom.hidden = false;
    this.stopPolling();

    const mode = this.mode === 'riichi' ? '日麻' : '经典';
    if (this.el.roomTitle) {
      this.el.roomTitle.textContent = (room?.seat ?? 0) === 0 ? '房间已创建' : '已加入房间';
    }
    if (this.el.roomMode) {
      this.el.roomMode.textContent = `${mode} · ${this.length === 'hanchan' ? '半庄' : '東風'}`;
    }
    if (this.el.roomCode) this.el.roomCode.textContent = room?.roomId || '—';
    this.roomId = room?.roomId || null;
    this.mySeat = room?.seat ?? 0;
    this.roomReady = false;
    this.renderRoomSeats(room?.seats || []);
    this.setButtonsEnabled(true);
    this.startRoomPolling();
  }

  /** 等待页四席位：room_joined 与 room_update（PROTOCOL §5.17）共用同一份渲染 */
  renderRoomSeats(seats = [], mySeat = this.mySeat ?? 0) {
    const roster = Array.isArray(seats) ? seats : [];
    const bySeat = new Map(roster.map((seat) => [seat.seat, seat]));
    const winds = ['東', '南', '西', '北'];
    if (this.el.roomSeats) {
      this.el.roomSeats.innerHTML = [0, 1, 2, 3].map((seat) => {
        const info = bySeat.get(seat);
        const me = seat === mySeat ? ' me' : '';
        const name = info?.name || `座位 ${seat}`;
        const kind = info?.is_bot ? 'AI' : seat === mySeat ? '你' : '玩家';
        // 建房时 seat 0 = 庄家 = 東，其余按座位号顺推（与开局后的自风一致）
        const wind = winds[seat % 4];
        return `<div class="room-seat${me}">`
          + `<span class="wind">${wind}</span>`
          + `<span class="who">${esc(name)}</span>`
          + `<span class="kind">${kind}</span>`
          + '</div>';
      }).join('');
    }
    if (this.el.roomTip && !this.roomReady) {
      const humans = roster.filter((seat) => !seat.is_bot).length;
      this.el.roomTip.textContent = humans >= 2 ? `${humans} 人已入座，可以开始了` : '等待其他玩家加入…';
    }
  }

  startGame() {
    if (this.busy) return;
    this.busy = true;
    this.roomReady = true;       // 别让迟到的 room_update 覆盖「发牌中…」
    if (this.el.ready) this.el.ready.disabled = true;
    if (this.el.roomTip) this.el.roomTip.textContent = '发牌中…';
    this.client.ready();
  }

  copyRoomCode() {
    const code = this.roomCode();
    if (!code) return;
    this.copyText(code, `房间号已复制：${code}`);
  }

  /** 邀请链接：好友打开后只需填昵称，登录成功即自动入座 */
  copyInviteLink() {
    const code = this.roomCode();
    if (!code) return;
    const href = globalThis.location?.href || '';
    if (!/^https?:\/\//i.test(href)) {
      // file:// 或本地预览：生成的链接好友打不开，别给无效地址
      this.notify(`当前不是 http 页面，请直接复制房间号：${code}`);
      return;
    }
    let link;
    try {
      link = buildInviteUrl(code, href, this.mode, this.length);
    } catch {
      this.notify(`无法生成邀请链接，请手动复制房间号：${code}`);
      return;
    }
    this.copyText(link, `邀请链接已复制：好友打开后填昵称即可加入 ${code}`);
  }

  roomCode() {
    const code = this.el.roomCode?.textContent?.trim() || '';
    return code && code !== '—' ? code : '';
  }

  /** 复制反馈统一走 toast：#lobby-msg 在登录面板里，房间页根本看不到 */
  copyText(text, message) {
    const done = () => this.notify(message, true);
    if (globalThis.navigator?.clipboard?.writeText) {
      navigator.clipboard.writeText(text).then(done).catch(() => this.fallbackCopy(text, done));
    } else {
      this.fallbackCopy(text, done);
    }
  }

  fallbackCopy(text, done) {
    const input = document.createElement('textarea');
    input.value = text;
    input.style.position = 'fixed';
    input.style.opacity = '0';
    document.body.appendChild(input);
    input.select();
    try { document.execCommand('copy'); done(); }
    catch { this.notify(`复制失败，请手动复制：${text}`); }
    input.remove();
  }

  /**
   * 登录页写 #lobby-msg；大厅/房间页写卡片内的 #lobby-toast。
   * 过去这些反馈都写进隐藏的登录面板，用户以为按钮没反应；
   * 全局 #toast(z25) 又会被 .modal-layer(z60) 盖住，所以不能直接用。
   */
  notify(text, ok = false) {
    if (!text) { this.setMessage(''); this.hideLobbyToast(); return; }
    const inLogin = this.el.viewLogin && !this.el.viewLogin.hidden;
    if (inLogin) {
      this.hideLobbyToast();
      this.setMessage(text, ok);
      return;
    }
    const bar = this.el.lobbyToast;
    if (bar) {
      clearTimeout(this._lobbyToastTimer);
      bar.hidden = false;
      bar.className = `lobby-toast ${ok ? 'good' : 'error'}`.trim();
      bar.textContent = text;
      this._lobbyToastTimer = setTimeout(() => { bar.hidden = true; }, 2600);
    } else if (this.ui?.toast) {
      this.ui.toast(text, ok ? 'good' : 'error');
    } else {
      this.setMessage(text, ok);
    }
  }

  hideLobbyToast() {
    clearTimeout(this._lobbyToastTimer);
    if (this.el.lobbyToast) this.el.lobbyToast.hidden = true;
  }

  /** 邀请链接落地：只在手动登录（非 auto）时由 login_ok 触发一次 */
  consumeInvite() {
    const roomId = this.inviteRoom;
    this.inviteRoom = '';
    if (!roomId) return;
    // 手动登录时 config.room 还在，会让「创建房间」误发 join_room；统一由大厅接管
    if (this.client.config) this.client.config.room = '';
    if (this.el.joinInput) this.el.joinInput.value = roomId;
    this.joinByCode(roomId);
  }

  /** 大厅按房间号加入：也接受粘贴的邀请链接 / ?room= 片段 */
  joinByCode(raw) {
    if (this.busy) return;
    const text = raw ?? this.el.joinInput?.value ?? '';
    const roomId = parseRoomCode(text);
    if (!roomId) {
      this.notify('房间号格式不对，应形如 r-0fs9tx1ayb3ep（也可粘贴邀请链接）');
      this.el.joinInput?.focus();
      return;
    }
    if (this.el.joinInput) this.el.joinInput.value = roomId;
    this.busyOn();
    this.notify(`正在加入 ${roomId}…`, true);
    if (!this.client.joinRoom(roomId)) {
      // roomRequested 已置位 / 未连接 / 限流都会返回 false，别把按钮锁死
      this.busy = false;
      this.setButtonsEnabled(true);
      this.notify('加入请求未发出：请检查连接，或先「返回大厅」换新连接再试');
    }
  }

  /** 协议没有 leave_room：换一条连接回大厅（旧房间未开局时由服务端回收） */
  leaveRoom() {
    this.showHall();
    this.client.returnToLobby();
  }

  hide() {
    this.visible = false;
    this.busy = false;
    this.el.root.hidden = true;
    this.hideLobbyToast();
    this.stopPolling();
  }

  selectTab(name) {
    this.el.tabs.forEach((tab) => tab.classList.toggle('active', tab.dataset.tab === name));
    Object.entries(this.el.panes).forEach(([key, pane]) => {
      if (pane) pane.hidden = key !== name;
    });
    this.setMessage('');
  }

  selectMode(mode, length = 'tonpu') {
    this.mode = mode === 'riichi' ? 'riichi' : 'classic';
    this.length = this.mode === 'riichi' && length === 'hanchan' ? 'hanchan' : 'tonpu';
    this.el.modes.forEach((card) => {
      const active = card.dataset.mode === this.mode
        && (this.mode !== 'riichi' || (card.dataset.length || 'tonpu') === this.length);
      card.classList.toggle('active', active);
    });
    if (this.el.tip) {
      this.el.tip.textContent = this.mode === 'riichi'
        ? `${this.length === 'hanchan' ? '半庄 8 局' : '東風 4 局'} · 25000 起 · 有空位时 AI 补齐`
        : '经典 4 局 · 空位由 AI 自动补齐';
    }
  }

  setMessage(text, ok = false) {
    if (!this.el.msg) return;
    this.el.msg.textContent = text || '';
    this.el.msg.classList.toggle('ok', !!text && ok);
  }

  setConnected(connected) {
    if (!this.el.hallConn) return;
    this.el.hallConn.textContent = connected ? '已连接' : '未连接';
    this.el.hallConn.classList.toggle('off', !connected);
  }

  setButtonsEnabled(enabled) {
    [this.el.guest, this.el.login, this.el.register, this.el.create, this.el.joinButton,
      this.el.ready, this.el.leaveRoom, this.el.copyRoom, this.el.copyInvite]
      .forEach((button) => {
        if (button) button.disabled = !enabled;
      });
    if (this.el.joinInput) this.el.joinInput.disabled = !enabled;
    this.el.roomList?.querySelectorAll('button').forEach((button) => { button.disabled = !enabled; });
  }

  busyOn() {
    this.busy = true;
    this.setButtonsEnabled(false);
  }

  // ---------------------------------------------------------------- 动作

  enterGuest() {
    if (this.busy) return;
    const name = (this.el.name?.value || '').trim();
    if (!name) {
      this.setMessage('请输入昵称');
      this.el.name?.focus();
      return;
    }
    if (name.length > 16) {
      this.setMessage('昵称最多 16 个字符');
      return;
    }
    this.setMessage('连接服务器…', true);
    this.busyOn();
    this.client.connect({ name }).catch((err) => {
      this.busy = false;
      this.setButtonsEnabled(true);
      this.notify(`无法连接服务器：${err.message}`);
    });
  }

  enterAccount(register) {
    if (this.busy) return;
    const user = (this.el.user?.value || '').trim();
    const pass = this.el.pass?.value || '';
    if (!/^[A-Za-z0-9_]{3,16}$/.test(user)) {
      this.setMessage('账号需 3~16 位字母 / 数字 / 下划线');
      return;
    }
    if (pass.length < 6) {
      this.setMessage('密码至少 6 位');
      return;
    }
    this.setMessage(register ? '注册中…' : '登录中…', true);
    this.busyOn();
    this.client.connect({
      user, pass, register, name: (this.el.name?.value || '').trim(),
    }).catch((err) => {
      this.busy = false;
      this.setButtonsEnabled(true);
      this.notify(`无法连接服务器：${err.message}`);
    });
  }

  createRoom() {
    if (this.busy) return;
    this.busyOn();
    this.notify(`正在创建${MODE_NAMES[this.mode] || ''}房间…`, true);
    this.client.createRoom({ mode: this.mode, length: this.length });
  }

  joinRoom(roomId) {
    if (this.busy) return;
    this.busyOn();
    this.notify(`正在加入 ${roomId}…`, true);
    this.client.joinRoom(roomId);
  }

  logout() {
    this.client.logout();
    this.showLogin('已退出登录');
    this.setConnected(false);
  }

  /** 房间等待期：用 room_list 里的 players 数刷新「几人已入座」 */
  syncRoom(rooms) {
    if (!this.roomId || !this.el.viewRoom || this.el.viewRoom.hidden) return;
    const mine = (rooms || []).find((room) => room.room_id === this.roomId);
    if (!mine) {
      if (this.el.roomTip) this.el.roomTip.textContent = '房间已开局或已解散';
      return;
    }
    if (this.el.roomTip) {
      const players = mine.players | 0;
      this.el.roomTip.textContent = players >= 2
        ? `${players} 人已入座，可以开始了`
        : '等待其他玩家加入…';
    }
  }

  startRoomPolling() {
    this.stopRoomPolling();
    this._roomPoll = setInterval(() => {
      if (!this.visible || !this.el.viewRoom || this.el.viewRoom.hidden) return;
      this.client.listRooms();
    }, 2000);
  }

  stopRoomPolling() {
    if (this._roomPoll) clearInterval(this._roomPoll);
    this._roomPoll = 0;
  }

  startPolling() {
    this.stopPolling();
    // §4.11：大厅里约每 1 秒刷一次房间列表
    this._poll = setInterval(() => {
      if (this.visible && !this.el.viewHall.hidden) this.client.listRooms();
    }, 1500);
  }

  stopPolling() {
    if (this._poll) clearInterval(this._poll);
    this._poll = 0;
    this.stopRoomPolling();
  }

  // ---------------------------------------------------------------- 渲染

  renderRooms(rooms) {
    if (!this.el.roomList) return;
    if (!Array.isArray(rooms) || !rooms.length) {
      this.el.roomList.innerHTML = '<div class="empty">暂无等待中的房间</div>';
      return;
    }
    this.el.roomList.innerHTML = rooms.map((room) => {
      const id = esc(room.room_id);
      return `<div class="room-item">`
        + `<span>${esc(room.host || '房主')}</span>`
        + `<span class="meta">${room.players ?? 1}/${room.seats ?? 4} 人 · ${id}</span>`
        + `<button data-room="${id}"${this.busy ? ' disabled' : ''}>加入</button>`
        + '</div>';
    }).join('');
  }

  renderStats(stats) {
    if (!this.el.stats) return;
    if (!stats) {
      this.el.stats.innerHTML = '<div class="empty">暂无战绩</div>';
      return;
    }
    const games = stats.games | 0;
    const wins = stats.wins | 0;
    const draws = stats.draws | 0;
    const net = stats.net_score | 0;
    const rate = games ? `${Math.round((wins / games) * 100)}%` : '—';
    const recent = Array.isArray(stats.recent) ? stats.recent : [];
    this.el.stats.innerHTML = [
      `<div class="stat-row"><span>对局</span><b>${games}</b></div>`,
      `<div class="stat-row"><span>和牌</span><b>${wins}</b></div>`,
      `<div class="stat-row"><span>荒庄</span><b>${draws}</b></div>`,
      `<div class="stat-row"><span>和牌率</span><b>${rate}</b></div>`,
      `<div class="stat-row"><span>累计得失</span><b>${signed(net)}</b></div>`,
      recent.length ? '<div class="stats-sep"></div>' : '',
      ...recent.slice(0, 6).map((item) => {
        const delta = item.delta_score | 0;
        const cls = delta > 0 ? 'up' : delta < 0 ? 'down' : '';
        return `<div class="recent-item"><span>${esc(ago(item.ended_at))}</span>`
          + `<span class="${cls}">${signed(delta)}</span></div>`;
      }),
      games ? '' : '<div class="empty">还没有打完一局</div>',
    ].join('');
  }
}
