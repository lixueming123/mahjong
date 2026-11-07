/**
 * 牌桌 HUD（纯 DOM 覆盖层，不含登录/大厅）。
 *
 * 职责：场况卡、四家名牌、听牌条、出牌倒计时、播报、轻提示、局终/场终结算。
 * 数据只从 state 读（服务端权威），倒计时从 client 的窗口读（唯一有 deadline 的地方）。
 *
 * 坐标按 1920x1080 设计像素写在 CSS 里，这里只改文案与显隐。
 */

const $ = (id) => document.getElementById(id);

const WIND_CHARS = ['東', '南', '西', '北'];
const HONOR_LABELS = { 1: '东', 2: '南', 3: '西', 4: '北', 5: '白', 6: '发', 7: '中' };
const ROUND_LABELS = { win_ron: '荣和', win_tsumo: '自摸', draw_exhausted: '荒庄流局' };
const ACTION_LABELS = {
  pong: '碰', chow: '吃', kong: '杠', ankan: '暗杠', kakan: '加杠',
  riichi: '立直', win: '和', pass: '过',
};
const YAKU_LABELS = {
  menzen: '门前清', tanyao: '断幺九', pinfu: '平和', iipeikou: '一杯口', yakuhai: '箭刻',
  rinshan: '岭上开花', sanshoku_doujun: '三色同顺', ittsuu: '一气通贯', toitoi: '对对和',
  sanankou: '三暗刻', honitsu: '混一色', shousangen: '小三元', seven_pairs: '七对子',
  pure_one_suit: '清一色', riichi: '立直', ippatsu: '一发', chankan: '抢杠',
  tenhou: '天和', chiihou: '地和', kokushi_13: '国士十三面', suuankou_tanki: '四暗刻单骑',
  chuuren_junsei: '纯正九莲宝灯', daisuushii: '大四喜', aka_dora: '赤宝牌',
};

/** §2.1 牌码 → 中文面（用于听牌条 / 立直候选这类小标签） */
export function tileLabel(code) {
  if (typeof code !== 'string' || code.length !== 2) return code || '';
  const [rank, suit] = code;
  if (suit === 'z') return HONOR_LABELS[rank] || code;
  const suitLabel = { m: '万', p: '筒', s: '索' }[suit] || suit;
  return `${rank === '0' ? '赤5' : rank}${suitLabel}`;
}

const esc = (text) => String(text ?? '').replace(/[&<>"']/g, (c) => (
  { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
));

const fmt = (value) => (typeof value === 'number' ? value.toLocaleString('en-US') : '—');
const signed = (value) => (typeof value !== 'number' ? '—' : `${value > 0 ? '+' : ''}${value}`);

export class UI {
  constructor(app) {
    this.app = app;
    this.client = null;
    this.lobby = null;
    this.el = {
      roundName: $('round-name'),
      roundMark: $('round-mark'),
      roundTitle: $('round-title'),
      remaining: $('remaining'),
      remainingTop: $('remaining-top'),
      honba: $('honba'),
      log: $('log-text'),
      handCount: $('hand-count'),
      discard: $('btn-discard'),
      draw: $('btn-draw'),
      pass: $('btn-pass'),
      stats: $('stats'),
      scores: [0, 1, 2, 3].map((i) => $(`seat-score-${i}`)),
      winds: [0, 1, 2, 3].map((i) => $(`seat-wind-${i}`)),
      tags: [0, 1, 2, 3].map((i) => $(`seat-tags-${i}`)),
      seats: [0, 1, 2, 3].map((i) => $(`seat-${i}`)),
      toggleSfx: $('btn-sfx'),

      countdown: $('countdown'),
      cdNum: $('cd-num'),
      cdBar: $('cd-bar'),

      tenpai: $('tenpai-bar'),
      tenpaiWaits: $('tp-waits'),
      tenpaiNote: $('tp-note'),

      broadcast: $('broadcast'),
      bcMain: $('bc-main'),
      bcSub: $('bc-sub'),

      toast: $('toast'),

      claimTip: $('claim-tip'),

      result: $('result'),
      rsKind: $('rs-kind'),
      rsTitle: $('rs-title'),
      rsWinner: $('rs-winner'),
      rsRows: $('rs-rows'),
      rsNext: $('rs-next'),
      rsClose: $('rs-close'),
      rsLobby: $('rs-lobby'),
    };

    // 倒计时环周长（r = 43）
    this.cdLength = 2 * Math.PI * 43;
    this._lastCdSecond = -1;
    this._bcTimer = 0;
    this._toastTimer = 0;
    this._resultDeadline = 0;
    this._nextTimer = 0;

    this.el.rsClose?.addEventListener('click', () => this.hideResult());
    this.el.rsLobby?.addEventListener('click', () => {
      this.hideResult();
      this.lobby?.showHall();
      this.client?.returnToLobby();
    });
  }

  attach({ client, lobby }) {
    this.client = client;
    this.lobby = lobby;
  }

  // ------------------------------------------------------------------ 牌桌

  update(state) {
    const el = this.el;
    if (el.roundName) el.roundName.textContent = state.round;
    if (el.roundMark) el.roundMark.textContent = (state.round || '東')[0];
    if (el.roundTitle) {
      const lengthText = this.client?.config?.length === 'hanchan' ? '半庄' : '東風';
      el.roundTitle.textContent = state.mode === 'riichi'
        ? `${lengthText}戦 · 日麻`
        : '東風戦 · 经典';
    }
    if (el.remaining) el.remaining.textContent = state.remaining;
    if (el.remainingTop) el.remainingTop.textContent = state.remaining;
    if (el.honba) el.honba.textContent = state.honba;
    if (el.handCount) el.handCount.textContent = state.hand.length;
    if (el.log) el.log.textContent = state.lastAction || '—';

    // 名牌：分数 / 自风 / 立直、托管标签
    const idle = state.phase === 'idle';
    for (let view = 0; view < 4; view++) {
      const protocolSeat = (state.mySeat + view) % 4;
      const windIndex = ((protocolSeat - state.dealer) % 4 + 4) % 4;
      const score = Array.isArray(state.scores) ? state.scores[view] : null;
      if (el.winds[view]) el.winds[view].textContent = idle ? '' : WIND_CHARS[windIndex];
      if (el.scores[view]) el.scores[view].textContent = idle || score == null ? '' : fmt(score);
      if (el.seats[view]) el.seats[view].classList.toggle('dealer', !idle && protocolSeat === state.dealer);

      const tags = [];
      if (state.riichi?.[view]) tags.push('<span class="seat-tag">立直</span>');
      if (state.disconnected?.[view]) tags.push('<span class="seat-tag off">托管</span>');
      if (state.isBot?.[view] && view !== 0) tags.push('<span class="seat-tag off">AI</span>');
      if (el.tags[view]) el.tags[view].innerHTML = tags.join('');
    }

    this.renderTenpai(state);
  }

  renderTenpai(state) {
    const el = this.el;
    if (!el.tenpai) return;
    const tenpai = state.tenpai || { isTenpai: false, waits: [] };
    const show = state.phase === 'playing' && tenpai.isTenpai;
    el.tenpai.hidden = !show;
    if (!show) return;

    const waits = Array.isArray(tenpai.waits) ? tenpai.waits : [];
    el.tenpaiWaits.innerHTML = waits.map((wait) => {
      const dead = !wait.remaining;
      return `<span class="tp-wait${dead ? ' dead' : ''}">${esc(tileLabel(wait.tile))}`
        + `<i>${dead ? '0' : wait.remaining}</i></span>`;
    }).join('');
    el.tenpaiNote.textContent = tenpai.noYaku ? '无役，和不了' : '';
  }

  /** client.tick() 每 200 ms 调一次：倒计时环 + 结算页「下一局」秒数 */
  tick(client) {
    const win = client?.window;
    const el = this.el;
    if (!el.countdown) return;
    if (!win) {
      if (!el.countdown.hidden) el.countdown.hidden = true;
      this._lastCdSecond = -1;
    } else {
      const remaining = client.remainingMs();
      const total = Math.max(1, win.totalMs || win.remainingMs || 1);
      const second = Math.ceil(remaining / 1000);
      el.countdown.hidden = false;
      el.countdown.classList.toggle('low', remaining <= 5000);
      if (second !== this._lastCdSecond) {
        this._lastCdSecond = second;
        if (el.cdNum) el.cdNum.textContent = Math.max(0, second);
        if (el.cdBar) {
          const ratio = Math.max(0, Math.min(1, remaining / total));
          el.cdBar.style.strokeDasharray = String(this.cdLength);
          el.cdBar.style.strokeDashoffset = String(this.cdLength * (1 - ratio));
        }
      }
    }

    if (this._resultDeadline) {
      const left = Math.max(0, Math.ceil((this._resultDeadline - performance.now()) / 1000));
      if (this._nextTimer) {
        this._nextTimer.textContent = left ? `下一局 ${left} 秒后开始` : '正在发牌…';
      }
      if (!left) this._resultDeadline = 0;
    }
  }

  // ------------------------------------------------------------------ 播报 / 提示

  /** 中央大字播报：立直、碰、吃、杠、和、局终 */
  broadcast(main, sub = '', ms = 1100) {
    const el = this.el;
    if (!el.broadcast) return;
    clearTimeout(this._bcTimer);
    el.broadcast.hidden = false;
    el.broadcast.classList.remove('out');
    el.bcMain.textContent = main;
    el.bcSub.textContent = sub;
    // 重启动画：强制 reflow
    void el.broadcast.offsetWidth;
    this._bcTimer = setTimeout(() => {
      el.broadcast.classList.add('out');
      this._bcTimer = setTimeout(() => { el.broadcast.hidden = true; }, 360);
    }, Math.max(360, ms));
  }

  hideBroadcast() {
    clearTimeout(this._bcTimer);
    if (this.el.broadcast) this.el.broadcast.hidden = true;
  }

  toast(text, kind = '', ms = 2600) {
    const el = this.el.toast;
    if (!el) return;
    clearTimeout(this._toastTimer);
    el.hidden = false;
    el.className = `toast ${kind}`.trim();
    el.textContent = text;
    this._toastTimer = setTimeout(() => { el.hidden = true; }, ms);
  }

  /**
   * 鸣牌提示：这次能吃/碰/杠/和的是**哪一张**。
   * 只给 pass 的时候不显示（没什么可鸣的，别打扰）。
   */
  claimTip(actions, tile, note = '') {
    const el = this.el.claimTip;
    if (!el) return;
    const real = (actions || []).filter((action) => action !== 'pass');
    if (!real.length) { el.hidden = true; return; }
    const order = ['win', 'kong', 'pong', 'chow'];
    real.sort((a, b) => order.indexOf(a) - order.indexOf(b));
    const badges = real.map((action) => {
      const label = ACTION_LABELS[action] || action;
      return `<span class="ct-act${action === 'win' ? ' win' : ''}">${esc(label)}</span>`;
    }).join('');
    el.innerHTML = badges
      + `<span class="ct-tile">${esc(tileLabel(tile))}</span>`
      + (note ? `<span class="ct-note">${esc(note)}</span>` : '');
    el.hidden = false;
  }

  hideClaimTip() {
    if (this.el.claimTip) {
      this.el.claimTip.hidden = true;
      this.el.claimTip.style.pointerEvents = '';
    }
  }

  /**
   * 多种吃法选择条：每种吃法一颗按钮，显示它的三张牌面
   * （服务端 tiles[] 已含被鸣那张）。onPick(option) 由 client 发帧。
   */
  showOptionPicker(options, onPick) {
    const el = this.el.claimTip;
    if (!el) return;
    const btns = (options || []).map((opt, i) => {
      const faces = (opt.tiles || []).map((code) => tileLabel(code)).join('');
      return `<button class="ct-pick" data-i="${i}">${esc(faces)}</button>`;
    }).join('');
    el.innerHTML = `<span class="ct-note">选一种吃</span>${btns}`;
    el.hidden = false;
    el.style.pointerEvents = 'auto';   // 提示条默认穿透，选择条必须可点
    el.querySelectorAll('.ct-pick').forEach((b) => {
      b.addEventListener('click', () => {
        this.hideClaimTip();
        onPick(options[+b.dataset.i]);
      });
    });
  }

  // ------------------------------------------------------------------ 结算

  /** 局终（game_end）：四家收支 + 和牌明细 */
  showResult(frame, state) {
    if (!frame || !this.el.result) return;
    const el = this.el;
    const win = frame.win;
    const reason = frame.reason || '';

    el.rsKind.textContent = ROUND_LABELS[reason] || reason || '局终';
    el.rsTitle.textContent = `${state.round || ''} 结束`;
    el.rsWinner.hidden = !win;
    if (win) {
      const yaku = (win.yaku || [])
        .map((item) => `${YAKU_LABELS[item.id] || item.id}${item.han != null ? ` ${item.han}番` : ''}`)
        .join(' · ');
      const parts = [];
      if (yaku) parts.push(yaku);
      if (win.han > 0) parts.push(`合计 ${win.han} 番`);
      if (win.total != null) parts.push(`${signed(win.total)} 点`);
      el.rsWinner.innerHTML = `<b>${esc(state.seatNames[this.viewOf(win.seat, state)])}</b>`
        + `${win.tsumo ? ' 自摸 ' : ' 荣和 '}${esc(tileLabel(win.tile))}`
        + (win.from_seat != null ? `（放铳：${esc(state.seatNames[this.viewOf(win.from_seat, state)])}）` : '')
        + (parts.length ? `<br>${esc(parts.join(' ｜ '))}` : '');
    }

    const results = Array.isArray(frame.results) ? [...frame.results] : [];
    results.sort((a, b) => (a.seat | 0) - (b.seat | 0));
    const isRiichi = Array.isArray(frame.points_after) && frame.points_after.length === 4;
    el.rsRows.innerHTML = results.map((result) => {
      const view = this.viewOf(result.seat, state);
      const me = view === 0 ? ' me' : '';
      const isWinner = frame.win && frame.win.seat === result.seat;
      const isLoser = frame.win && frame.win.from_seat === result.seat && !frame.win.tsumo;
      const role = isWinner ? (frame.win.tsumo ? '自摸' : '荣和')
        : isLoser ? '放铳'
          : (frame.reason === 'draw_exhausted' && frame.tenpai?.[result.seat] ? '听牌' : '—');
      const delta = result.delta_score;
      const total = isRiichi ? frame.points_after[result.seat] : result.match_total;
      const dealer = frame.dealer === result.seat ? '<em>亲</em>' : '';
      return `<div class="rs-row${me}">`
        + `<span class="name">${esc(state.seatNames[view])}${dealer}</span>`
        + `<span class="role">${esc(role)}</span>`
        + `<span class="delta ${delta > 0 ? 'up' : delta < 0 ? 'down' : ''}">${signed(delta)}</span>`
        + `<span class="total">${fmt(total)}</span>`
        + '</div>';
    }).join('');

    el.rsClose.hidden = false;
    el.rsLobby.hidden = !!frame.match_will_continue;
    const interlude = Number.isFinite(frame.interlude_ms) ? frame.interlude_ms : 12000;
    this._nextTimer = el.rsNext;
    if (frame.match_will_continue) {
      this._resultDeadline = performance.now() + Math.max(0, interlude);
      el.rsNext.textContent = `下一局 ${Math.ceil(interlude / 1000)} 秒后开始`;
    } else {
      this._resultDeadline = 0;
      el.rsNext.textContent = '本场结束，正在结算…';
    }

    el.result.hidden = false;
  }

  /** 场终（match_end）：名次表 */
  showFinal(state) {
    const frame = state.matchEnd;
    if (!frame || !this.el.result) return;
    const el = this.el;
    el.rsKind.textContent = '场终';
    el.rsTitle.textContent = `共 ${frame.hands_played ?? state.handsPlayed ?? 0} 手 ｜ 连庄 ${frame.renchan_count ?? 0} 次`;
    el.rsWinner.hidden = true;

    const rows = Array.isArray(frame.rankings) ? frame.rankings : [];
    el.rsRows.innerHTML = rows.map((row) => {
      const view = this.viewOf(row.seat, state);
      const me = view === 0 ? ' me' : '';
      const score = row.final_score;
      return `<div class="rs-row${me}">`
        + `<span class="name rank${row.rank === 1 ? ' first' : ''}">${row.rank} 位</span>`
        + `<span class="role">${esc(state.seatNames[view])}</span>`
        + `<span class="delta ${score > 0 ? 'up' : score < 0 ? 'down' : ''}">${signed(score)}</span>`
        + '<span class="total"></span>'
        + '</div>';
    }).join('');

    el.rsClose.hidden = true;
    el.rsLobby.hidden = false;
    el.rsNext.textContent = '本场已结束';
    this._nextTimer = null;
    this._resultDeadline = 0;
    el.result.hidden = false;
  }

  hideResult() {
    if (this.el.result) this.el.result.hidden = true;
    this._resultDeadline = 0;
    this._nextTimer = null;
  }

  /** 协议 seat → 屏幕视角（与 state.viewSeat 同口径，UI 不依赖 app 实例） */
  viewOf(seat, state) {
    return (((seat | 0) - (state?.mySeat | 0)) + 4) % 4;
  }

  stats(info) {
    if (this.el.stats) this.el.stats.textContent = `${info.calls} draw calls · ${info.fps} fps`;
  }
}

export { ACTION_LABELS, YAKU_LABELS };
