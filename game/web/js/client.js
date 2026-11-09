/**
 * 协议控制器：把 PROTOCOL.md 的帧翻译成 state.js 的写入，把玩家操作翻译成帧。
 *
 * 分工：
 *   net.js   只管连接与收发（一条 WS = 一条游戏连接，§1.4）
 *   state.js 只管数据（屏幕视角，本家恒 0 号位）
 *   client.js（本文件）管「什么时候发什么帧、收到帧该怎么改状态」
 *
 * 行为口径对齐 `game/client/game_state.py` + `game/client/app.py`（参考客户端）：
 *   - `seq` 只认严格更大：更新的帧推进窗口，过期帧不重建窗口（§6.2 / H9）
 *   - 提交后进入 pending：服务端确认（action_event / 本家 discard_event）前不再发帧
 *   - 窗口到期（服务端 deadline 与本地兜底取小）后按钮全灰（§6.2）
 *   - 立直两段式：点「立直」进入宣言模式 → 点候选牌即宣言并打出（§12.4 / v1.21）
 *   - 吃/暗杠/加杠有多条 options 时，用当前选中的手牌消歧，而不是永远取第一条
 *
 * 上线流程（PROTOCOL.md §9.1）：login → create_room/join_room → ready → game_start。
 * 昵称与玩法用 URL 参数指定（不新增任何界面元素）：
 *   ?name=小明&mode=riichi&length=hanchan&room=r-xxxx
 */
import { Net } from './net.js';
import { normTile, isValidTile, drawnIndex } from './state.js';
import { buildVoiceCues, voiceDuration } from './voice.js';
import { FRAME_TYPE, ACTION, WINDOW, MODE, LENGTH, ERROR_CODE, yakuName } from './protocol_enums.js';

const FALLBACK_NAMES = ['自', '右', '对', '左'];

/** §6.1 两种窗口 + §12.10 抢杠窗口（日麻） */
const WINDOW_TYPES = new Set([WINDOW.DISCARD, WINDOW.CLAIM, WINDOW.CHANKAN]);

/** 生命周期闸门（参考端 app.py 的同名常量） */
const LOGIN_TIMEOUT_MS = 6000;      // 建连后等 login_ok
const ROOM_TIMEOUT_MS = 3000;       // 等 room_joined（§8.6）
const START_TIMEOUT_MS = 10000;     // 等 game_start
const SILENCE_WARN_MS = 16000;      // 心跳连续 3 次无响应
const SILENCE_LIMIT_MS = 20000;     // §8.4 网络无响应
const ZERO_WINDOW_MS = 5000;        // 倒计时归零后仍无状态更新
const ILLEGAL_LIMIT = 10;           // §7.2 服务端第 10 次非法操作断开
const NAME_MAX = 16;

const YAKU_NAMES = {
  menzen: '门前清', tsumo: '门前清自摸', tanyao: '断幺九', pinfu: '平和', iipeikou: '一杯口',
  yakuhai: '箭刻', rinshan: '岭上开花', sanshoku_doujun: '三色同顺', ittsuu: '一气通贯',
  toitoi: '对对和', sanankou: '三暗刻', sankantsu: '三杠子', honitsu: '混一色',
  shousangen: '小三元', seven_pairs: '七对子', chiitoitsu: '七对子',
  pure_one_suit: '清一色', chinitsu: '清一色', chanta: '混全带幺九', junchan: '纯全带幺九',
  ryanpeikou: '二杯口', honroutou: '混老头', riichi: '立直', ippatsu: '一发', chankan: '抢杠',
  haitei: '海底捞月', houtei: '河底捞鱼', dora: '宝牌', ura_dora: '里宝牌', aka_dora: '赤宝牌',
  tenhou: '天和', chiihou: '地和', kokushi: '国士无双', kokushi_13: '国士十三面',
  suuankou: '四暗刻', suuankou_tanki: '四暗刻单骑', daisangen: '大三元', tsuuiisou: '字一色',
  ryuuiisou: '绿一色', chinroutou: '清老头', chuuren: '九莲宝灯', chuuren_junsei: '纯正九莲宝灯',
  suukantsu: '四杠子', shousuushii: '小四喜', daisuushii: '大四喜',
};

/** 操作栏按钮只能按文案取：index.html 保持原样，不为联机改 id */
function collectButtons() {
  const bar = document.querySelector('.action-bar');
  const buttons = bar ? [...bar.querySelectorAll('.call-btn')] : [];
  const byLabel = (label) => buttons.find((button) => button.textContent.trim() === label) || null;
  return {
    chi: byLabel('吃'),
    pong: byLabel('碰'),
    kong: byLabel('杠'),
    riichi: byLabel('立直'),
    win: byLabel('和'),
    pass: byLabel('过'),
    discard: byLabel('打出'),
    sync: byLabel('摸牌'),   // 已从操作栏移除；保留取值以防旧页面缓存
  };
}

const sameTiles = (a, b) => a.length === b.length && a.every((tile, i) => normTile(tile) === normTile(b[i]));

export class MahjongClient {
  constructor({ app, ui, sfx, url, options = {} }) {
    this.app = app;
    this.state = app.state;
    this.ui = ui;
    this.sfx = sfx;
    this.net = new Net(url);
    this.config = options;

    /**
     * 会话凭据（游客 name / 账号 user+pass）。null = 还没登录过（等大厅提交）。
     * 重连、返回大厅都复用它，所以这里存的是「这份会话是谁」而不是某次连接的参数。
     */
    this.credential = null;
    /** 大厅驱动：true 时 login_ok 之后不自动建房，等玩家在大厅选玩法 */
    this.lobbyDriven = false;
    /** URL 直接带昵称/账号：保持一步到位（自动化验收也走这条） */
    this.autoStart = !!(this.config.name || this.config.user);
    /** 入座即开局（URL 一步到位）；大厅建房时改为等玩家点「开始对局」 */
    this.autoReady = true;

    this.window = null;          // 当前动作窗口 {seq, window, options, riichiTiles, tile, remainingMs, expiresAt}
    this.room = null;            // {roomId, seat, seats, mode}
    this.playerId = null;
    this.name = '';
    this.buttons = collectButtons();
    this.handshakeDone = false;

    this.lastSeq = null;         // 已见最大 seq（§6.2 严格更大才推进）
    this.pending = false;        // 已提交动作，等服务端确认
    this.pendingLabel = '';
    this.frozen = false;         // 和牌 / 局终 / 场终后冻结输入
    this.riichiArming = false;   // 立直宣言模式（点候选牌才发帧）
    this.expired = false;
    this._lastSyncAt = -1e9;     // 首次补偿同步不受 1 秒限流影响（performance.now() 从 0 起算）
    this._lastTickSecond = -1;

    // ---- 会话阶段与自愈（对齐参考端 app.py 的 login_stage / retry / watchdog）----
    this.stage = 'idle';         // idle | login | lobby | room | start | closed
    this.stageSince = 0;
    this.connectAttempts = 0;
    this.loginTimeouts = 0;
    this.hasAccount = false;
    this.stats = null;
    this.rooms = [];
    this.illegalCount = 0;
    this.rateLimitedUntil = 0;
    this.roomRequested = false;
    this.readySent = false;
    this._emptyHandSynced = false;
    this._silenceWarned = false;
    this._zeroSince = 0;
    this._zeroWarned = false;
    this._reconnecting = false;   // 主动重连中：旧连接的 close 不再当作"断线"提示
    // PROTOCOL v2.2 §5.1：账号登录会带 in_room，命中时进入"恢复快照"模式，不能再自动建房/
    // 自动 ready。`restoring` 只用于抑制重放快照期间的开场语音与播报。
    this.resumeNext = true;       // 下一次 login 是否请求恢复（返回大厅时置 false）
    this.resumedInRoom = false;   // 本次登录命中 in_room:true
    this.restoring = false;
    this._restoreTimer = 0;
    this.alreadyOnlineRetries = 0;

    this.bindNet();
    this.bindButtons();
    this.bindStore();
    setInterval(() => this.tick(), 200);
  }

  /**
   * 建连（最多 3 次）。带 auth 时立即登录（大厅路径）；
   * 不带 auth 时：URL 里有昵称/账号 → 用 URL 凭据自动登录，否则只建连、等 authenticate。
   */
  async connect(auth = null) {
    if (auth) {
      this.credential = {
        name: auth.name ?? '',
        user: auth.user || '',
        pass: auth.pass || '',
        register: !!auth.register,
      };
      this.lobbyDriven = true;
      this.autoReady = false;
      this.resumeNext = true;   // 大厅手动登录默认请求恢复；只有结算页出口会显式置 false
    } else if (!this.credential) {
      this.credential = {
        name: this.config.name || '',
        user: this.config.user || '',
        pass: this.config.pass || '',
        register: !!this.config.register,
      };
      if (!this.credential.name && !this.credential.user) this.credential = null;
    }
    this.connectAttempts = 0;
    return this.connectWithRetry();
  }

  async connectWithRetry() {
    try {
      await this.net.connect();
      this.connectAttempts = 0;
      return true;
    } catch (err) {
      this.connectAttempts += 1;
      if (this.connectAttempts < 3) {
        const delay = [800, 1600, 3200][Math.min(this.connectAttempts - 1, 2)];
        this.hint(`连接失败（${this.connectAttempts}/3）：${err.message}，${delay / 1000} s 后重试`);
        await new Promise((resolve) => setTimeout(resolve, delay));
        return this.connectWithRetry();
      }
      this.hint(`无法连接 ${this.net.url}：${err.message}`);
      throw err;
    }
  }

  // ---------------------------------------------------------------- 连接

  bindNet() {
    this.net.on('open', () => {
      this.handshakeDone = false;
      this.window = null;
      this.lastSeq = null;
      this.pending = false;
      this.frozen = false;
      this.riichiArming = false;
      this.roomRequested = false;
      this.readySent = false;
      this.illegalCount = 0;
      this.rateLimitedUntil = 0;
      this._emptyHandSynced = false;
      this._silenceWarned = false;
      this._zeroSince = 0;
      this._zeroWarned = false;
      this._reconnecting = false;
      this.alreadyOnlineRetries = 0;
      this.resumedInRoom = false;
      this.restoring = false;
      if (this._restoreTimer) {
        clearTimeout(this._restoreTimer);
        this._restoreTimer = 0;
      }
      this.app.reset();
      this.app.emit('session', { stage: 'connecting' });
      if (this.credential) {
        this.stage = 'login';
        this.stageSince = performance.now();
        this.hint('已连接，正在登录…');
        this.send(this.authFrame());
      } else {
        // 大厅还没提交登录：连接先挂着，凭据到了再发 login（此时发 ping 会被 NOT_LOGGED_IN）
        this.stage = 'ready';
        this.stageSince = performance.now();
        this.app.emit('session', { stage: 'ready' });
      }
    });
    this.net.on('close', ({ reason } = {}) => {
      if (this._reconnecting) return;    // 主动重连：旧连接的 close 不当作断线
      const wasPlaying = this.state.phase === 'playing';
      this.pending = false;
      this.restoring = false;
      this.resumedInRoom = false;
      if (this._restoreTimer) {
        clearTimeout(this._restoreTimer);
        this._restoreTimer = 0;
      }
      this.stage = 'closed';
      this.applyButtons();
      this.app.emit('session', { stage: 'closed' });
      this.app.emit('close', { reason });
      this.hint(wasPlaying
        ? `连接已断开（服务端将托管本局）${reason ? `：${reason}` : ''}`
        : `连接已断开${reason ? `：${reason}` : ''}`);
    });
    this.net.on('frame', (frame) => this.handleFrame(frame));
    this.net.on('rtt', (rtt) => console.debug('rtt', rtt, 'ms'));
  }

  /**
   * §4.1 登录帧：出现 `username` 即账号形态（此时忽略 name）。
   * 凭据来源：大厅表单（connect(auth)）/ URL 参数（?user=alice&pass=secret&register=1）。
   */
  authFrame() {
    const cred = this.credential || {
      name: this.config.name || '',
      user: this.config.user || '',
      pass: this.config.pass || '',
      register: !!this.config.register,
    };
    const name = sanitizeName(cred.name);
    if (cred.user) {
      const frame = {
        type: cred.register ? FRAME_TYPE.REGISTER : FRAME_TYPE.LOGIN,
        username: String(cred.user),
        password: String(cred.pass || ''),
      };
      if (name) frame.name = name;
      // PROTOCOL v2.2 §4.1：账号登录默认请求恢复对局；结算页「返回大厅」把下一帧置 false。
      if (!cred.register && this.resumeNext === false) frame.resume = false;
      this.name = name || String(cred.user);
      return frame;
    }
    const guest = name || `web${Math.floor(Math.random() * 9000 + 1000)}`;
    this.name = guest;
    return { type: FRAME_TYPE.LOGIN, name: guest };
  }

  // ---------------------------------------------------------------- 操作栏

  bindButtons() {
    const buttons = this.buttons;
    if (buttons.discard) buttons.discard.addEventListener('click', () => this.discardSelected());
    if (buttons.pass) buttons.pass.addEventListener('click', () => this.action(ACTION.PASS, []));
    if (buttons.win) buttons.win.addEventListener('click', () => this.action(ACTION.WIN, []));
    if (buttons.chi) buttons.chi.addEventListener('click', () => this.actionFromOptions(ACTION.CHOW));
    if (buttons.pong) buttons.pong.addEventListener('click', () => this.actionFromOptions(ACTION.PONG));
    if (buttons.kong) buttons.kong.addEventListener('click', () => this.actionFromOptions([ACTION.KONG, ACTION.ANKAN, ACTION.KAKAN]));
    if (buttons.riichi) buttons.riichi.addEventListener('click', () => this.declareRiichi());
    this.applyButtons();
  }

  /** 本窗口是否提供该动作（带 tiles 时按牌面序列精确比对，§4.6） */
  hasOption(action, tiles = null) {
    const win = this.window;
    if (!win) return false;
    const options = win.options.filter((item) => item.action === action);
    if (!options.length) return false;
    if (!tiles || !tiles.length) return true;
    return options.some((item) => sameTiles(item.tiles || [], tiles));
  }

  /**
   * 按钮可用性完全按窗口 options 决定（H1），再叠加参考端的四道基础闸门：
   * 无窗口 / 已提交待确认 / 已冻结 / 窗口到期 / 连接断开 —— 任一成立即全灰。
   */
  applyButtons() {
    const win = this.window;
    const base = !!win && !this.pending && !this.frozen && this.remainingMs() > 0 && this.net.connected;
    const set = (button, enabled) => {
      if (button) button.disabled = !enabled;
    };
    const buttons = this.buttons;
    set(buttons.discard, base && win.window === WINDOW.DISCARD && this.state.selected >= 0);
    set(buttons.pass, base && this.hasOption(ACTION.PASS));
    set(buttons.win, base && this.hasOption(ACTION.WIN));
    set(buttons.chi, base && this.hasOption(ACTION.CHOW));
    set(buttons.pong, base && this.hasOption(ACTION.PONG));
    set(buttons.kong, base && (this.hasOption(ACTION.KONG) || this.hasOption(ACTION.ANKAN) || this.hasOption(ACTION.KAKAN)));
    set(buttons.riichi, base && this.hasOption(ACTION.RIICHI));
  }

  // ---------------------------------------------------------------- 对局动作

  remainingMs() {
    if (!this.window) return 0;
    return Math.max(0, this.window.expiresAt - performance.now());
  }

  /** 打牌：必须回带最近一次 action_request.seq（H2），等服务端 discard_event 落地 */
  discardSelected() {
    if (!this.window || this.window.window !== WINDOW.DISCARD) {
      this.hint('现在不是你的出牌窗口');
      return false;
    }
    if (this.pending) {
      this.hint(`已提交「${this.pendingLabel}」，等服务端确认`);
      return false;
    }
    if (this.frozen) return false;
    if (this.remainingMs() <= 0) {
      this.hint('窗口已超时，等待服务端推进');
      return false;
    }
    if (this.state.selected < 0) {
      this.hint('先选一张牌');
      return false;
    }
    // 立直后手牌已固定：只能打出刚摸到的那张（服务端同规则，提前拦一下省一次非法操作）
    if (this.state.riichi[0]) {
      const drawn = drawnIndex(this.state);
      if (drawn >= 0 && this.state.selected !== drawn) {
        this.hint('立直后只能打出刚摸到的那张牌');
        return false;
      }
    }
    const tile = this.state.hands[0][this.state.selected];
    if (!tile) return false;
    const seq = this.window.seq;
    if (!this.send({ type: FRAME_TYPE.DISCARD, tile, seq })) return false;
    this.markPending(`打牌 ${tile}`);
    return true;
  }

  /** 一条 options 里候选动作可能有多条（多种吃法 / 多个暗杠面），用选中的手牌消歧 */
  actionFromOptions(names) {
    const wanted = Array.isArray(names) ? names : [names];
    if (!this.window) return false;
    const options = [];
    for (const name of wanted) {
      options.push(...this.window.options.filter((item) => item.action === name));
    }
    if (!options.length) {
      this.hint('这个动作现在不可用');
      return false;
    }
    // 多种吃法（如 3-4-5万 / 4-5-6万 / 5-6-7万 都含那张 5万）：
    // 优先按玩家当前选中的手牌消歧 —— 点中了哪张就吃含它的那把；
    // 没选中或选中的不在候选里 ⇒ 弹选择条让玩家自己点，**绝不悄悄替选第一种**。
    if (options.length > 1) {
      const index = this.state.selected;
      const tile = index >= 0 ? this.state.hands[0][index] : null;
      if (tile) {
        const face = normTile(tile);
        const hit = options.find((item) => (item.tiles || []).some((code) => normTile(code) === face));
        if (hit) return this.action(hit.action, hit.tiles || []);
      }
      this.ui?.showOptionPicker?.(options, (opt) => this.action(opt.action, opt.tiles || []));
      return true;
    }
    const option = options[0];
    return this.action(option.action, option.tiles || []);
  }

  action(action, tiles = []) {
    if (!this.window) {
      this.hint('没有待回应的窗口');
      return false;
    }
    if (this.pending) {
      this.hint(`已提交「${this.pendingLabel}」，等服务端确认`);
      return false;
    }
    if (this.frozen) return false;
    if (this.remainingMs() <= 0) {
      this.hint('窗口已超时，等待服务端推进');
      return false;
    }
    const option = this.window.options.find(
      (item) => item.action === action && (!tiles.length || sameTiles(item.tiles || [], tiles)),
    );
    if (!option) {
      this.hint('该动作不在当前窗口的 options 内');
      return false;
    }
    // 打牌窗口永不提供「过」（§4.6 合法性矩阵）：即便服务端误发也不发帧
    if (action === ACTION.PASS && ![WINDOW.CLAIM, WINDOW.CHANKAN].includes(this.window.window)) {
      this.hint('打牌窗口不能「过」，直接打牌或宣言');
      return false;
    }
    const tilesOut = option.tiles || [];
    // §4.6：ankan/kakan/riichi 的 tiles 恰好 1 个牌面，多了服务端会回 ILLEGAL_ACTION
    if ((action === ACTION.RIICHI || action === ACTION.ANKAN || action === ACTION.KAKAN) && tilesOut.length !== 1) {
      this.hint(`${action} 需要恰好 1 张牌`);
      return false;
    }
    const seq = this.window.seq;
    if (!this.send({ type: FRAME_TYPE.ACTION, action, tiles: tilesOut, seq })) return false;
    this.markPending(actionLabelOf(action));
    return true;
  }

  /**
   * 立直两段式（§12.4 / v1.21）：
   *   已选中合法宣言牌 → 直接宣言；否则进入宣言模式，等玩家点候选牌。
   * 宣言与打牌是同一条 action 帧，绝不拆成两条。
   */
  declareRiichi() {
    if (!this.window || this.window.window !== WINDOW.DISCARD) {
      this.hint('现在不是你的出牌窗口');
      return false;
    }
    if (!this.hasOption(ACTION.RIICHI)) {
      this.hint('当前不能立直');
      return false;
    }
    if (this.pending || this.frozen || this.remainingMs() <= 0) return false;

    const candidates = this.window.riichiTiles;
    const index = this.state.selected;
    const tile = index >= 0 ? this.state.hands[0][index] : null;
    if (tile && (!candidates.length || candidates.includes(normTile(tile)))) {
      return this.sendRiichi(tile);
    }
    if (!candidates.length) {
      // 老服务端不下发 riichi_tiles：退回「先选牌再点立直」的旧路径
      this.hint('立直：先点选一张手牌作为宣言牌');
      return false;
    }
    this.riichiArming = true;
    this.state.riichiArming = true;
    this.state.selected = -1;
    this.state.riichiTiles = candidates;
    this.hint(`立直：点一张可宣言的牌（${candidates.join('、')}）`);
    this.applyButtons();
    this.app.changed();
    return true;
  }

  /** 宣言模式里点手牌：命中候选才发帧，否则只提示（不打扰服务端） */
  commitRiichiAt(index) {
    if (!this.riichiArming || !this.window) return false;
    const tile = this.state.hands[0][index];
    if (!tile) return false;
    const candidates = this.window.riichiTiles;
    if (candidates.length && !candidates.includes(normTile(tile))) {
      this.hint(`这张打完不听牌，换一张（${candidates.join('、')}）`);
      return false;
    }
    return this.sendRiichi(tile);
  }

  sendRiichi(tile) {
    if (this.pending || this.frozen || this.remainingMs() <= 0) return false;
    const seq = this.window.seq;
    // 宣言牌发手上那张物理牌（可能是赤 0m）：服务端按牌面裁决，牌河要显示真正离手的那张
    if (!this.send({ type: FRAME_TYPE.ACTION, action: ACTION.RIICHI, tiles: [tile], seq })) return false;
    this.markPending(`立直 ${tile}`);
    return true;
  }

  /** 提交成功后的统一收口（参考端 mark_pending）：挂起 + 清选中，等服务端确认 */
  markPending(label) {
    this.pending = true;
    this.pendingLabel = label;
    this.riichiArming = false;
    this.state.riichiArming = false;
    this.state.selected = -1;
    this.applyButtons();
    this.app.changed();
  }

  cancelRiichiArming() {
    if (!this.riichiArming) return false;
    this.riichiArming = false;
    this.state.riichiArming = false;
    this.hint('已取消立直宣言');
    this.app.changed();
    return true;
  }

  send(frame) {
    // §7 RATE_LIMITED：本地短暂锁发送，避免风暴期间继续堆帧
    if (performance.now() < this.rateLimitedUntil) {
      this.hint('操作过于频繁，请稍候');
      return false;
    }
    if (!this.net.send(frame)) {
      this.hint('未连接，发送失败');
      return false;
    }
    return true;
  }

  // ---------------------------------------------------------------- 帧分发

  handleFrame(frame) {
    switch (frame.type) {
      case FRAME_TYPE.LOGIN_OK: {
        this.playerId = frame.player_id;
        this.name = frame.name || this.name;
        // `u-` 前缀 = 账号会话（ACCOUNT_DESIGN §1.2）：只有账号才有战绩
        this.hasAccount = typeof frame.player_id === 'string' && frame.player_id.startsWith('u-');
        this.loginTimeouts = 0;
        this.alreadyOnlineRetries = 0;
        // 注册成功后同一个凭据再重连必须是 login，不能重放 register（否则 USERNAME_TAKEN）。
        if (this.credential && this.credential.register) this.credential.register = false;
        const inRoom = frame.in_room === true;
        // §4.8：心跳必须在 login_ok 之后才开始，否则可能被判 NOT_LOGGED_IN
        this.net.startHeartbeat();
        this.hint(`已登录：${this.name}${this.hasAccount ? '（账号）' : ''}`);
        this.app.emit('session', { stage: inRoom ? 'resuming' : 'lobby' });
        this.app.emit('login_ok', {
          name: this.name, hasAccount: this.hasAccount, playerId: this.playerId, inRoom,
        });
        if (this.hasAccount) this.send({ type: FRAME_TYPE.STATS_REQUEST });
        if (inRoom) {
          // PROTOCOL v2.2 §5.1：服务端马上会重发 room_joined + game_start/state_sync 快照。
          // 这里清空本地残留，避免慢帧被旧 seq/旧窗口判过期；不自动建房、不自动 ready。
          this.resumedInRoom = true;
          this.restoring = true;
          this.autoReady = false;
          this.room = null;
          this.state.roomId = null;
          this.window = null;
          this.lastSeq = null;
          this.pending = false;
          this.frozen = false;
          this.riichiArming = false;
          this.roomRequested = false;
          this.readySent = false;
          this.stage = 'resuming';
          this.stageSince = performance.now();
          if (this._restoreTimer) clearTimeout(this._restoreTimer);
          this._restoreTimer = setTimeout(() => {
            this._restoreTimer = 0;
            this.restoring = false;
          }, 1500);
          this.resumeNext = true;
          this.hint('已登录，正在恢复对局…');
          break;
        }
        this.resumedInRoom = false;
        this.restoring = false;
        this.resumeNext = true;
        this.stage = 'lobby';
        this.stageSince = performance.now();
        // 大厅路径：停在 hall 等玩家选玩法；URL 一步到位：直接建房
        if (this.lobbyDriven) this.listRooms();
        else this.createRoom();
        break;
      }

      case FRAME_TYPE.ROOM_JOINED:
        this.room = {
          roomId: frame.room_id,
          seat: frame.seat,
          seats: frame.seats || [],
          mode: this.config.mode || 'classic',
        };
        // 恢复模式：服务端已经开局，客户端绝不能因为 autoReady 再发 ready()。
        if (this.resumedInRoom) this.autoReady = false;
        // 入座即确定视角：join_room 拿到 1/2/3 号位时，名牌与 viewSeat 都要立刻正确
        if (Number.isInteger(frame.seat)) this.state.mySeat = frame.seat;
        this.state.roomId = typeof frame.room_id === 'string' ? frame.room_id : null;
        // 大厅路径要停在等待页等人，不能进入「等 game_start」的 10 s 超时：
        // 超时后 stage 掉到 idle，listRooms() 的门禁会连兜底轮询一起掐掉。
        this.stage = this.autoReady ? 'start' : 'waiting';
        this.stageSince = performance.now();
        this.applySeatNames();
        this.app.emit('room_joined', this.room);
        // §4.4：ready 是开局信号。URL 一步到位直接开始；大厅路径等玩家点「开始对局」
        if (this.autoReady) {
          this.hint(`房间 ${frame.room_id} · ${frame.seat} 号位，开始对局`);
          this.ready();
        } else {
          this.hint(`已进入房间 ${frame.room_id}，等玩家加入或直接开始`);
        }
        break;

      case FRAME_TYPE.ROOM_UPDATE:
        // PROTOCOL §5.17：未开局房间的座位快照，服务端广播给房内所有真人。
        // `room_list` 只有人数没有名单，等待页必须靠这一帧同步他人入座/离开。
        if (typeof frame.room_id !== 'string') break;
        if (this.room && this.room.roomId && this.room.roomId !== frame.room_id) break;
        this.room = this.room || {};
        this.room.roomId = frame.room_id;
        this.room.seats = Array.isArray(frame.seats) ? frame.seats : [];
        this.app.emit('room_update', this.room);
        break;

      case FRAME_TYPE.GAME_START:
        this.room = this.room || {};
        this.room.seat = frame.seat ?? this.room.seat ?? 0;
        this.clearWindow(true);
        this.frozen = false;
        this.riichiArming = false;
        this.stage = 'idle';
        this.readySent = false;
        this.roomRequested = false;
        this._emptyHandSynced = false;
        this._zeroSince = 0;
        this._zeroWarned = false;
        this.app.startRound(frame);
        this.applySeatNames();
        this.handshakeDone = true;
        // 新一局：结算页必须立刻收掉（局间没有「继续」按钮，服务端自动发牌）
        this.ui?.hideResult?.();
        this.app.emit('game_start', frame);
        const modeText = this.state.mode === 'riichi' ? '日麻' : '经典';
        if (!this.restoring) {
          this.ui?.broadcast?.(this.state.round, `${modeText} · 共 ${this.state.roundCount} 局`, 1200);
        }
        // 参考端兜底：进了牌桌但手牌为空（丢帧/重连）⇒ 立刻要一份权威状态
        this.maybeSyncEmptyHand();
        break;

      case FRAME_TYPE.DRAW:
        if (!this.acceptSeq(frame)) break;
        this.app.drawTile(frame);
        break;

      case FRAME_TYPE.DISCARD_EVENT:
        if (!this.acceptSeq(frame)) break;
        this.app.discardTile(frame);
        if (frame.seat === this.state.mySeat) {
          this.pending = false;
          this.pendingLabel = '';
          this.clearWindow();
        }
        break;

      case FRAME_TYPE.ACTION_REQUEST:
        this.openWindow(frame);
        break;

      case FRAME_TYPE.ACTION_EVENT:
        if (!this.acceptSeq(frame)) break;
        this.pending = false;
        this.pendingLabel = '';
        this.app.applyActionEvent(frame);
        if (frame.action === ACTION.WIN) this.frozen = true;
        if (frame.seat === this.state.mySeat || frame.action === ACTION.WIN) this.clearWindow();
        this.announceAction(frame);
        break;

      case FRAME_TYPE.STATE_SYNC:
        if (!this.acceptSeq(frame)) break;
        this.app.applySync(frame);
        break;

      case FRAME_TYPE.TENPAI:
        this.app.applyTenpai(frame);
        console.debug('tenpai', this.state.tenpai);
        break;

      case FRAME_TYPE.GAME_END:
        this.clearWindow(true);
        this.frozen = true;
        this.stage = 'idle';
        this.app.applyGameEnd(frame);
        this.logGameEnd(frame);
        if (!this.restoring) this.playEndVoice(frame);
        this.app.emit('game_end', frame);
        // 先让「和 / 流局」播报落地，再弹结算；期间若已开下一局则不再弹（守 phase）
        setTimeout(() => {
          if (this.state.phase === 'ended') this.ui?.showResult?.(frame, this.state);
        }, 900);
        // 本局刚落库（ACCOUNT_DESIGN §4.1）：账号会话补拉一次累计战绩
        if (this.hasAccount) this.send({ type: FRAME_TYPE.STATS_REQUEST });
        break;

      case FRAME_TYPE.MATCH_END:
        this.clearWindow(true);
        this.frozen = true;
        this.stage = 'idle';
        this.app.applyMatchEnd?.(frame);
        this.app.emit('match_end', frame);
        this.ui?.showFinal?.(this.state);
        console.info('[场终]', this.describeRankings(frame));
        break;

      case FRAME_TYPE.STATS:
        this.stats = frame;
        this.state.stats = frame;
        console.info('[战绩]', frame);
        this.app.emit('stats', frame);
        this.app.changed();
        break;

      case FRAME_TYPE.ROOM_LIST:
        this.rooms = Array.isArray(frame.rooms) ? frame.rooms : [];
        this.state.rooms = this.rooms;
        this.app.emit('rooms', this.rooms);
        this.app.changed();
        console.debug('[房间列表]', this.rooms.length, '间');
        break;

      case FRAME_TYPE.ERROR:
        this.handleError(frame);
        break;

      case FRAME_TYPE.PONG:
        break;

      default:
        // 未知帧类型：忽略并记日志（§1.3 前向兼容）
        console.debug('忽略未知帧', frame.type);
    }
  }

  /**
   * §6.2：seq 只认严格更大。
   * 返回 false = 这一帧比已见状态旧（H9 乱序），丢弃；同时把已过期的窗口关掉。
   */
  acceptSeq(frame) {
    const seq = frame.seq;
    if (!Number.isInteger(seq)) return true;
    if (this.lastSeq !== null && seq < this.lastSeq) {
      console.debug('丢弃过期帧', frame.type, seq, '<', this.lastSeq);
      return false;
    }
    this.lastSeq = seq;
    if (this.window && seq > this.window.seq) this.clearWindow(true);
    return true;
  }

  /**
   * §7 错误码分支（对齐参考端 `_on_error`）：
   *  - 非法类错误本地同步计数（服务端第 10 次断开，提示到 10）；
   *  - 会破坏本地状态的错误补拉一次 `sync`（1 s 限流）；
   *  - RATE_LIMITED 短暂锁发送；FRAME_TOO_LARGE / NOT_LOGGED_IN 直接断开。
   */
  handleError(frame) {
    const code = Number.isInteger(frame.code) ? frame.code : ERROR_CODE.INTERNAL;
    const message = typeof frame.message === 'string' ? frame.message : '';
    // PROTOCOL v2.2 §4.2：一账号一连接。刷新时旧连接可能还没被服务端判定关闭，账号登录先撞
    // ALREADY_ONLINE；在同一连接上短重试 login，等服务端释放旧账号。绝不接管、不建第二连接。
    if (code === ERROR_CODE.ALREADY_ONLINE && this.stage === 'login' && this.credential?.user &&
        !this.credential.register && this.alreadyOnlineRetries < 6) {
      this.alreadyOnlineRetries += 1;
      this.stageSince = performance.now();
      this.hint(`正在释放旧连接（${this.alreadyOnlineRetries}/6）…`);
      this.app.emit('session', { stage: 'login' });
      setTimeout(() => {
        if (this.net.connected && this.stage === 'login') this.send(this.authFrame());
      }, 500);
      return;
    }
    const rejected = this.pending;
    this.pending = false;
    this.pendingLabel = '';
    this.applyButtons();

    const counted = [ERROR_CODE.ILLEGAL_ACTION, ERROR_CODE.NOT_YOUR_TURN, ERROR_CODE.NO_PENDING_ACTION, ERROR_CODE.UNKNOWN_TILE, ERROR_CODE.AUTH_FAILED];
    if (counted.includes(code)) this.illegalCount += 1;

    let text = `${code}: ${message}`;
    // message 是服务端自由文案：'riichi' 与下面的 'already in a room' 是既有的服务端文案耦合，保留
    if (code === ERROR_CODE.ILLEGAL_ACTION && message.includes('riichi')) {
      // 立直被拒最常见的原因是「这张牌打完不听牌」，中文说清下一步
      text = '立直失败：这张牌打出后不听牌，换一张再点立直';
      this.state.selected = -1;
    } else if (code === ERROR_CODE.RATE_LIMITED) {
      this.rateLimitedUntil = performance.now() + 1000;
      text = '操作过于频繁，请稍候';
    } else if (code === ERROR_CODE.FRAME_TOO_LARGE) {
      text = '发送帧过大，已断开连接';
      this.net.close('发送帧过大');
    } else if (code === ERROR_CODE.NOT_LOGGED_IN) {
      text = '会话未建立，请刷新页面重新登录';
      this.net.close('会话未建立');
    } else if (code === ERROR_CODE.ROOM_NOT_FOUND) {
      this.room = null;
      this.state.roomId = null;
      this.readySent = false;
      this.roomRequested = false;
      text = '房间不存在或已关闭';
    } else if (code === ERROR_CODE.INTERNAL) {
      text = `服务端内部错误：${message || '未知原因'}`;
    } else if (code === ERROR_CODE.AUTH_FAILED) {
      text = '账号或密码错误';
    } else if (code === ERROR_CODE.USERNAME_TAKEN) {
      text = '账号名已被占用，换一个再注册';
    } else if (code === ERROR_CODE.ALREADY_ONLINE) {
      text = this.alreadyOnlineRetries >= 6
        ? '该账号仍在另一连接上，请关闭其他页面后重试'
        : '该账号已在另一处登录';
    } else if (code === ERROR_CODE.INVALID_NAME) {
      text = '昵称非法：需 1~16 个字符，且不含换行或控制字符';
    }
    if (this.illegalCount >= ILLEGAL_LIMIT) {
      text += `（非法操作 ${this.illegalCount}/${ILLEGAL_LIMIT}，将被断开）`;
    } else if (this.illegalCount > 0) {
      text += `（非法操作 ${this.illegalCount}/${ILLEGAL_LIMIT}）`;
    }

    console.warn('服务端错误', frame);
    this.state.lastError = { code, message };
    if ([ERROR_CODE.ILLEGAL_ACTION, ERROR_CODE.NOT_YOUR_TURN, ERROR_CODE.NO_PENDING_ACTION, ERROR_CODE.UNKNOWN_TILE].includes(code)) {
      // 参考端 clear_pending：被拒的动作不再挂着，选中与立直宣言模式一并清掉
      this.riichiArming = false;
      this.state.selected = -1;
      this.applyButtons();
    }
    this.hint(text);
    this.app.emit('error', { code, message, text });
    this.app.changed();

    // 被拒后本地可能已与服务端不一致：这几类错误必须拉权威状态
    const resync = [ERROR_CODE.BAD_REQUEST, ERROR_CODE.NOT_YOUR_TURN, ERROR_CODE.NO_PENDING_ACTION, ERROR_CODE.UNKNOWN_TILE];
    if (code === ERROR_CODE.BAD_REQUEST || (rejected && resync.includes(code))) this.requestSync();
    // 「already in a room」= 旧房间还没被回收（场终后 30 秒）：换一条连接回大厅最稳
    // message.includes('already in a room') 是既有的服务端文案耦合，保留
    if (code === ERROR_CODE.BAD_REQUEST && message.includes('already in a room') && this.lobbyDriven) {
      this.returnToLobby();
    }
  }

  /** 补偿同步（1 s 限流）：错误恢复 / 本地手牌对不上时用 */
  requestSync() {
    const now = performance.now();
    if (now - this._lastSyncAt < 1000) return false;
    this._lastSyncAt = now;
    return this.send({ type: FRAME_TYPE.SYNC });
  }

  /** 牌桌里手牌为空（丢帧 / 重连）⇒ 只补一次 sync（参考端 `_empty_hand_synced`） */
  maybeSyncEmptyHand() {
    if (this.state.phase !== 'playing' || this.state.hands[0].length) return;
    if (this._emptyHandSynced) return;
    if (this.requestSync()) this._emptyHandSynced = true;
  }

  /** 大厅可主动刷新房间列表（§4.11）；结果落在 client.rooms / state.rooms */
  listRooms() {
    if (!this.net.connected) return false;
    // 大厅、等待页（waiting）、已点开始（start）都要刷：等待页靠它兜底人数
    if (!['lobby', 'waiting', 'start', 'ready'].includes(this.stage)) return false;
    return this.send({ type: FRAME_TYPE.LIST_ROOMS });
  }

  /** 主动查战绩（§4.10）；游客会拿到零值载荷，不是错误 */
  requestStats() {
    return this.send({ type: FRAME_TYPE.STATS_REQUEST });
  }

  /** 开局信号（§4.4）：重复调用不会再发（服务端会回 BAD_REQUEST） */
  ready() {
    if (this.readySent) return false;
    this.readySent = true;
    // 只有发出 ready 之后才开始计「等 game_start」的 10 s 超时（§8.6）
    this.stage = 'start';
    this.stageSince = performance.now();
    return this.send({ type: FRAME_TYPE.READY });
  }

  /** 建房（大厅点「创建房间并开始」）：mode/length 由大厅传入，缺省用 URL/默认值 */
  createRoom(options = {}) {
    if (this.roomRequested) return false;      // 重复的 login_ok 不得重复建房（会 BAD_REQUEST）
    if (options.mode) this.config.mode = options.mode === 'riichi' ? 'riichi' : 'classic';
    if (options.length) this.config.length = options.length === 'hanchan' ? 'hanchan' : 'tonpu';
    this.roomRequested = true;
    this.stage = 'room';
    this.stageSince = performance.now();
    if (this.config.room) {
      return this.send({ type: FRAME_TYPE.JOIN_ROOM, room_id: this.config.room });
    }
    // config.mode/length 是 URL/大厅的 UI 字符串；仅在构造线帧时翻译成 v2.0 整数
    const mode = this.config.mode === 'riichi' ? MODE.RIICHI : MODE.CLASSIC;
    const frame = { type: FRAME_TYPE.CREATE_ROOM, mode };
    // 半庄只对日麻有效（§12.1：classic 传 hanchan 回 BAD_REQUEST）
    if (mode === MODE.RIICHI && this.config.length === 'hanchan') frame.length = LENGTH.HANCHAN;
    return this.send(frame);
  }

  /** 加入大厅列表里的房间（§4.3） */
  joinRoom(roomId) {
    if (this.roomRequested) return false;
    if (typeof roomId !== 'string' || !roomId) return false;
    this.roomRequested = true;
    this.stage = 'room';
    this.stageSince = performance.now();
    return this.send({ type: FRAME_TYPE.JOIN_ROOM, room_id: roomId });
  }

  /** 退出登录：断开连接、清空本局状态，回到登录页 */
  logout() {
    this.credential = null;
    this.room = null;
    this.stats = null;
    this.rooms = [];
    this.roomRequested = false;
    this.readySent = false;
    this.window = null;
    this.lastSeq = null;
    this.pending = false;
    this.stage = 'idle';
    this.resumeNext = true;
    this.restoring = false;
    this.resumedInRoom = false;
    this.alreadyOnlineRetries = 0;
    if (this._restoreTimer) {
      clearTimeout(this._restoreTimer);
      this._restoreTimer = 0;
    }
    this.state.roomId = null;
    this._reconnecting = true;      // 主动断开：不弹「连接已断开」
    setTimeout(() => { this._reconnecting = false; }, 800);
    this.app.reset();
    this.net.close('退出登录');
    this.app.emit('session', { stage: 'idle' });
  }

  /** 场终后回大厅：换一条连接（旧房间要等 30 秒回收，同连接再建房会被拒） */
  returnToLobby() {
    if (!this.credential) return;
    this.lobbyDriven = true;
    this._reconnecting = true;
    // PROTOCOL v2.2 §4.1：恢复同一账号，但明确不要回到旧房间；由服务端先把旧座位转托管。
    this.resumeNext = false;
    this.restoring = false;
    this.resumedInRoom = false;
    if (this._restoreTimer) {
      clearTimeout(this._restoreTimer);
      this._restoreTimer = 0;
    }
    this.room = null;
    this.window = null;
    this.lastSeq = null;
    this.pending = false;
    this.roomRequested = false;
    this.readySent = false;
    this.frozen = false;
    this.app.reset();
    this.net.close('返回大厅');
    this.connectWithRetry()
      .then(() => { this.app.emit('session', { stage: this.stage }); })
      .catch((err) => {
        this._reconnecting = false;
        this.resumeNext = true;
        this.hint(`无法连接服务器：${err.message}`);
        this.app.emit('session', { stage: 'closed' });
      });
  }

  // ---------------------------------------------------------------- 窗口

  openWindow(frame) {
    const seq = frame.seq;
    if (!Number.isInteger(seq)) {
      console.warn('action_request.seq 非法，已忽略', frame);
      return;
    }
    if (!WINDOW_TYPES.has(frame.window)) {
      console.warn('action_request.window 非法，已忽略', frame.window);
      return;
    }
    if (this.lastSeq !== null && seq < this.lastSeq) {
      console.warn('收到过期 action_request', seq, '<', this.lastSeq);
      return;
    }

    const options = (Array.isArray(frame.options) ? frame.options : [])
      .filter((item) => item && Number.isInteger(item.action))
      .map((item) => ({ action: item.action, tiles: Array.isArray(item.tiles) ? [...item.tiles] : [] }));

    // §6.2：显示用剩余 = min(服务端剩余, 本地兜底)，本地兜底窗口夹在 [1s, 60s]
    const nowEpoch = Date.now();
    const deadline = Number.isFinite(frame.deadline_ms) ? frame.deadline_ms : nowEpoch;
    const serverRemaining = deadline - nowEpoch;
    const guardMs = Math.min(Math.max(serverRemaining, 1000), 60000);
    const remaining = Math.max(0, Math.min(serverRemaining, guardMs));

    const riichiTiles = [];
    for (const code of Array.isArray(frame.riichi_tiles) ? frame.riichi_tiles : []) {
      if (!isValidTile(code)) {
        console.warn('riichi_tiles 含非法牌码，已忽略', code);
        continue;
      }
      const face = normTile(code);
      if (!riichiTiles.includes(face)) riichiTiles.push(face);
    }

    // §12.10：抢杠窗口必须带被抢的那张牌（它不在任何人的牌河里）
    let windowTile = null;
    if (frame.window === WINDOW.CHANKAN) {
      if (isValidTile(frame.tile)) windowTile = frame.tile;
      else console.warn('chankan 窗口缺少合法 tile', frame.tile);
    }

    this.lastSeq = seq;
    this.window = {
      seq,
      window: frame.window,
      options,
      riichiTiles,
      tile: windowTile,
      totalMs: remaining,          // 倒计时环的分母（服务端剩余秒）
      remainingMs: remaining,
      expiresAt: performance.now() + remaining,
    };
    this.pending = false;
    this.pendingLabel = '';
    this.riichiArming = false;
    this.state.riichiArming = false;
    this.expired = false;
    this.state.riichiTiles = riichiTiles;
    this.state.windowTile = this.window.tile;
    // 鸣牌时在牌河里点亮被指的那张。打出者以 lastDiscard 为准：
    // 只有吃限定上家；碰/和可鸣任何一家刚打出的牌，随局轮转。
    const ld = this.state.lastDiscard;
    this.state.claimTarget = frame.window === WINDOW.CLAIM && ld
      ? { seat: ld.seat, index: (this.state.rivers[ld.seat] || []).length - 1 }
      : null;
    this.state.selected = -1;          // 新窗口不继承上一手的选中（参考端同口径）
    this.applyButtons();
    this.app.changed();

    this.showClaimTip(frame, options);
    if (frame.window === WINDOW.CHANKAN) {
      this.hint(`抢杠 ${this.window.tile || ''}：可以和或过`);
    }
  }

  /**
   * 鸣牌提示：这次到底吃/碰/杠/和的是**哪一张**。
   *
   * claim 窗口的猎物恒为「上一家最近一次打出」（§4.6 规则 2），而 state_sync 可能
   * 已经先到、把牌河刷过了，所以优先用窗口自带的 tile，其次退回牌河末张。
   */
  showClaimTip(frame, options) {
    if (!this.ui?.claimTip) return;
    const actions = options.map((item) => item.action);
    if (frame.window === WINDOW.DISCARD) { this.ui.hideClaimTip?.(); return; }

    let tile = isValidTile(frame.tile) ? frame.tile : null;
    if (!tile) {
      // 打出者随局轮转（碰/和可鸣任何一家），以 lastDiscard 为准；
      // claim 的猎物就是它牌河的末张
      const ld = this.state.lastDiscard;
      const river = ld ? this.state.rivers?.[ld.seat] || [] : [];
      if (river.length) tile = river[river.length - 1];
    }
    if (!tile) { this.ui.hideClaimTip?.(); return; }

    const chows = options.filter((item) => item.action === ACTION.CHOW).length;
    let note = '';
    if (frame.window === WINDOW.CHANKAN) note = '抢杠';
    else if (chows > 1) note = `${chows} 种吃法`;
    this.ui.claimTip(actions, tile, note);
  }

  /** releasePending：窗口被更新帧推进/局终时，本地挂起的提交也随之作废（§6.3 静默丢弃） */
  clearWindow(releasePending = false) {
    this.window = null;
    this.state.riichiTiles = [];
    this.state.windowTile = null;
    this.state.claimTarget = null;
    this.ui?.hideClaimTip?.();
    this.riichiArming = false;
    this.state.riichiArming = false;
    this.expired = false;
    if (releasePending) {
      this.pending = false;
      this.pendingLabel = '';
    }
    this.applyButtons();
  }

  tick() {
    const now = performance.now();
    this.checkStages(now);
    this.checkSilence(now);
    // 本地已知对不上（例如本家 discard_event 的牌不在手牌里）：补拉一次权威状态
    if (this.state.needsSync && this.requestSync()) this.state.needsSync = false;
    this.maybeSyncEmptyHand();
    this.ui?.tick?.(this);

    if (!this.window) {
      this._zeroSince = 0;
      this._zeroWarned = false;
      return;
    }
    const remaining = this.remainingMs();
    this.window.remainingMs = remaining;
    const second = Math.ceil(remaining / 1000);
    if (second !== this._lastTickSecond) {
      this._lastTickSecond = second;
    }
    if (remaining > 0) {
      this._zeroSince = 0;
      this._zeroWarned = false;
      return;
    }
    if (!this.expired) {
      this.expired = true;
      this.applyButtons();
    }
    // 归零后服务端迟迟不推进 ⇒ 升一级提示（参考端 _window_watchdog）
    if (!this._zeroSince) {
      this._zeroSince = now;
    } else if (!this._zeroWarned && now - this._zeroSince > ZERO_WINDOW_MS) {
      this._zeroWarned = true;
      this.hint('服务器无响应（5 秒无状态更新）');
      this.requestSync();
    }
  }

  /** 阶段超时：login_ok / room_joined / game_start（参考端 _stage_timeouts） */
  checkStages(now) {
    if (this.stage === 'login' && now - this.stageSince > LOGIN_TIMEOUT_MS) {
      this.stage = 'idle';
      this.loginTimeouts += 1;
      if (this.loginTimeouts > 3) {
        this.net.close('login_ok 超时');
        this.hint('服务器无响应（login_ok 连续 3 次超时），请确认服务端已启动');
        return;
      }
      this.hint(`服务器无响应（未返回 login_ok），正在重连（${this.loginTimeouts}/3）…`);
      this.connectAttempts = 0;
      this._reconnecting = true;
      this.net.close('login_ok 超时');
      this.connectWithRetry().catch((err) => {
        this._reconnecting = false;
        this.hint(`重连失败：${err.message}`);
      });
      return;
    }
    if (this.stage === 'room' && now - this.stageSince > ROOM_TIMEOUT_MS) {
      this.stage = 'idle';
      this.hint('等待 room_joined 超时（3 秒），可刷新页面重试');
      return;
    }
    if (this.stage === 'resuming' && now - this.stageSince > 5000) {
      // 服务端承诺 in_room:true 后马上重发快照；5 秒还没到说明房间已被回收/异常，
      // 退回大厅，避免卡在"正在恢复"。
      this.stage = 'lobby';
      this.stageSince = now;
      this.restoring = false;
      this.resumedInRoom = false;
      this.hint('恢复对局超时，已回到大厅');
      this.app.emit('session', { stage: 'lobby' });
      this.listRooms();
      return;
    }
    if (this.stage === 'start' && now - this.stageSince > START_TIMEOUT_MS) {
      this.stage = 'idle';
      this.hint('等待开局超时（10 秒）');
    }
  }

  /** §8.2 / §8.4：入站静默 16 s 提示，20 s 按断线处理 */
  checkSilence(now) {
    if (!this.net.connected || this.stage === 'login') return;
    const last = this.net.lastInbound;
    if (!last) return;
    const silence = now - last;
    if (silence > SILENCE_WARN_MS && !this._silenceWarned) {
      this._silenceWarned = true;
      this.hint('连接不稳定（心跳无响应）');
    } else if (silence <= SILENCE_WARN_MS) {
      this._silenceWarned = false;
    }
    if (silence > SILENCE_LIMIT_MS) {
      this.hint('网络无响应（20 秒），已断开');
      this.frozen = true;
      this.net.close('网络无响应（20 秒）');
      this.applyButtons();
    }
  }

  // ---------------------------------------------------------------- 其它

  applySeatNames() {
    const names = [...FALLBACK_NAMES];
    for (const seat of this.room?.seats || []) {
      const view = this.app.viewSeat(seat.seat);
      names[view] = seat.name || names[view];
      this.state.isBot[view] = !!seat.is_bot;
    }
    this.state.seatNames = names;
    // 名牌是既有 DOM，只改文案（不动结构与样式）
    names.forEach((name, view) => {
      const el = document.querySelector(`#seat-${view} .seat-name`);
      if (el) el.textContent = name;
    });
  }

  bindStore() {
    // ui.update 由 main.js 的渲染回路负责，这里只同步按钮可用性（注册在后 ⇒ 覆盖 ui.update）
    this.app.on('change', () => this.applyButtons());
  }

  logGameEnd(frame) {
    const rows = (frame.results || []).map((result) => {
      const view = this.app.viewSeat(result.seat);
      return `${this.state.seatNames[view]} ${result.delta_score > 0 ? '+' : ''}${result.delta_score}`;
    });
    const win = frame.win;
    const detail = win
      ? `${this.state.seatNames[this.app.viewSeat(win.seat)]} ${win.tsumo ? '自摸' : '荣和'} `
        + `${win.tile || ''} ${(win.yaku || [])
          .map((item) => YAKU_NAMES[yakuName(item.id)] || yakuName(item.id)).join('·')}`
        + (win.han != null ? ` ${win.han}番` : '')
      : '荒庄';
    this.hint(`[局终] ${detail} ｜ ${rows.join(' / ')}`);
  }

  /**
   * 局终语音播报（voice.js 口径 = pygame broadcast.py 日麻侧）：
   * 顺序念「和牌方式 → 番种 → 合计番数」，横幅同步展示结论，时长随语音。
   */
  playEndVoice(frame) {
    const win = frame && typeof frame === 'object' ? frame.win : null;
    const { cues } = buildVoiceCues(frame);
    let caption;
    if (win) {
      const name = this.state.seatNames[this.app.viewSeat(win.seat)] || '';
      const yaku = (win.yaku || [])
        .map((item) => YAKU_NAMES[yakuName(item.id)] || yakuName(item.id))
        .join('·');
      const parts = [
        `${name} ${win.tsumo ? '自摸' : '荣和'}${win.tile ? ` ${win.tile}` : ''}`,
        yaku,
        Number.isInteger(win.han) ? `合计 ${win.han} 番` : '',
      ].filter(Boolean);
      caption = parts.join(' · ');
    } else {
      caption = '荒庄流局';
    }
    this.sfx?.speak?.(cues);
    // 横幅窗口 = 语音总时长 + 余量，至少 2.4s；「和/流」大字沿用副露播报样式
    this.ui?.broadcast?.(win ? '和' : '流局', caption, Math.max(2400, voiceDuration(cues) + 800));
  }

  describeRankings(frame) {
    return (frame.rankings || [])
      .map((row) => `${row.rank}. ${this.state.seatNames[this.app.viewSeat(row.seat)]} ${row.final_score}`)
      .join('  ');
  }

  /** 没有新增界面：提示写到日志条、控制台，并在牌桌上弹一条轻提示 */
  hint(text) {
    const el = document.getElementById('log-text');
    if (el) el.textContent = text;
    console.info('[麻将]', text);
    this.app.emit('hint', text);
    this.ui?.toast?.(text);
  }

  /** 副露 / 立直 / 和牌的中央播报（§5.8 action_event 即权威事件源） */
  announceAction(frame) {
    const labels = {
      [ACTION.PONG]: '碰', [ACTION.CHOW]: '吃', [ACTION.KONG]: '杠', [ACTION.ANKAN]: '暗杠',
      [ACTION.KAKAN]: '加杠', [ACTION.RIICHI]: '立直', [ACTION.WIN]: '和',
    };
    const label = labels[frame.action];
    if (!label) return;
    const view = this.app.viewSeat(frame.seat);
    const name = this.state.seatNames[view] || '';
    this.ui?.broadcast?.(label, name, 820);
    if (frame.action === ACTION.RIICHI) this.sfx?.play?.('click');
  }
}

function actionLabelOf(action) {
  return {
    [ACTION.PONG]: '碰', [ACTION.CHOW]: '吃', [ACTION.KONG]: '杠', [ACTION.ANKAN]: '暗杠',
    [ACTION.KAKAN]: '加杠', [ACTION.WIN]: '和牌', [ACTION.PASS]: '过', [ACTION.RIICHI]: '立直',
  }[action] || action;
}

/**
 * §4.1 昵称校验的客户端前置：1..16 字符、不含 `\n`/`\r`/控制字符。
 * 不做这一步会直接吃到 INVALID_NAME，累计 4 次还会被服务端断开。
 */
function sanitizeName(raw) {
  if (typeof raw !== 'string') return '';
  return raw
    .replace(/[\u0000-\u001f\u007f]/g, '')
    .trim()
    .slice(0, NAME_MAX);
}
