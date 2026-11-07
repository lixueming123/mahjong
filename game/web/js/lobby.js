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

export class Lobby {
  constructor({ app, client, auto = false }) {
    this.app = app;
    this.client = client;
    this.auto = auto;               // URL 带昵称/账号：跳过大厅直接进桌
    this.mode = 'classic';
    this.length = 'tonpu';
    this.visible = false;
    this.busy = false;
    this._poll = 0;

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

      roomCode: $('room-code'),
      roomMode: $('room-mode'),
      roomSeats: $('room-seats'),
      roomTip: $('room-tip'),
      ready: $('btn-ready'),
      copyRoom: $('btn-copy-room'),
      leaveRoom: $('btn-leave-room'),
    };

    this.bind();
    this.bindClient();
    this.selectMode('classic');
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
    this.el.refresh?.addEventListener('click', () => this.client.listRooms());
    this.el.logout?.addEventListener('click', () => this.logout());
    this.el.ready?.addEventListener('click', () => this.startGame());
    this.el.copyRoom?.addEventListener('click', () => this.copyRoomCode());
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
    this.app.on('game_start', () => this.hide());
    this.app.on('session', ({ stage }) => {
      this.setConnected(stage !== 'closed' && stage !== 'idle');
    });
    this.app.on('error', ({ text }) => {
      this.busy = false;
      this.setButtonsEnabled(true);
      if (this.visible) this.setMessage(text);
      else this.ui?.toast?.(text, 'error');
    });
    this.app.on('close', () => {
      this.setConnected(false);
      if (this.visible) this.setMessage('连接已断开，请重新登录');
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
    if (this.el.roomMode) {
      this.el.roomMode.textContent = `${mode} · ${this.length === 'hanchan' ? '半庄' : '東風'}`;
    }
    if (this.el.roomCode) this.el.roomCode.textContent = room?.roomId || '—';
    this.roomId = room?.roomId || null;
    this.roomReady = false;
    if (this.el.roomTip) {
      const humans = (room?.seats || []).filter((seat) => !seat.is_bot).length;
      this.el.roomTip.textContent = humans >= 2 ? `${humans} 人已入座，可以开始了` : '等待其他玩家加入…';
    }

    const seats = room?.seats || [];
    const mySeat = room?.seat ?? 0;
    const bySeat = new Map(seats.map((seat) => [seat.seat, seat]));
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
    this.setButtonsEnabled(true);
    this.startRoomPolling();
  }

  startGame() {
    if (this.busy) return;
    this.busy = true;
    if (this.el.ready) this.el.ready.disabled = true;
    if (this.el.roomTip) this.el.roomTip.textContent = '发牌中…';
    this.client.ready();
  }

  copyRoomCode() {
    const code = this.el.roomCode?.textContent || '';
    if (!code || code === '—') return;
    const done = () => this.setMessage(`房间号已复制：${code}`, true);
    if (navigator.clipboard?.writeText) {
      navigator.clipboard.writeText(code).then(done).catch(() => this.fallbackCopy(code, done));
    } else {
      this.fallbackCopy(code, done);
    }
  }

  fallbackCopy(text, done) {
    const input = document.createElement('textarea');
    input.value = text;
    input.style.position = 'fixed';
    input.style.opacity = '0';
    document.body.appendChild(input);
    input.select();
    try { document.execCommand('copy'); done(); } catch { this.setMessage(`房间号：${text}`); }
    input.remove();
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
    [this.el.guest, this.el.login, this.el.register, this.el.create, this.el.ready, this.el.leaveRoom]
      .forEach((button) => {
        if (button) button.disabled = !enabled;
      });
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
      this.setMessage(`无法连接服务器：${err.message}`);
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
      this.setMessage(`无法连接服务器：${err.message}`);
    });
  }

  createRoom() {
    if (this.busy) return;
    this.busyOn();
    this.setMessage(`正在创建${MODE_NAMES[this.mode] || ''}房间…`, true);
    this.client.createRoom({ mode: this.mode, length: this.length });
  }

  joinRoom(roomId) {
    if (this.busy) return;
    this.busyOn();
    this.setMessage(`正在加入 ${roomId}…`, true);
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
