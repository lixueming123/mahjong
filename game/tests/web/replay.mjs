// 回放播放器验收（replay.js）。
//
// 三条主线：
//   1. 帧驱动：同一份回放文件喂进去，最终牌桌状态必须和"最后一步"一致（牌河/副露/余牌/持点）。
//   2. 视角：切座位会按私有帧过滤重放，`state.mySeat` 与屏幕座位换算都要跟着变。
//   3. 透视：`revealHands` 按屏幕座位索引，内容取自文件的手牌轨道（线上帧推不出来的那一份）。
//
// 夹具是手写的（不是录出来的），因为它要覆盖一个真实对局很难凑齐的形状：两家副露、
// 一条私有 draw 只发给某一家、一条广播 discard_event，以及一条带 `t` 间隔的时间轴。
// 运行：node game/tests/web/replay.mjs

import { FRAME_TYPE, ACTION, WINDOW, MELD_TYPE } from '../../web/js/protocol_enums.js';

// ---- 最小 DOM（client.js 的构造与 buttons 绑定要它）----
const mk = (label = '') => ({
  textContent: label, disabled: false, hidden: false, dataset: {}, style: {},
  classList: { toggle() {}, add() {}, remove() {} },
  addEventListener(t, f) { (this._h ||= {})[t] = f; },
  click() { this._h?.click?.(); },
});
const labels = ['吃', '碰', '杠', '立直', '和', '过', '打出', '摸牌'];
const buttons = labels.map((l) => mk(l));
const bar = { querySelectorAll: () => buttons };
const byId = {};
globalThis.document = {
  getElementById(id) { return (byId[id] ||= mk()); },
  querySelector(sel) {
    if (sel === '.action-bar') return bar;
    if (sel.startsWith('#seat-')) return mk();
    return null;
  },
};
globalThis.window = globalThis.window || { addEventListener() {}, removeEventListener() {} };
globalThis.performance = globalThis.performance || { now: () => Date.now() };

const { createState } = await import('../../web/js/state.js');
const { ReplayPlayer } = await import('../../web/js/replay.js');

/** 一份 1 局、两人对局形状的回放（4 个座位，座位 1/3 是机器人）。 */
function fixture() {
  // §5.18: frames are spliced into the file as JSON objects, not wrapped in strings.
  const frame = (value) => value;
  const seats = [
    { seat: 0, player_id: 'u-000042', name: '小明', is_bot: false, account_id: 42 },
    { seat: 1, player_id: 'bot-1', name: 'AI-1', is_bot: true, account_id: 0 },
    { seat: 2, player_id: 'bot-2', name: 'AI-2', is_bot: true, account_id: 0 },
    { seat: 3, player_id: 'bot-3', name: 'AI-3', is_bot: true, account_id: 0 },
  ];
  const start = {
    type: FRAME_TYPE.GAME_START,
    seat: 0,
    dealer: 0,
    round_index: 0,
    round_count: 4,
    match_total: [0, 0, 0, 0],
    renchan_count: 0,
    hand: ['1m', '2m', '3m', '4m', '5m', '6p', '7p', '8p', '1s', '2s', '3s', '1z', '1z', '9s'],
  };
  // state_sync 是**每座位私有**的一份（§5.9）：`hand` 与顶层 `melds` 只对收件人有效。
  // 录制里四家各一份（真实对局中只有真人座位有 wire），回放按视角挑自己那份。
  const syncFor = (seat, fields) => {
    const view = { ...fields };
    return {
      type: FRAME_TYPE.STATE_SYNC,
      seq: view.seq,
      remaining: 69,
      current_seat: view.current_seat,
      hand: view.hand ?? [],
      melds: view.melds ?? [],
      seats: view.seats,
    };
  };
  const publicSeats = (overrides = {}) => [0, 1, 2, 3].map((seat) => ({
    seat,
    name: seats[seat].name,
    is_bot: seats[seat].is_bot,
    hand_count: 13,
    discards: [],
    melds: [],
    disconnected: false,
    ...(overrides[seat] || {}),
  }));
  // 开局后座位 0 摸牌：14 张，其他人 13 张
  const dealtSeats = publicSeats({
    0: { hand_count: 14 },
    1: { hand_count: 13 },
    2: { hand_count: 13 },
    3: { hand_count: 13 },
  });
  const sync0 = syncFor(0, { seq: 1, current_seat: 0, hand: start.hand, seats: dealtSeats });
  // 座位 1 的那一份：手牌是它自己的，其余三家只剩张数。
  const sync1 = syncFor(1, {
    seq: 1,
    current_seat: 0,
    hand: ['1s', '2s', '3s', '4p', '5p', '6p', '7s', '8s', '9s', '2z', '2z', '5z', '6z'],
    seats: dealtSeats,
  });
  // 座位 0 打出 9s：广播给四家
  const discard = { type: FRAME_TYPE.DISCARD_EVENT, seat: 0, tile: '9s', seq: 2 };
  // 座位 1 摸牌的私有帧（只有座位 1 收到）
  const draw1 = { type: FRAME_TYPE.DRAW, tile: '7z', seat: 1, seq: 3 };
  // 座位 1 碰（广播）
  const pong = {
    type: FRAME_TYPE.ACTION_EVENT,
    seat: 1,
    action: ACTION.PONG,
    tiles: ['9s', '9s', '9s'],
    from_seat: 0,
    seq: 3,
  };
  const syncAfterPong = syncFor(0, {
    seq: 3,
    current_seat: 1,
    hand: ['1m', '2m', '3m', '4m', '5m', '6p', '7p', '8p', '1s', '2s', '3s', '1z', '1z'],
    // 被鸣的那张已经从牌河移除（服务端在鸣牌生效时就摘掉了，state.js 的注释同样口径）。
    seats: publicSeats({
      0: { hand_count: 13 },
      1: { hand_count: 10, melds: [{ type: MELD_TYPE.PONG, tiles: ['9s', '9s', '9s'] }] },
    }),
  });
  const syncAfterPong1 = syncFor(1, {
    seq: 3,
    current_seat: 1,
    hand: ['1s', '2s', '3s', '4p', '5p', '6p', '7s', '8s', '2z', '2z'],
    melds: [{ type: MELD_TYPE.PONG, tiles: ['9s', '9s', '9s'] }],
    seats: publicSeats({
      0: { hand_count: 13 },
      1: {
        hand_count: 10,
        melds: [{ type: MELD_TYPE.PONG, tiles: ['9s', '9s', '9s'] }],
      },
    }),
  });
  const end = {
    type: FRAME_TYPE.GAME_END,
    reason: 3,
    round_index: 0,
    dealer: 0,
    renchan_count: 0,
    match_will_continue: false,
    interlude_ms: 0,
    results: [
      { seat: 0, delta_score: 0, hand: start.hand, win_type: [], match_total: 0 },
      { seat: 1, delta_score: 0, hand: [], win_type: [], match_total: 0 },
      { seat: 2, delta_score: 0, hand: [], win_type: [], match_total: 0 },
      { seat: 3, delta_score: 0, hand: [], win_type: [], match_total: 0 },
    ],
    win: null,
  };
  return {
    format: 'mahjong-replay',
    version: 1,
    room_id: 'r-0000000000001',
    game_id: 'riichi',
    seed: '1',
    options_json: {},
    started_at: 0,
    seats,
    finished: true,
    match_over: true,
    totals: [0, 0, 0, 0],
    final_rank: [1, 2, 3, 4],
    steps: [
      // 开局：game_start 逐座位下发（座位 0 与座位 1 各一份，模拟两个真人座位）
      { t: 0, h: ['1m,2m,3m,4m,5m,6p,7p,8p,1s,2s,3s,1z,1z,9s', '', '', ''], f: [[[0], frame(start)]] },
      // 摸牌后 state_sync：四家各一份，这里录了 0 与 1 两份
      { t: 100, h: ['', '', '', ''], f: [[[0], frame(sync0)], [[1], frame(sync1)]] },
      // 打牌：广播
      { t: 200, h: ['1m,2m,3m,4m,5m,6p,7p,8p,1s,2s,3s,1z,1z', '', '', ''], f: [[[0, 1, 2, 3], frame(discard)]] },
      // 座位 1 摸牌：私有
      { t: 300, h: ['', '1s,2s,3s', '', ''], f: [[[1], frame(draw1)]] },
      // 座位 1 碰：广播
      { t: 400, h: ['', '', '', ''], f: [[[0, 1, 2, 3], frame(pong)]] },
      // 碰后 state_sync：四家各一份，这里录了 0 与 1 两份
      {
        t: 500,
        h: ['', '1s,2s,3s,4p,5p,6p,7s,8s,2z,2z', '', ''],
        f: [[[0], frame(syncAfterPong)], [[1], frame(syncAfterPong1)]],
      },
      { t: 600, h: ['', '', '', ''], f: [[[0, 1, 2, 3], frame(end)]] },
    ],
  };
}

let pass = 0;
let fail = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a === e) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name}\n       got ${a}\n       exp ${e}`); }
};

// ---------------------------------------------------------------- 1. 帧驱动

const app = createState();
const player = new ReplayPlayer({ app, ui: null, sfx: null, replay: fixture(), seat: 0 });
player.seekTo(player.steps.length);

// 自己打出的 9s 被下家碰走了：牌河里没有它，被鸣的那张记在本家的 meldClaims 里
// （渲染层用它决定副露里哪一张横放）。
check('座位 0 视角：打出的 9s 已被鸣走，牌河为空', app.state.rivers[0], []);
check('座位 0 视角：碰是下家（屏幕座位 1）的副露',
  app.state.melds.map((m) => ({ seat: m.seat, type: m.type })), [{ seat: 1, type: MELD_TYPE.PONG }]);
check('座位 0 视角：余牌跟随最后一份 state_sync', app.state.remaining, 69);
check('座位 0 视角：本家手牌最后是 13 张', app.state.hands[0].length, 13);

// ---------------------------------------------------------------- 2. 切视角

player.setSeat(1);
check('切到座位 1：mySeat 跟着变', app.state.mySeat, 1);
// 座位 0 在座位 1 眼里是「上家」= 屏幕座位 3；它打出的 9s 已被碰走，所以牌河是空的，
// 而"被鸣的那张"记在 meldClaims 里（渲染层用它决定横放哪一张）。
check('座位 1 视角：9s 已被鸣走，上家牌河为空', app.state.rivers[3], []);
check('座位 1 视角：被鸣的 9s 记在本家 meldClaims 里', app.state.meldClaims[0], ['9s']);
check('座位 1 视角：碰是本家副露', app.state.melds.map((m) => m.seat), [0]);
// 座位 1 在录制里有一份自己的 state_sync，所以它有线；下面再用座位 2 覆盖
// 「机器人没有 wire」那条路径。

// ---------------------------------------------------------------- 2b. 没有 wire 的座位

// 座位 2 在录制里既没有 game_start 也没有 state_sync —— 真实对局里机器人就是这样，服务端
// 从不为它构建私有帧。所以它的视角有两种情况，都要能正确：
{
  // 轨道也没有 → 空牌桌（不伪造别人的手牌）
  const noTrack = fixture();
  for (const step of noTrack.steps) step.h = ['', '', '', ''];
  const bare = new ReplayPlayer({ app: createState(), ui: null, sfx: null, replay: noTrack, seat: 2 });
  bare.seekTo(bare.steps.length);
  check('无轨道 + 无私有帧的座位：本家手牌为空（不伪造）', bare.app.state.hands[0].length, 0);

  // 轨道有货（真实录制里一定有：Game::ReplayHands 每次 state_sync 都记四家）→ 补回手牌。
  const withTrack = fixture();
  // 给座位 2 补一条自己的轨道，模拟真实录制。
  withTrack.steps[1].h[2] = '2m,3m,4m,5p,6p,7p,8s,9s,1z,2z,3z,5z,6z';
  withTrack.steps[5].h[2] = '2m,3m,4m,5p,6p,7p,8s,9s,1z,2z,3z';
  const tracked = new ReplayPlayer({ app: createState(), ui: null, sfx: null, replay: withTrack, seat: 2 });
  tracked.seekTo(tracked.steps.length);
  check('无私有帧但轨道有货的座位：本家手牌由轨道补回', tracked.app.state.hands[0].length, 11);
  check('补回的手牌内容就是轨道里的那一份',
    tracked.app.state.hands[0].slice(0, 3), ['2m', '3m', '4m']);

  // 有私有帧的座位以帧为准：把轨道清空，手牌照样正确。
  const seatOne = new ReplayPlayer({ app: createState(), ui: null, sfx: null, replay: fixture(), seat: 1 });
  for (const step of seatOne.steps) step.h = ['', '', '', ''];
  seatOne.seekTo(seatOne.steps.length);
  check('有私有帧的座位：手牌照常来自帧（轨道缺席也不影响）', seatOne.app.state.hands[0].length > 0, true);
}

// ---------------------------------------------------------------- 3. 透视

player.setSeat(0);
player.setGodMode(true);
player.seekTo(player.steps.length);
// 手牌轨道按协议座位存，revealHands 按屏幕座位存；座位 0 视角下两者一致。
// 座位 0 是 13 张、座位 1 是 10 张（碰后），座位 2/3 从没录过手牌轨道 → null（不是空数组）。
check('透视：四家手牌按屏幕座位给出，只显示录过的那两家',
  app.state.revealHands.map((h) => (h ? h.length : 0)), [13, 10, 0, 0]);
check('透视：座位 0 的手牌内容来自手牌轨道', app.state.revealHands[0].slice(0, 3), ['1m', '2m', '3m']);
check('透视：不是和牌者高亮（不抢结算页金条）', app.state.revealWinner, null);

player.setGodMode(false);
check('关掉透视：手牌恢复牌背', app.state.revealHands, null);

// ---------------------------------------------------------------- 4. 批量推进只渲染一次

let changeCount = 0;
app.on('change', () => { changeCount += 1; });
const before = changeCount;
player.seekTo(0);
player.seekTo(player.steps.length);
check('一次 seek 只发一次 change 事件（批处理生效）', changeCount - before, 2);

// ---------------------------------------------------------------- 5. 单步

player.seekTo(0);
const stepCount = player.steps.length;
let forward = 0;
while (player.stepForward()) forward += 1;
check('单步走完所有步', forward, stepCount);
check('单步之后 cursor 在末尾', player.cursor, stepCount);

// ---------------------------------------------------------------- 6. 倍速

check('倍速只接受档位内的值', player.setSpeed(3), 1);
check('倍速接受 4x', player.setSpeed(4), 4);

// ---------------------------------------------------------------- 7. 旧式字符串帧仍可读

// §5.18 的帧现在直接拼接成对象（replay.cc），但把帧包成字符串的旧文件仍然存在，
// 播放器两种形状都接受 —— 否则一次格式收紧就会让所有历史回放失效。
{
  const legacy = fixture();
  for (const step of legacy.steps) {
    step.f = step.f.map(([recipients, payload]) => [
      recipients,
      typeof payload === 'string' ? payload : JSON.stringify(payload),
    ]);
  }
  const old = new ReplayPlayer({ app: createState(), ui: null, sfx: null, replay: legacy, seat: 0 });
  old.seekTo(old.steps.length);
  check('字符串形式的帧仍能重放（向后兼容）', old.app.state.rivers[1].length, 0);
  check('字符串帧重放后本家手牌正确', old.app.state.hands[0].length, 13);
}

// ---------------------------------------------------------------- 8. 计时推进不吞渲染

// 批处理是「suspend 就必须 resume」的一对。曾经的写法是 suspend 无条件、resume 只在
// 「这一拍真的消费了帧」时才调用 —— 1× 下每步约 1 秒而计时器 50ms 一拍，绝大多数拍都是空拍，
// 于是第一次空拍就让 batchDepth 永久 >0，之后 changed() 全部被吞：控制条在走、牌桌不动。
{
  // 8a. 已经播完（没有步可消费）时反复 tick，之后必须仍然能渲染。
  const endApp = createState();
  const ended = new ReplayPlayer({ app: endApp, ui: null, sfx: null, replay: fixture(), seat: 0 });
  ended.seekTo(ended.steps.length);
  let endChanges = 0;
  endApp.on('change', () => { endChanges += 1; });
  for (let index = 0; index < 5; index += 1) ended._advanceByClock();
  check('播完后的空拍不会吞掉 change（batchDepth 不泄漏）', endChanges, 0);
  endApp.changed();
  check('播完之后 changed 仍能穿透', endChanges, 1);

  // 8b. 播放途中的空拍同样不能泄漏；跨过整条时间轴后才渲染一次。
  //     把时间轴拉开，保证每个 tick 都落在两步之间（真实的 1× 就是这个节奏）。
  //     刻意不调用 play()：那会起一个真实的 setInterval，和这里的手动推进互相干扰。
  const slow = fixture();
  for (const step of slow.steps) step.t *= 100000;
  const tickApp = createState();
  const ticking = new ReplayPlayer({ app: tickApp, ui: null, sfx: null, replay: slow, seat: 0 });
  // seekTo(1)：第 0 步已消费，下一步在 10,000,000ms 之后 —— 50ms 的 tick 追不上它。
  ticking.seekTo(1);
  ticking.clockMs = 0;
  ticking._lastFrameAt = 0;
  let tickChanges = 0;
  tickApp.on('change', () => { tickChanges += 1; });

  for (let index = 0; index < 30; index += 1) ticking._advanceByClock();  // 全是空拍
  check('空拍不推进 cursor', ticking.cursor, 1);
  check('空拍不发 change（不白重建场景）', tickChanges, 0);

  // 批处理没泄漏：手动 changed() 能穿透到渲染层（这正是上面 bug 的表现所在）。
  tickApp.changed();
  check('空拍之后仍能渲染', tickChanges, 1);

  // 把时钟推过整条时间轴：这一次必须真的渲染，且 cursor 走到底。
  ticking.clockMs = 1e12;
  ticking._advanceByClock();
  check('推进到末尾后 cursor 到末尾', ticking.cursor, ticking.steps.length);
  check('真正消费了帧就再渲染一次', tickChanges, 2);
}

// ---------------------------------------------------------------- 9. 切到 AI 视角时另外三家也要有牌背

// 录制里只有真人座位有 wire。从 AI 视角重放时，另外三家（含原本的真人座位 0）都没有
// 私有 state_sync，`hands[1..3]` 会留空 —— 渲染层按长度摆牌，于是三家一张牌都不画，
// 牌桌只剩自己一手牌。轨道里记着四家张数，必须据此把牌背占位补齐。
{
  const viewApp = createState();
  const fromBot = new ReplayPlayer({ app: viewApp, ui: null, sfx: null, replay: fixture(), seat: 2 });
  fromBot.seekTo(fromBot.steps.length);
  const state = viewApp.state;
  // 座位 0（本家）在座位 2 眼里是屏幕座位 2（对面）：13 张牌背
  check('AI 视角：对面（原真人座位）也有牌背', state.hands[2].length, 13);
  check('AI 视角：牌背占位是 back', state.hands[2].every((tile) => tile === 'back'), true);
  // 座位 1 在座位 2 眼里是屏幕座位 3：10 张（碰后）
  check('AI 视角：上家牌背张数跟随轨道', state.hands[3].length, 10);
  // 屏幕座位映射（视角 = 协议座位 2）：2→0(本家)、3→1、0→2、1→3。
  // 座位 3 的轨道从没出现过 → 保持 game_start 给的 13 张牌背（真实对局里对手总有牌背，
  // 空桌反而是错的）；其余三家由轨道覆盖。张数仍以轨道为准，见下一个断言。
  check('AI 视角：另三家的张数与轨道一致', state.hands.map((hand) => hand.length), [0, 13, 13, 10]);
  check('AI 视角：handCounts 与 hands 一致',
    state.handCounts, state.hands.map((hand) => hand.length));

  // 张数要跟着步进变化，而不是停在切视角那一刻。
  fromBot.seekTo(2);
  const early = state.hands[3].length;
  fromBot.seekTo(fromBot.steps.length);
  check('AI 视角：牌背张数随进度变化', early !== state.hands[3].length, true);
}

// ---------------------------------------------------------------- 10. AI 视角必须按局重置

// `game_start` 是按座位构造的，机器人没有 endpoint 所以服务端从不为它构造这一帧 —— 录下来
// 的几份都发给了真人座位。播放器若不借用，本局就不会重置：牌河与局次跨局累加。在真实录制
// 上表现为「切到 AI 视角后四家各有 18 张牌河、余牌 0」。
{
  const twoRounds = fixture();
  // 造第二局：同一批帧再走一遍，中间夹一个 game_end（广播，所有座位都收到）。
  const endAt = twoRounds.steps.length;
  twoRounds.steps.push({ t: 700, h: ['', '', '', ''], f: [[[0, 1, 2, 3], {
    type: FRAME_TYPE.GAME_END,
    reason: 3,
    round_index: 0,
    dealer: 0,
    renchan_count: 0,
    match_will_continue: true,
    interlude_ms: 0,
    results: [0, 1, 2, 3].map((seat) => ({ seat, delta_score: 0, hand: [], win_type: [], match_total: 0 })),
    win: null,
  }]] });
  // 第二局的 game_start 只发给座位 0（真实录制就是这个形状）。
  const secondStart = { ...twoRounds.steps[0].f[0][1], round_index: 1 };
  twoRounds.steps.push({ t: 800, h: ['1m,2m,3m,4m,5m,6p,7p,8p,1s,2s,3s,1z,1z,9s', '', '', ''],
    f: [[[0], secondStart]] });
  twoRounds.steps.push({ t: 900, h: ['1m,2m,3m,4m,5m,6p,7p,8p,1s,2s,3s,1z,1z', '', '', ''],
    f: [[[0, 1, 2, 3], { type: FRAME_TYPE.DISCARD_EVENT, seat: 0, tile: '9s', seq: 20 }]] });

  const roundApp = createState();
  const bot = new ReplayPlayer({ app: roundApp, ui: null, sfx: null, replay: twoRounds, seat: 2 });
  bot.seekTo(bot.steps.length);
  const state = roundApp.state;
  // 第二局只打了 1 张牌：座位 0 在座位 2 眼里是屏幕座位 2，牌河必须是 1 张，不是两局累加。
  check('AI 视角：第二局不继承第一局的牌河', state.rivers[2].length, 1);
  check('AI 视角：局次前进了（借用 game_start 生效）', state.roundIndex, 1);
  check('AI 视角：mySeat 没被别人的 game_start 改写', state.mySeat, 2);
  // 第二局里座位 2 没有新的轨道记录（真实录制中它是机器人），所以本家手牌是空的 ——
  // 关键是它**不继承**第一局的 11 张，否则换局后桌上会挂着上一局的牌。
  check('AI 视角：本家手牌不跨局沿用', state.hands[0].length, 0);
}

// 录制里只有真人座位有 wire。从 AI 视角重放时，另外三家（含原本的真人座位 0）都没有
// 私有 state_sync，`hands[1..3]` 会留空 —— 渲染层按长度摆牌，于是三家一张牌都不画，
// 牌桌只剩自己一手牌。轨道里记着四家张数，必须据此把牌背占位补齐。
{
  const viewApp = createState();
  const fromBot = new ReplayPlayer({ app: viewApp, ui: null, sfx: null, replay: fixture(), seat: 2 });
  fromBot.seekTo(fromBot.steps.length);
  const state = viewApp.state;
  // 座位 0（本家）在座位 2 眼里是屏幕座位 2（对面）：13 张牌背
  check('AI 视角：对面（原真人座位）也有牌背', state.hands[2].length, 13);
  check('AI 视角：牌背占位是 back', state.hands[2].every((tile) => tile === 'back'), true);
  // 座位 1 在座位 2 眼里是屏幕座位 3：10 张（碰后）
  check('AI 视角：上家牌背张数跟随轨道', state.hands[3].length, 10);
  // 屏幕座位映射（视角 = 协议座位 2）：2→0(本家)、3→1、0→2、1→3。
  // 座位 3 的轨道从没出现过 → 保持 game_start 给的 13 张牌背（真实对局里对手总有牌背，
  // 空桌反而是错的）；其余三家由轨道覆盖。张数仍以轨道为准，见下一个断言。
  check('AI 视角：另三家的张数与轨道一致', state.hands.map((hand) => hand.length), [0, 13, 13, 10]);
  check('AI 视角：handCounts 与 hands 一致',
    state.handCounts, state.hands.map((hand) => hand.length));

  // 张数要跟着步进变化，而不是停在切视角那一刻。
  fromBot.seekTo(2);
  const early = state.hands[3].length;
  fromBot.seekTo(fromBot.steps.length);
  check('AI 视角：牌背张数随进度变化', early !== state.hands[3].length, true);
}

// ---------------------------------------------------------------- 11. 局边界（进度条分段）

// 「442 步」在时间轴上是一局一局拼起来的。控制条按 `game_start` 的位置画分隔线，
// 玩家才看得出"这一局还有多长"。这里验证切分口径。
{
  const multi = fixture();
  const startFrame = multi.steps[0].f[0][1];
  // 追加第二局与第三局：每局开头一份 game_start（只发给座位 0，真实录制就是这个形状）。
  multi.steps.push({ t: 700, h: ['', '', '', ''], f: [[[0], { ...startFrame, round_index: 0, dealer: 1 }]] });
  multi.steps.push({ t: 800, h: ['', '', '', ''], f: [[[0], { ...startFrame, round_index: 1, dealer: 0 }]] });
  const rounds = new ReplayPlayer({ app: createState(), ui: null, sfx: null, replay: multi, seat: 0 });
  // fixture 原有 7 步，追加的两局落在下标 7、8。
  check('局边界按 game_start 切分', rounds._rounds.map((round) => round.index), [0, 7, 8]);
  // round_index 是整场局序号（0 基、连庄不增）：0→東1、1→東2。
  // 第二段我特意用 `round_index: 0, dealer: 1`（连庄：同一局号、庄家变了）→ 仍标 東1。
  check('局名由 round_index 决定，连庄不进阶', rounds._rounds.map((round) => round.label), ['東1', '東1', '東2']);

  // 南场：round_index 4 起就是南1（一圈 4 局）。
  const nanba = new ReplayPlayer({ app: createState(), ui: null, sfx: null, replay: {
    ...multi,
    steps: [{ t: 0, h: ['', '', '', ''], f: [[[0], { ...startFrame, round_index: 4, dealer: 0 }]] }],
  }, seat: 0 });
  check('第二圈是南风', nanba._rounds[0].label, '南1');

  // 当前局随光标移动：cursor < 7 在第 1 局，7 起在第 2 局。
  rounds.seekTo(0);
  check('光标在开头 → 第 1 局', rounds.round.index, 0);
  rounds.seekTo(6);
  check('第 1 局末尾仍是第 1 局', rounds.round.index, 0);
  rounds.seekTo(7);
  check('跨过边界 → 第 2 局', rounds.round.index, 1);
  check('局总数', rounds.round.total, 3);
  check('局标签跟着走（第 2 段是连庄，仍标 東1）', rounds.round.label, '東1');
  rounds.seekTo(8);
  check('第三段进到東2', rounds.round.label, '東2');

  // 没有 game_start 的文件退化成单段，进度条不崩。
  const noStart = fixture();
  for (const step of noStart.steps) {
    step.f = step.f.filter(([, payload]) => payload.type !== FRAME_TYPE.GAME_START);
  }
  const bare = new ReplayPlayer({ app: createState(), ui: null, sfx: null, replay: noStart, seat: 0 });
  check('无 game_start 时退化成单段', bare._rounds.length, 1);
  check('单段从第 0 步开始', bare._rounds[0].index, 0);
}

// ---------------------------------------------------------------- 12. 局终结算页的显示判据

// 结算页由「光标是否停在 game_end 那一步」决定，而不是"刚收到帧" —— 否则拖动进度条
// 来回扫会疯狂开关浮层。这里验证判据本身，以及 seek 期间不会逐局回调。
{
  const app2 = createState();
  const events = [];
  const endPlayer = new ReplayPlayer({ app: app2, ui: null, sfx: null, replay: fixture(), seat: 0 });
  endPlayer.onRoundEnd = (frame, info) => events.push(info.showing);

  // fixture 的最后一步是 game_end（下标 last-1 之后）。
  const endIndex = endPlayer.steps.length - 1;
  events.length = 0;
  endPlayer.seekTo(endPlayer.steps.length);
  check('跳到末尾停在局终', endPlayer.atRoundEnd, true);
  check('seek 期间不逐局回调，只按落点通知一次', events, [true]);

  events.length = 0;
  endPlayer.seekTo(0);
  check('跳回开头不在局终', endPlayer.atRoundEnd, false);
  check('离开局终会收起', events, [false]);

  // 单步走到局终也要显示（这是「逐手复盘」最常用的一条路径）。
  // fixture 的 game_end 在最后一步，所以 seekTo(steps.length) 就停在它上面。
  events.length = 0;
  endPlayer.seekTo(endIndex);
  check('停在局终前一步不在局终', endPlayer.atRoundEnd, false);
  endPlayer.stepForward();
  check('单步走到局终就显示', endPlayer.atRoundEnd, true);
  check('单步也通知', events[events.length - 1], true);

  // 跨过局终进入新一局后，不能还认为"停在局终"。
  // 早先 `_endStepIndex` 不清，`cursor-1` 恰好指到上一局的 game_end，结算页会一直挂着。
  const nextRound = fixture();
  const startOfRound = nextRound.steps[0].f[0][1];
  nextRound.steps.push({ t: 700, h: ['', '', '', ''], f: [[[0], { ...startOfRound, round_index: 1 }]] });
  const across = new ReplayPlayer({ app: createState(), ui: null, sfx: null, replay: nextRound, seat: 0 });
  across.seekTo(across.steps.length);
  check('新一局开局后不再算停在局终', across.atRoundEnd, false);
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);