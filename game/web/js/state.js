/**
 * 牌局状态机（服务端权威版）。
 *
 * 数据形状是「屏幕视角」的：索引 0 恒为本家（无论协议里的 seat 是几），
 * 1/2/3 依次是下家/对家/上家。协议里的 seat 与屏幕视角的换算只在这里做一次
 * （viewSeat），渲染层（hand/opponents/rivers/melds）永远只认 0..3 的屏幕座位。
 *
 * 写入只有这几个入口，全部由 client.js 按 PROTOCOL.md 的帧调用：
 *   startRound(game_start)  → 整局重置
 *   applySync(state_sync)   → 权威整体覆盖（H9）
 *   drawTile / discardTile / applyActionEvent / applyGameEnd / applyTenpai → 即时事件
 * 渲染层只读，不碰网络。
 */
import { RIVER, HAND, HAND_SIZE } from './config.js';
import { ACTION, REASON, MELD_TYPE, nameOf, yakuName } from './protocol_enums.js';

const WINDS = ['東', '南', '西', '北'];

/** §2.1 牌码合取正则（含赤 5 `0m/0p/0s`） */
const TILE_RE = /^((0|[1-9])[mps]|[1-7]z)$/;

export function isValidTile(code) {
  return typeof code === 'string' && TILE_RE.test(code);
}

export function normTile(code) {
  if (code === '0m' || code === '0p' || code === '0s') return `5${code[1]}`;
  return code;
}

export function sortKey(code) {
  const c = normTile(code);
  const suit = 'mpsz'.indexOf(c[1]);
  return suit * 10 + parseInt(c[0], 10);
}

export function sortHand(hand) {
  return [...hand].sort((a, b) => sortKey(a) - sortKey(b) || a.localeCompare(b));
}

/** 服务端 melds[]（H4）→ 渲染层的 {seat, kind, tiles, rotated} */
function toViewMeld(viewSeat, meld, claimedTile = null) {
  const tiles = Array.isArray(meld.tiles) && meld.tiles.length
    ? [...meld.tiles]
    : Array(4).fill('back');            // 他家暗杠：服务端恒下发 []（§5.9）
  // 横放的是「被鸣的那张」：协议 melds[] 不带这个信息，由 action_event 时记下的牌河末张反查；
  // 拿不到就退化成组首（碰/杠三张同面，横哪张都一样；吃则至少有横放，不会整组立着）
  let rotated = -1;
  if (Array.isArray(meld.tiles) && meld.tiles.length) {
    if (claimedTile) {
      const at = tiles.indexOf(claimedTile);
      if (at >= 0) rotated = at;
    }
    if (rotated < 0 && meld.type !== MELD_TYPE.ANKAN) rotated = 0;
  }
  return {
    seat: viewSeat,
    // kind 是渲染层 UI 别名（字符串），不是线值；由线枚举反查名称，吃沿用 'chi'
    kind: meld.type === MELD_TYPE.CHOW ? 'chi' : nameOf('meld_type', meld.type),
    type: meld.type,
    tiles,
    rotated,
  };
}

export function createState() {
  const listeners = new Map();
  const state = {
    // ---- 桌面视图（渲染层读这些）----
    hands: [[], [], [], []],          // hands[0] 明牌，1..3 用 'back' 占位
    rivers: [[], [], [], []],
    melds: [],
    // 每家副露「横放的那张」（按 view 座位、按副露下标）：协议 melds[] 不带，
    // 只能在 action_event 时从牌河末张记下来，等 state_sync 落地时按序贴回去
    meldClaims: [[], [], [], []],
    turn: 0,                          // 屏幕视角的当前手番
    scores: [0, 0, 0, 0],
    round: '—',
    honba: 0,
    remaining: 0,
    selected: -1,
    drawn: null,
    lastAction: '',

    // ---- 协议侧（client.js 读这些）----
    mySeat: 0,                        // 协议座位号
    seatNames: ['自', '右', '对', '左'],
    isBot: [false, false, false, false],
    rooms: [],                        // §5.14 大厅房间列表（最近一次 room_list）
    handCounts: [HAND_SIZE, HAND_SIZE, HAND_SIZE, HAND_SIZE],
    disconnected: [false, false, false, false],
    riichi: [false, false, false, false],
    riichiTiles: [],                  // v1.21：立直宣言牌候选（当前打牌窗口）
    riichiArming: false,              // 立直宣言模式：点候选牌才发帧（渲染层据此高亮）
    windowTile: null,                 // §12.10：抢杠窗口的被抢牌面（其余窗口为 null）
    lastDiscard: null,                // 最近一次 discard_event 的打出者 {seat, code}（碰/和可鸣任何一家）
    claimTarget: null,                // 鸣牌窗口：牌河里被指的那张 {seat, index}（渲染层据此打光）
    tenpai: { isTenpai: false, waits: [], noYaku: false },
    tenpaiSeq: null,                  // §5.16：独立的 tenpai_seq，不掺 room.seq
    tenpaiFlags: [],                  // §12.5：荒庄时四家是否听牌（game_end / state_sync 携带）
    mode: 'classic',                  // §5.4：帧里有 points ⇒ riichi
    points: null,
    dora: [],
    roundIndex: 0,
    roundCount: 4,
    dealer: 0,
    matchTotal: [0, 0, 0, 0],
    matchEnd: null,                   // §9.6 场终载荷（未场终为 null）
    phase: 'idle',                    // idle | playing | ended

    // ---- 结算 / 会话留档（逻辑层只存原始载荷，怎么画由上层决定）----
    result: null,                     // §5.10 game_end 原始载荷（局终）
    resultAt: 0,                      // 收到局终的时刻（performance.now，供局间倒计时）
    winSeat: null,                    // 和牌者（协议 seat；荒庄为 null）
    // v4.5 摊牌（用户口径：只亮和牌者）：game_end.results[].hand（扁平手牌，
    // 含副露）扣掉副露后的门前牌，按屏幕视角索引；仅结算期有值，game_start 复位。
    // 渲染层据此把和牌者的牌背换成牌面，其余三家不动（pygame 摊牌口径的收窄版）。
    revealHands: null,                // [view0..3] -> 门前牌码数组 | null（仅和牌者席位非空）
    revealWinner: null,               // 和牌者的屏幕视角（渲染层高亮；荒庄为 null）
    matchWillContinue: false,         // §5.10：本场是否还有下一局
    rankings: [],                     // §5.15 场终名次
    handsPlayed: 0,                   // §5.15 本场已打局数
    stats: null,                      // §5.13 战绩载荷（游客为零值）
    lastError: null,                  // 最近一条 error{code,message}
    roomId: null,
    needsSync: false,                 // 本地与服务端可能不一致时置位，由 client 拉 sync
    protocolWarning: false,           // §8.2：收到过 seats[].hand 之类的越权字段
  };

  // 兼容只关心本家手牌的模块（hand.js / ui.js / input.js）：真实数据仍在 hands[0]
  Object.defineProperty(state, 'hand', {
    enumerable: true,
    get: () => state.hands[0],
    set: (v) => { state.hands[0] = v; },
  });
  Object.defineProperty(state, 'oppHandCount', {
    enumerable: true,
    get: () => state.hands.map((h) => h.length),
  });

  function on(evt, fn) {
    if (!listeners.has(evt)) listeners.set(evt, []);
    listeners.get(evt).push(fn);
  }
  function emit(evt, payload) {
    for (const fn of listeners.get(evt) || []) fn(payload, state);
  }
  function changed(action) {
    if (action) state.lastAction = action;
    emit('change', state);
  }

  /** 协议 seat → 屏幕座位（本家恒 0） */
  function viewSeat(seat) {
    return ((seat | 0) - state.mySeat + 4) % 4;
  }

  /** game_start：整局重置（§5.4 客户端本地重置的唯一信号） */
  function startRound(frame) {
    state.mySeat = Number.isInteger(frame.seat) ? frame.seat : state.mySeat;
    const points = Array.isArray(frame.points) ? frame.points : null;
    state.mode = points ? 'riichi' : 'classic';
    const roundIndex = Number.isInteger(frame.round_index) && frame.round_index >= 0 && frame.round_index < 64
      ? frame.round_index : 0;
    state.roundIndex = roundIndex;
    state.roundCount = Number.isInteger(frame.round_count) && frame.round_count > 0 ? frame.round_count : 4;
    state.dealer = Number.isInteger(frame.dealer) ? frame.dealer : 0;

    state.hands = [[], [], [], []];
    // 顺序由服务端给（§2.2），客户端不重排；非法牌码过滤掉并记一条 WARN（参考端同口径）
    const hand = Array.isArray(frame.hand) ? frame.hand : [];
    state.hands[0] = hand.filter(isValidTile);
    if (state.hands[0].length !== hand.length) {
      console.warn('game_start.hand 含非法牌码，已过滤', hand);
    }
    for (let view = 1; view < 4; view++) state.hands[view] = Array(HAND_SIZE).fill('back');
    state.handCounts = [state.hands[0].length, HAND_SIZE, HAND_SIZE, HAND_SIZE];

    state.rivers = [[], [], [], []];
    state.melds = [];
    state.meldClaims = [[], [], [], []];
    state.selected = -1;
    state.drawn = null;
    state.riichi = [false, false, false, false];
    state.riichiTiles = [];
    state.riichiArming = false;
    state.windowTile = null;
    state.lastDiscard = null;
    state.claimTarget = null;
    state.tenpai = { isTenpai: false, waits: [], noYaku: false };
    state.tenpaiSeq = null;                       // 新场次：服务端计数器也不再生效（§5.16）
    state.tenpaiFlags = [];
    // PROTOCOL v2.2 §5.4：本局开始前的场次进度。刷新恢复时靠它把经典场累计分和连庄数
    // 一次对齐，不再依赖客户端从上一局 game_end 累加。
    if (Array.isArray(frame.match_total) && frame.match_total.length === 4) {
      state.matchTotal = frame.match_total.map((value) => (Number.isInteger(value) ? value : 0));
    }
    state.honba = Number.isInteger(frame.renchan_count) && frame.renchan_count >= 0 ? frame.renchan_count : 0;
    state.phase = 'playing';
    state.disconnected = [false, false, false, false];
    state.result = null;
    state.resultAt = 0;
    state.winSeat = null;
    state.revealHands = null;
    state.revealWinner = null;
    state.matchWillContinue = false;
    state.rankings = [];
    state.handsPlayed = 0;
    state.needsSync = false;
    state.protocolWarning = false;

    if (state.mode === 'riichi') {
      state.points = points.length === 4 ? [...points] : null;
      state.scores = state.points ? [...state.points] : [0, 0, 0, 0];
    } else {
      state.points = null;
      state.scores = [...state.matchTotal];
    }
    state.dora = (frame.dora_indicators || []).filter(isValidTile);
    // 日麻切 14 张王牌，发牌后 remaining 69；classic 83（§12.2 / §0.6）
    state.remaining = state.mode === 'riichi' ? 69 : 83;

    const wind = frame.round_wind != null ? frame.round_wind : Math.floor(state.roundIndex / 4);
    state.round = `${WINDS[wind % 4]}${(state.roundIndex % 4) + 1}局`;

    changed(`${state.round} 开始 · ${state.mode === 'riichi' ? '日麻' : '经典'}`);
  }

  /** state_sync：权威整体覆盖（§5.12） */
  function applySync(frame) {
    const seats = Array.isArray(frame.seats) ? frame.seats : [];
    const melds = [];
    seats.forEach((seat) => {
      if (!seat || typeof seat !== 'object' || !Number.isInteger(seat.seat)) return;
      // §8.2 第 2 条：即便服务端缺陷下发 seats[].hand，客户端也不消费、不渲染，只记 WARN。
      if (Object.prototype.hasOwnProperty.call(seat, 'hand')) {
        state.protocolWarning = true;
        console.warn('state_sync.seats[].hand 属越权字段，已脱敏忽略', seat.seat);
      }
      const view = viewSeat(seat.seat);
      if (typeof seat.name === 'string') state.seatNames[view] = seat.name;
      state.isBot[view] = !!seat.is_bot;
      state.handCounts[view] = Number.isInteger(seat.hand_count) ? seat.hand_count : state.handCounts[view];
      state.disconnected[view] = !!seat.disconnected;
      state.riichi[view] = !!seat.riichi;
      const discards = Array.isArray(seat.discards) ? seat.discards : [];
      state.rivers[view] = discards.filter(isValidTile);
      if (state.rivers[view].length !== discards.length) {
        console.warn('state_sync 牌河含非法牌码，已过滤', discards);
      }
      if (view !== 0) {
        state.hands[view] = Array(state.handCounts[view]).fill('back');
        const claims = state.meldClaims[view] || [];
        (seat.melds || []).forEach((meld, i) => {
          melds.push(toViewMeld(view, meld, claims[i] ?? null));
        });
      }
    });
    // 本家副露取顶层 melds：只有这一份带暗杠的真实牌面（§5.9）
    (frame.melds || []).forEach((meld, i) => {
      melds.push(toViewMeld(0, meld, state.meldClaims[0]?.[i] ?? null));
    });
    state.melds = melds;

    if (Array.isArray(frame.hand)) {
      const hand = frame.hand.filter(isValidTile);
      if (hand.length !== frame.hand.length) console.warn('state_sync.hand 含非法牌码，已过滤', frame.hand);
      const ownMelds = melds.filter((meld) => meld.seat === 0).length;
      const afterDiscard = 13 - 3 * ownMelds;
      // 服务端按 §2.2 排序下发；但「刚摸到的那张」要留在最右（雀魂口径），
      // 等这一次打出之后才归位，否则每摸一张牌都会窜进牌列中间。
      // 杠后的补牌同理：那次 sync 的手牌比杠前少了几张，只能靠 draw 帧记下的牌面认出来。
      state.hands[0] = keepDrawnLast(hand, state.hands[0], afterDiscard, state.drawn);
      state.handCounts[0] = state.hands[0].length;
      state.drawn = state.hands[0].length > afterDiscard
        ? state.hands[0][state.hands[0].length - 1]
        : null;
    }
    if (Number.isInteger(frame.remaining)) state.remaining = frame.remaining;
    if (Number.isInteger(frame.current_seat) && frame.current_seat >= 0 && frame.current_seat <= 3) {
      state.turn = viewSeat(frame.current_seat);
    }
    if (Array.isArray(frame.points) && frame.points.length === 4) {
      state.points = [...frame.points];
      state.scores = [...frame.points];
    }
    if (Array.isArray(frame.dora_indicators)) {
      state.dora = frame.dora_indicators.filter(isValidTile);
    }
    // §12.5：荒庄前那份 state_sync 会带四家听牌标记（classic 帧不带该键）
    if (Array.isArray(frame.tenpai) && frame.tenpai.length === 4) {
      state.tenpaiFlags = frame.tenpai.map((flag) => !!flag);
    }

    if (state.selected >= state.hands[0].length) state.selected = -1;
    state.needsSync = false;          // 权威帧到达 ⇒ 本地不再需要补偿同步
    changed();
  }

  /** draw：仅本家收得到（§5.5） */
  function drawTile({ tile, seat }) {
    if (!Number.isInteger(seat) || seat < 0 || seat > 3) {
      console.warn('draw.seat 非法，已忽略', seat);
      return;
    }
    if (!isValidTile(tile)) {
      console.warn('draw.tile 非法，已忽略', tile);
      return;
    }
    const view = viewSeat(seat);
    if (view === 0) {
      state.hands[0].push(tile);
      state.drawn = tile;
    } else {
      state.hands[view].push('back');
    }
    state.handCounts[view] = state.hands[view].length;
    state.turn = view;
    state.remaining = Math.max(0, state.remaining - 1);
    changed();
    emit('draw', { seat: view, code: view === 0 ? tile : null });
  }

  /** discard_event：广播 4 家（§5.6）。点击打牌后不动本地状态，等这一帧落地。 */
  function discardTile({ seat, tile }) {
    if (!Number.isInteger(seat) || seat < 0 || seat > 3) {
      console.warn('discard_event.seat 非法，已忽略', seat);
      return null;
    }
    if (!isValidTile(tile)) {
      console.warn('discard_event.tile 非法，已忽略', tile);
      return null;
    }
    const view = viewSeat(seat);
    let from = null;
    if (view === 0) {
      const hand = state.hands[0];
      let index = hand.lastIndexOf(tile);
      if (index < 0) index = hand.findIndex((code) => normTile(code) === normTile(tile));
      if (index < 0) {
        // 本地手牌与服务端不一致（丢过帧 / 同步前手牌为空）：这条广播**不能吞**，
        // 否则牌河与手番一起停摆；照常落河，并置位让 client 拉一次权威同步。
        console.warn('本家 discard_event 的牌不在手牌中，等待同步', tile, hand);
        state.needsSync = true;
      } else {
        // 飞出起点按「含摸牌偏移」的槽位算：打掉的若是刚摸那张，就从抬起的位置起飞
        from = handLayout(hand.length, drawnIndex(state))[index];
        hand.splice(index, 1);
        // 打完这一张才归位排序（摸牌期间那张一直挂在最右，不插进牌列）
        state.hands[0] = sortHand(hand);
      }
      state.selected = -1;
      state.drawn = null;
    } else if (state.hands[view].length) {
      state.hands[view].pop();
    }
    state.handCounts[view] = state.hands[view].length;

    const river = state.rivers[view];
    if (river.length >= RIVER.cap) river.shift();
    river.push(tile);

    state.turn = (view + 1) % 4;
    state.lastDiscard = { seat: view, code: tile };   // 碰/和可鸣任何一家刚打的这张
    state.riichiTiles = [];
    state.riichiArming = false;
    changed(`seat${view} 打出 ${tile}`);
    emit('discard', { code: tile, seat: view, from });
    return tile;
  }

  /**
   * action_event：即时事件（H9）。权威状态仍以随后的 state_sync 为准（§5.8 冻结规则 3）。
   *
   * 这里**只**做能从事件本身确定的公开派生：
   *  - pong/chow/kong：把被鸣的那张从 from_seat 牌河移除（规则 2：恒为该家最近一次打出）；
   *  - ankan/kakan：不动任何牌河（规则 4）；
   *  - riichi：置公开立直标记（权威值随后由 state_sync.seats[].riichi 覆盖）；
   *  - win：清听牌缓存。
   * 副露与手牌**一律不在这里拼**：`tiles` 的语义按动作不同（pong/kong/kakan = [被鸣张] 1 张、
   * chow = [手中两张]、ankan 恒 []），拿它反推整组副露会拼出 2 张的碰；等 state_sync 覆盖。
   */
  function applyActionEvent(event) {
    if (!Number.isInteger(event.seat) || event.seat < 0 || event.seat > 3) {
      console.warn('action_event.seat 非法，已忽略', event.seat);
      return;
    }
    const view = viewSeat(event.seat);
    const action = event.action;
    if (action === ACTION.PONG || action === ACTION.CHOW || action === ACTION.KONG) {
      if (Number.isInteger(event.from_seat)) {
        const river = state.rivers[viewSeat(event.from_seat)];
        if (river.length) {
          // 被鸣的这张（牌河末张）就是副露里要横放的那张，记下来给渲染层用
          state.meldClaims[view].push(river[river.length - 1]);
          river.pop();
        } else {
          console.warn('action_event 的 from_seat 牌河为空', event.from_seat);
          state.meldClaims[view].push(null);
        }
      } else {
        console.warn('action_event 缺少 from_seat（协议异常）', action);
        state.meldClaims[view].push(null);
      }
    } else if (action === ACTION.ANKAN) {
      // 暗杠没有「被鸣的那张」，但组数 +1，占位保持下标对齐
      state.meldClaims[view].push(null);
    } else if (action === ACTION.RIICHI) {
      state.riichi[view] = true;
    } else if (action === ACTION.WIN) {
      state.phase = 'ended';
      state.winSeat = Number.isInteger(event.seat) ? event.seat : null;
    }
    // F-TP-11：和牌或任何副露生效 ⇒ 牌形已变，旧的听牌 waits 是脏数据
    if ([ACTION.WIN, ACTION.PONG, ACTION.CHOW, ACTION.KONG, ACTION.ANKAN, ACTION.KAKAN].includes(action)) {
      state.tenpai = { isTenpai: false, waits: [], noYaku: false };
    }
    state.riichiTiles = [];
    state.riichiArming = false;
    state.lastAction = `${state.seatNames[view]} ${actionLabel(action)}`;
    changed();
    emit('action', { seat: view, action, tiles: event.tiles || [] });
  }

  /** game_end：局终（§5.10）。分数之外保留原始载荷，结算界面自行取用。 */
  function applyGameEnd(frame) {
    state.result = frame;             // 原始载荷（含 results[].hand / win.yaku / points_after）
    state.resultAt = performance.now();
    state.phase = 'ended';
    state.winSeat = frame.win && Number.isInteger(frame.win.seat) ? frame.win.seat : null;
    state.matchWillContinue = frame.match_will_continue === true;
    const results = Array.isArray(frame.results) ? frame.results : [];
    if (Array.isArray(frame.points_after) && frame.points_after.length === 4) {
      state.points = [...frame.points_after];
      state.scores = [...frame.points_after];
    } else {
      results.forEach((result) => {
        if (!result || !Number.isInteger(result.seat)) return;
        const view = viewSeat(result.seat);
        state.matchTotal[view] = typeof result.match_total === 'number'
          ? result.match_total
          : (state.matchTotal[view] || 0) + (result.delta_score || 0);
      });
      state.scores = [...state.matchTotal];
    }
    if (typeof frame.renchan_count === 'number') state.honba = frame.renchan_count;
    if (Array.isArray(frame.tenpai) && frame.tenpai.length === 4) {
      state.tenpaiFlags = frame.tenpai.map((flag) => !!flag);
    }
    state.selected = -1;
    state.drawn = null;
    state.riichiTiles = [];
    state.riichiArming = false;
    state.windowTile = null;
    state.lastDiscard = null;
    state.claimTarget = null;
    state.tenpai = { isTenpai: false, waits: [], noYaku: false };

    // v4.5 摊牌（用户口径修订）：**只有和牌者**亮门前牌，其余三家保持牌背。
    // results[].hand 是四家扁平手牌（门前 + 副露，game_end 专属），扣掉该家
    // 副露（多重集减法）得到门前牌；荒庄没人和牌 ⇒ 不摊牌。
    // 他家暗杠此时已被 §5.9 的最后一份 state_sync 补全为真实牌面；万一仍是
    // 'back' 占位，indexOf 找不到合法牌码，天然跳过，不会误删门前牌。
    state.revealHands = null;
    state.revealWinner = null;
    if (state.winSeat != null) {
      const winnerSeat = state.winSeat;
      const entry = results.find((r) => r && r.seat === winnerSeat && Array.isArray(r.hand));
      if (entry) {
        const view = viewSeat(winnerSeat);
        const concealed = entry.hand.filter(isValidTile);
        for (const meld of state.melds) {
          if (meld.seat !== view) continue;
          for (const tile of meld.tiles || []) {
            const at = concealed.indexOf(tile);
            if (at >= 0) concealed.splice(at, 1);
          }
        }
        state.revealHands = [null, null, null, null];
        state.revealHands[view] = concealed;
        state.revealWinner = view;
      }
    }

    const win = frame.win;
    let text = reasonLabel(frame.reason);
    if (win) {
      const seat = viewSeat(win.seat);
      const yaku = (win.yaku || [])
        .map((item) => YAKU_NAMES[yakuName(item.id)] || yakuName(item.id))
        .join(' ');
      text = `${state.seatNames[seat]} ${win.tsumo ? '自摸' : '荣和'} ${win.tile || ''}`
        + (yaku ? ` · ${yaku}` : '') + (win.han != null ? ` · ${win.han}番` : '');
    }
    changed(text);
  }

  /**
   * tenpai：只影响听牌条（§5.16）；`tenpai_seq` 独立于 room.seq，且只接受严格递增。
   * 乱序/重放帧一律丢弃，避免听牌条回退（§7 R6）；is_tenpai=false 时必须清空 waits。
   */
  function applyTenpai(frame) {
    const seq = frame.tenpai_seq;
    if (!Number.isInteger(seq)) {
      console.warn('tenpai 帧缺少 tenpai_seq，已丢弃', frame);
      return;
    }
    if (state.tenpaiSeq !== null && seq <= state.tenpaiSeq) {
      console.debug('丢弃过期 tenpai 帧', seq, '<=', state.tenpaiSeq);
      return;
    }
    state.tenpaiSeq = seq;
    const waits = [];
    for (const item of Array.isArray(frame.waits) ? frame.waits : []) {
      if (!item || !isValidTile(item.tile)) {
        if (item) console.warn('tenpai.waits 含非法牌面，已过滤', item.tile);
        continue;
      }
      const remaining = Number.isInteger(item.remaining) ? Math.max(0, Math.min(4, item.remaining)) : 0;
      waits.push({ tile: item.tile, remaining, yaku: item.yaku !== false });
    }
    const isTenpai = typeof frame.is_tenpai === 'boolean' ? frame.is_tenpai : waits.length > 0;
    state.tenpai = {
      isTenpai,
      waits: isTenpai ? waits : [],       // 双向一致：false ⇒ []
      noYaku: isTenpai ? !!frame.no_yaku : false,
    };
    emit('tenpai', state.tenpai);
    changed();
  }

  /** match_end：场终（§9.6），此后不再有对局帧 */
  function applyMatchEnd(frame) {
    state.matchEnd = frame;
    state.rankings = Array.isArray(frame.rankings) ? frame.rankings.map((row) => ({ ...row })) : [];
    state.handsPlayed = Number.isInteger(frame.hands_played) ? frame.hands_played : 0;
    state.matchWillContinue = false;
    state.phase = 'ended';
    state.selected = -1;
    state.riichiTiles = [];
    state.riichiArming = false;
    state.windowTile = null;
    state.lastDiscard = null;
    state.claimTarget = null;
    state.tenpai = { isTenpai: false, waits: [], noYaku: false };
    changed('场终');
  }

  function select(index) {
    state.selected = index >= 0 && index < state.hands[0].length ? index : -1;
    changed();
  }

  function reset() {
    state.hands = [[], [], [], []];
    state.handCounts = [HAND_SIZE, HAND_SIZE, HAND_SIZE, HAND_SIZE];
    state.rivers = [[], [], [], []];
    state.melds = [];
    state.meldClaims = [[], [], [], []];
    state.selected = -1;
    state.drawn = null;
    state.riichi = [false, false, false, false];
    state.riichiTiles = [];
    state.riichiArming = false;
    state.windowTile = null;
    state.lastDiscard = null;
    state.claimTarget = null;
    state.tenpai = { isTenpai: false, waits: [], noYaku: false };
    state.tenpaiSeq = null;                       // 新场次：服务端计数器也不再生效（§5.16）
    state.tenpaiFlags = [];
    state.remaining = 0;
    state.turn = 0;
    state.phase = 'idle';
    state.result = null;
    state.resultAt = 0;
    state.winSeat = null;
    state.revealHands = null;
    state.revealWinner = null;
    state.matchWillContinue = false;
    state.matchEnd = null;
    state.rankings = [];
    state.handsPlayed = 0;
    state.lastError = null;
    state.needsSync = false;
    state.protocolWarning = false;
    changed();
  }

  return {
    state, on, emit, changed, select, reset,
    viewSeat, startRound, applySync, drawTile, discardTile, applyActionEvent, applyGameEnd,
    applyMatchEnd, applyTenpai,
    sortHand,
  };
}

const YAKU_NAMES = {
  menzen: '门前清', tanyao: '断幺九', pinfu: '平和', iipeikou: '一杯口', yakuhai: '箭刻',
  rinshan: '岭上开花', sanshoku_doujun: '三色同顺', ittsuu: '一气通贯', toitoi: '对对和',
  sanankou: '三暗刻', honitsu: '混一色', shousangen: '小三元', seven_pairs: '七对子',
  pure_one_suit: '清一色', riichi: '立直', ippatsu: '一发', chankan: '抢杠',
  tenhou: '天和', chiihou: '地和', kokushi_13: '国士十三面', suuankou_tanki: '四暗刻单骑',
  chuuren_junsei: '纯正九莲宝灯', daisuushii: '大四喜', aka_dora: '赤宝牌',
};

const ACTION_LABELS = {
  [ACTION.PONG]: '碰', [ACTION.CHOW]: '吃', [ACTION.KONG]: '杠', [ACTION.ANKAN]: '暗杠',
  [ACTION.KAKAN]: '加杠', [ACTION.WIN]: '和牌', [ACTION.RIICHI]: '立直',
};

const REASON_LABELS = {
  [REASON.WIN_RON]: '荣和', [REASON.WIN_TSUMO]: '自摸', [REASON.DRAW_EXHAUSTED]: '荒庄流局',
};

export function actionLabel(action) {
  return ACTION_LABELS[action] || action;
}

export function reasonLabel(reason) {
  return REASON_LABELS[reason] || reason;
}

/**
 * 手牌重排：把「刚摸到的那张」留在最右（雀魂口径：摸到先摆最右，打完才归位）。
 *
 * 服务端每次 state_sync 都按 §2.2 排序下发，若直接采用，刚摸的牌就会窜进牌列中间。
 * 这里从旧手牌末尾把那张取回来，再从新的有序手牌里摘掉一份、接在末尾；
 * 张数没多出来（已经打完了 / 本来就没摸）时原样返回。
 *
 * 杠的补牌也走这条路：杠吃掉了几张手牌，sync 的张数和杠前对不上，
 * 此时只认 draw 帧记下的 `pendingDrawn`（那一帧必在 sync 之前到），不靠张数反推。
 */
function keepDrawnLast(sorted, prev, afterDiscard, pendingDrawn) {
  if (sorted.length <= afterDiscard) return sorted;
  // 丢帧/重连时没有 pendingDrawn，退回旧手牌末张——但它只在张数没变时可信
  const drawn = pendingDrawn
    || (Array.isArray(prev) && prev.length === sorted.length ? prev[prev.length - 1] : null);
  if (!drawn) return sorted;
  const out = [...sorted];
  let index = out.lastIndexOf(drawn);
  if (index < 0) index = out.findIndex((code) => normTile(code) === normTile(drawn));
  if (index < 0 || index === out.length - 1) return out;
  out.splice(index, 1);
  out.push(drawn);
  return out;
}

/**
 * 手牌槽位：**左端固定**，第 i 张中心 x = left + i×列距。
 * drawn >= 0 表示最后那张是刚摸到的：它右移一格空档并上抬（录像实测 33 / 47）。
 * 这样 13 张和 14 张的左端是同一个像素，摸打时整排不会左右跳。
 */
export function handLayout(count = HAND.count, drawn = -1) {
  return Array.from({ length: count }, (_, i) => ({
    x: HAND.left + i * (HAND.w + HAND.gap) + (i === drawn ? HAND.drawGap : 0),
    y: HAND.cy - (i === drawn ? HAND.drawLift : 0),
  }));
}

/**
 * 刚摸到那张的下标；没有摸牌窗口时返回 -1。
 *
 * 不能只看 state.drawn：重连/state_sync 之后手牌是 14 张但 drawn 已经丢了，
 * 那张牌就不该再贴着手牌排。按「副露 k 组 → 打完后手牌 13-3k 张」反推，
 * 多出来的那一张就是刚摸的（庄家开局 14 张同理）。
 */
export function drawnIndex(state) {
  const hand = state.hand || [];
  if (!hand.length) return -1;
  // 没有「刚摸到的那张」= 还没摸牌（庄家开局的第 14 张也按平放，不抬起来）
  if (state.drawn == null) return -1;
  const melds = (state.melds || []).filter((meld) => meld.seat === 0).length;
  const afterDiscard = 13 - 3 * melds;
  return hand.length > afterDiscard ? hand.length - 1 : -1;
}
