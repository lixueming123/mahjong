/**
 * 牌桌 HUD（纯 DOM 覆盖层，不含登录/大厅）。
 *
 * 职责：场况卡、四家名牌、听牌条、出牌倒计时、播报、轻提示、局终/场终结算。
 * 数据只从 state 读（服务端权威），倒计时从 client 的窗口读（唯一有 deadline 的地方）。
 *
 * 坐标按 1920x1080 设计像素写在 CSS 里，这里只改文案与显隐。
 */
import { ALL_CODES } from './config.js';
import { normTile } from './state.js';
import { ACTION, REASON, yakuName } from './protocol_enums.js';

const $ = (id) => document.getElementById(id);

/** 结算摊牌贴图：手牌图集 8x5，格位与 assets.js 的 UV 同源（每格 120x200） */
const DORA_ATLAS_URL = './assets/tile-atlas-hand.png';
/**
 * 场况卡宝牌是平躺的小牌：用平躺牌面图集（无立牌牌顶），格位同为 8x5。
 * 经 CSS 变量传给 ui.css 时 url() 会按样式表目录解析，所以这里给绝对地址。
 */
const DORA_FLAT_ATLAS_URL = new URL('../assets/tile-atlas-flat.png', import.meta.url).href;
const DORA_INDEX = new Map(ALL_CODES.map((code, i) => [code, i]));

const WIND_CHARS = ['東', '南', '西', '北'];
const HONOR_LABELS = { 1: '东', 2: '南', 3: '西', 4: '北', 5: '白', 6: '发', 7: '中' };
const ROUND_LABELS = {
  [REASON.WIN_RON]: '荣和', [REASON.WIN_TSUMO]: '自摸', [REASON.DRAW_EXHAUSTED]: '荒庄流局',
};
const ACTION_LABELS = {
  [ACTION.PONG]: '碰', [ACTION.CHOW]: '吃', [ACTION.KONG]: '杠', [ACTION.ANKAN]: '暗杠',
  [ACTION.KAKAN]: '加杠', [ACTION.RIICHI]: '立直', [ACTION.WIN]: '和', [ACTION.PASS]: '过',
};
const YAKU_LABELS = {
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
      roundBars: [...($('round-bars')?.children ?? [])],
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
      rsHand: $('rs-hand'),
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
    this.renderDoraRow(state);
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

  /**
   * 场况卡第一行 = 宝牌指示牌（用户指定位置，参考截图：明牌 + 背面槽位）。
   * state.dora 来自 game_start / state_sync 的 dora_indicators[]：
   * 有指示牌的槽位翻开后牌面，其余保持橙金背面；一局开始时至少翻 1 张，
   * 杠后最多补到 5 张。还没有对局数据时第一格显示场风字。
   */
  renderDoraRow(state) {
    const el = this.el;
    if (!el.roundMark) return;
    const dora = Array.isArray(state.dora) ? state.dora : [];
    const slots = [el.roundMark, ...el.roundBars];
    slots.forEach((slot, i) => {
      if (!slot) return;
      const code = dora[i];
      if (typeof code === 'string' && code.length === 2) {
        // 牌面用图集贴图（用户：不要「3筒」这种文字标签）。整格铺满槽位：
        // background-size 800%x500%（8 列 5 行），百分比位移取格 ——
        // bg-position 百分比的语义是「图的 p% 对齐槽的 p%」，所以
        // x = col/(cols-1)、y = row/(rows-1)，与槽位实际尺寸无关。
        // 红五/红中图集里本来就是红的，不再需要字色类。
        const idx = DORA_INDEX.has(code) ? DORA_INDEX.get(code) : DORA_INDEX.get('back');
        const col = idx % 8;
        const row = Math.floor(idx / 8);
        slot.classList.add('face');
        slot.classList.remove('red', 'sm');
        slot.textContent = '';
        slot.style.setProperty('--face', `url(${DORA_FLAT_ATLAS_URL})`);
        slot.style.setProperty('--fx', `${col / 7 * 100}%`);
        slot.style.setProperty('--fy', `${row / 4 * 100}%`);
      } else {
        slot.classList.remove('face', 'red', 'sm');
        slot.textContent = i === 0 && !dora.length ? (state.round || '東')[0] : '';
        for (const prop of ['--face', '--fx', '--fy']) {
          slot.style.removeProperty(prop);
        }
      }
    });
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
    const real = (actions || []).filter((action) => action !== ACTION.PASS);
    if (!real.length) { el.hidden = true; return; }
    const order = [ACTION.WIN, ACTION.KONG, ACTION.PONG, ACTION.CHOW];
    real.sort((a, b) => order.indexOf(a) - order.indexOf(b));
    const badges = real.map((action) => {
      const label = ACTION_LABELS[action] || action;
      return `<span class="ct-act${action === ACTION.WIN ? ' win' : ''}">${esc(label)}</span>`;
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
        .map((item) => `${YAKU_LABELS[yakuName(item.id)] || yakuName(item.id)}${item.han != null ? ` ${item.han}番` : ''}`)
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
    this.renderWinHand(frame, state);

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
          : (frame.reason === REASON.DRAW_EXHAUSTED && frame.tenpai?.[result.seat] ? '听牌' : '—');
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

  /**
   * 结算面板 · 和牌详情：把和牌者的牌**用大牌面排出来**——
   * 副露分组（被鸣张横置）+ 门前牌 + 和牌张单列高亮 + 役种逐条列番数。
   * 3D 侧家的摊牌立牌又小又远，看不清「怎么和的」；这里是权威展示位。
   * 数据：state.revealHands（和牌者门前牌，服务端 results[].hand 扣副露）
   * + state.melds（副露分组）+ win.tile / win.yaku。
   */
  renderWinHand(frame, state) {
    const el = this.el;
    if (!el.rsHand) return;
    const win = frame && typeof frame === 'object' ? frame.win : null;
    if (!win) {
      el.rsHand.hidden = true;
      el.rsHand.innerHTML = '';
      return;
    }
    const view = this.viewOf(win.seat, state);
    // 贴图挂在独立的 .rsface 内层：被鸣张横放时只旋转这一层，外层仍按 64x46
    // 占位。早先把 background-size 直接贴在变形的外层上，图案会被拉扁成「矮牌」。
    const face = (code, cls = '') => {
      const idx = DORA_INDEX.has(code) ? DORA_INDEX.get(code) : DORA_INDEX.get('back');
      const col = idx % 8;
      const row = Math.floor(idx / 8);
      return `<i class="rstile${cls ? ` ${cls}` : ''}"><i class="rsface" style="background-image:url(${DORA_ATLAS_URL});`
        + `background-size:800% auto;background-position:${col / 7 * 100}% ${row / 4 * 100}%"></i></i>`;
    };

    // 副露分组：被鸣的那张横置（meld.rotated 是 action_event 反查的下标）；
    // 他家暗杠的 'back' 占位（服务端异常时）退化为背面格
    const melds = (state.melds || []).filter((meld) => meld.seat === view);
    const meldHtml = melds.map((meld) => {
      const tiles = (meld.tiles || []).map((t) => (typeof t === 'string' ? t : 'back'));
      if (!tiles.length) return '';
      const inner = tiles.map((t, i) => {
        // meld.kind 是渲染层 UI 别名（chi/pong/kong/ankan/kakan），非线协议字段，按硬规则 2 保留字符串
        const rotated = meld.kind !== 'ankan' && i === meld.rotated;
        return face(normTile(t) || t, rotated ? 'claim' : '');
      }).join('');
      return `<div class="rsmeld">${inner}</div>`;
    }).join('');

    // 门前牌：和牌张抽出来单列（雀魂口径：右侧独立格 + 金框高亮）
    const hand = (state.revealHands?.[view] || []).map(normTile).filter(Boolean);
    const agari = normTile(win.tile);
    const at = agari ? hand.indexOf(agari) : -1;
    if (at >= 0) hand.splice(at, 1);
    const handHtml = hand.map((t) => face(t)).join('');
    const agariHtml = agari ? face(agari, 'agari') : '';

    const yakuHtml = (Array.isArray(win.yaku) ? win.yaku : [])
      .filter((item) => item && (typeof item.id === 'number' || typeof item.id === 'string'))
      .map((item) => `<b>${esc(YAKU_LABELS[yakuName(item.id)] || yakuName(item.id))}</b>`
        + `${Number.isInteger(item.han) ? ` ${item.han}番` : ''}`)
      .join('<span class="sep"> · </span>');

    const row = meldHtml || hand.length || agariHtml
      ? `<div class="rshand-row">`
        + (meldHtml ? `<div class="rshand-melds">${meldHtml}</div>` : '')
        + `<div class="rshand-concealed">${handHtml}</div>`
        + (agariHtml ? `<div class="rshand-agari">${agariHtml}</div>` : '')
        + `</div>`
      : `<div class="rshand-empty">手牌数据缺失</div>`;
    el.rsHand.innerHTML = row
      + (yakuHtml ? `<div class="rshand-yaku">${yakuHtml}</div>` : '');
    el.rsHand.hidden = false;
  }

  /** 场终（match_end）：名次表 */
  showFinal(state) {
    const frame = state.matchEnd;
    if (!frame || !this.el.result) return;
    const el = this.el;
    el.rsKind.textContent = '场终';
    el.rsTitle.textContent = `共 ${frame.hands_played ?? state.handsPlayed ?? 0} 手 ｜ 连庄 ${frame.renchan_count ?? 0} 次`;
    el.rsWinner.hidden = true;
    if (el.rsHand) { el.rsHand.hidden = true; el.rsHand.innerHTML = ''; }

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
