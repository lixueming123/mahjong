// 针对「对照 game/client 补齐的逻辑」做合成帧断言：不连服务端、不开浏览器。
import { FRAME_TYPE, REASON, ERROR_CODE } from '../../web/js/protocol_enums.js';

const mk = () => ({
  textContent: '', disabled: false, hidden: false, dataset: {}, style: {},
  classList: { toggle() {}, add() {}, remove() {} },
  addEventListener() {}, click() {},
});
const labels = ['吃', '碰', '杠', '立直', '和', '过', '打出', '摸牌'];
const buttons = labels.map(() => mk());
const byLabel = Object.fromEntries(labels.map((l, i) => [l, buttons[i]]));
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

const { createState } = await import('../../web/js/state.js');
const { MahjongClient } = await import('../../web/js/client.js');

let passed = 0, failed = 0;
function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a === e) { passed++; console.log('  ok  ', name); }
  else { failed++; console.log('  FAIL', name, '\n    got:', a, '\n    want:', e); }
}

function makeClient(options = {}) {
  const app = createState();
  const client = new MahjongClient({ app, ui: { update() {}, el: {} }, sfx: {}, url: 'ws://test', options });
  const sent = [];
  client.net.connected = true;
  client.net.send = (frame) => { sent.push(frame); return true; };
  return { app, client, sent };
}

// ---------- 1. 登录帧（§4.1） ----------
{
  const { client } = makeClient({ name: '  a\nb  ' });
  check('游客昵称去掉控制字符并 trim', client.authFrame(), { type: FRAME_TYPE.LOGIN, name: 'ab' });
}
{
  const { client } = makeClient({ user: 'alice', pass: 's3cret', name: '爱丽丝' });
  check('账号形态（出现 username 即账号登录）', client.authFrame(),
    { type: FRAME_TYPE.LOGIN, username: 'alice', password: 's3cret', name: '爱丽丝' });
}
{
  const { client } = makeClient({ user: 'bob', pass: 'pw1234', register: true });
  check('注册形态', client.authFrame(), { type: FRAME_TYPE.REGISTER, username: 'bob', password: 'pw1234' });
}
{
  const { client } = makeClient({ name: 'x'.repeat(40) });
  check('昵称截到 16 字符', client.authFrame().name, 'x'.repeat(16));
}

// ---------- 2. 局终 / 场终载荷留档（§5.10 / §5.15） ----------
{
  const { app, client } = makeClient();
  client.handleFrame({
    type: FRAME_TYPE.GAME_END, reason: REASON.WIN_RON, renchan_count: 2, match_will_continue: true,
    results: [
      { seat: 0, delta_score: -1000, match_total: -1000, hand: [] },
      { seat: 1, delta_score: 1000, match_total: 1000, hand: ['1m'] },
      { seat: 2, delta_score: 0, match_total: 0, hand: [] },
      { seat: 3, delta_score: 0, match_total: 0, hand: [] },
    ],
    win: { seat: 1, tile: '3p', tsumo: false, yaku: [{ id: 'tanyao', han: 1 }], han: 1 },
  });
  check('game_end 原始载荷留档', app.state.result?.reason, REASON.WIN_RON);
  check('和牌者座位', app.state.winSeat, 1);
  check('match_will_continue', app.state.matchWillContinue, true);
  check('连庄数进 honba', app.state.honba, 2);
}
{
  const { app, client } = makeClient();
  client.handleFrame({
    type: FRAME_TYPE.MATCH_END, hands_played: 5, renchan_count: 1,
    rankings: [{ seat: 0, rank: 1, final_score: 31000 }, { seat: 1, rank: 2, final_score: 25000 }],
  });
  check('场终名次留档', app.state.rankings.length, 2);
  check('场终局数', app.state.handsPlayed, 5);
  check('场终后不再继续', app.state.matchWillContinue, false);
}

// ---------- 3. 听牌帧过滤非法牌面（§5.16） ----------
{
  const { app, client } = makeClient();
  client.handleFrame({
    type: FRAME_TYPE.TENPAI, tenpai_seq: 1, is_tenpai: true,
    waits: [{ tile: 'zz', remaining: 1 }, { tile: '3p', remaining: 2 }],
  });
  check('tenpai 只保留合法牌面', app.state.tenpai.waits.map((w) => w.tile), ['3p']);
}

// ---------- 4. 隐私字段：seats[].hand 不消费（§8.2） ----------
{
  const { app, client } = makeClient();
  client.handleFrame({
    type: FRAME_TYPE.STATE_SYNC, seq: 3, remaining: 70, current_seat: 1, hand: ['1m', '2m'],
    melds: [],
    seats: [
      { seat: 0, name: '我', hand_count: 2, discards: [], melds: [] },
      { seat: 1, name: 'AI-1', hand_count: 13, discards: ['9p'], melds: [], hand: ['5z'] },
      { seat: 2, name: 'AI-2', hand_count: 13, discards: [], melds: [] },
      { seat: 3, name: 'AI-3', hand_count: 13, discards: [], melds: [] },
    ],
  });
  check('越权 hand 被脱敏（不写入他家手牌）', app.state.hands[1].every((t) => t === 'back'), true);
  check('越权 hand 置协议告警', app.state.protocolWarning, true);
}

// ---------- 5. 本家 discard_event 手牌对不上时不吞广播 ----------
{
  const { app, client } = makeClient();
  app.startRound({ seat: 0, dealer: 0, round_index: 0, round_count: 4, hand: ['1m', '2m'] });
  app.state.remaining = 80;
  const tile = app.discardTile({ seat: 0, tile: '9p' });
  check('discard_event 仍返回牌面', tile, '9p');
  check('牌河照常落牌', app.state.rivers[0], ['9p']);
  check('置位补偿同步', app.state.needsSync, true);
}

// ---------- 6. 错误码分支（§7） ----------
{
  const { app, client } = makeClient();
  client.handleError({ code: ERROR_CODE.RATE_LIMITED, message: 'slow down' });
  check('RATE_LIMITED 本地锁 1 秒', client.rateLimitedUntil > performance.now(), true);
  check('RATE_LIMITED 提示文案', byId['log-text'].textContent.includes('操作过于频繁'), true);
  check('限流期不发帧', client.send({ type: FRAME_TYPE.SYNC }), false);
  client.rateLimitedUntil = 0;
}
{
  const { client } = makeClient();
  client.handleError({ code: ERROR_CODE.ILLEGAL_ACTION, message: 'cannot declare riichi now' });
  check('立直被拒改中文提示', byId['log-text'].textContent.includes('换一张再点立直'), true);
  check('非法操作计数 +1', client.illegalCount, 1);
}
{
  const { app, client } = makeClient();
  app.state.roomId = 'r-1';
  client.room = { roomId: 'r-1' };
  client.roomRequested = true;
  client.handleError({ code: ERROR_CODE.ROOM_NOT_FOUND });
  check('房间不存在清空房间', [client.room, app.state.roomId], [null, null]);
  check('允许重新建房', client.roomRequested, false);
}

// ---------- 7. 空手牌补偿同步 + 心跳不提前发 ----------
{
  const { app, client, sent } = makeClient();
  let heartbeat = 0;
  client.net.startHeartbeat = () => { heartbeat += 1; };
  client.handleFrame({ type: FRAME_TYPE.GAME_START, seat: 0, dealer: 0, round_index: 0, round_count: 4, hand: [] });
  check('login_ok 之前不启动心跳', heartbeat, 0);
  check('空手牌立刻要 sync', sent.some((f) => f.type === FRAME_TYPE.SYNC), true);
  client.handleFrame({ type: FRAME_TYPE.LOGIN_OK, player_id: 'p-x', name: '我' });
  check('login_ok 后启动心跳', heartbeat, 1);
  check('游客不拉战绩', sent.some((f) => f.type === FRAME_TYPE.STATS_REQUEST), false);
}
{
  const { client, sent } = makeClient();
  client.handleFrame({ type: FRAME_TYPE.LOGIN_OK, player_id: 'u-9', name: '账号' });
  check('账号会话拉战绩', sent.some((f) => f.type === FRAME_TYPE.STATS_REQUEST), true);
  check('账号会话标记', client.hasAccount, true);
}

console.log(failed ? `\n失败 ${failed} 条` : `\n新增逻辑断言：ALL PASS  ${passed} passed, 0 failed`);
process.exit(failed ? 1 : 0);
