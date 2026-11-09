// Web 客户端 v2.2 恢复路径的最小行为测试（无浏览器、无 socket）。
// 覆盖：login_ok{in_room:true} 不建房/不 ready；room_joined 保留恢复态；game_start 恢复
// match_total/renchan_count；state_sync/action_request 落地；game_end 重放进入结算。

globalThis.document = {
  querySelector: () => null,
  querySelectorAll: () => [],
  getElementById: () => null,
};

const { createState } = await import('../../web/js/state.js');
const { MahjongClient } = await import('../../web/js/client.js');
const { FRAME_TYPE, REASON } = await import('../../web/js/protocol_enums.js');

let pass = 0;
let fail = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a === e) {
    pass += 1;
    console.log(`  ok   ${name}`);
  } else {
    fail += 1;
    console.log(`  FAIL ${name}\n       got ${a}\n       exp ${e}`);
  }
};
const ok = (name, condition) => check(name, !!condition, true);

const app = createState();
const client = new MahjongClient({ app, ui: null, sfx: null, url: 'ws://test', options: {} });
client.credential = { user: 'alice', pass: 'pw' };
let createRoomCalls = 0;
let readyCalls = 0;
client.createRoom = () => { createRoomCalls += 1; return true; };
client.ready = () => { readyCalls += 1; return true; };

const seats = [
  { seat: 0, name: 'Alice', is_bot: false },
  { seat: 1, name: 'AI-1', is_bot: true },
  { seat: 2, name: 'Bob', is_bot: false },
  { seat: 3, name: 'AI-3', is_bot: true },
];

client.handleFrame({
  type: FRAME_TYPE.LOGIN_OK, player_id: 'u-000042', name: 'Alice', in_room: true,
});
check('in_room:true 进入恢复态', client.stage, 'resuming');
ok('in_room:true 设置 resumedInRoom', client.resumedInRoom);
check('login_ok 不自动建房', createRoomCalls, 0);

client.handleFrame({ type: FRAME_TYPE.ROOM_JOINED, room_id: 'r-test', seat: 2, seats });
check('room_joined 保留座位', client.room.seat, 2);
check('恢复模式不自动 ready', readyCalls, 0);
check('恢复模式 autoReady=false', client.autoReady, false);

client.handleFrame({
  type: FRAME_TYPE.GAME_START, seat: 2, dealer: 0, round_index: 1, round_count: 4,
  match_total: [-8, 8, 0, 0], renchan_count: 1,
  hand: ['1m', '2m', '3m', '4m', '5m', '6p', '7p', '8p', '9p', '1s', '1s', '2z', '2z'],
});
check('game_start 恢复累计分', app.state.matchTotal, [-8, 8, 0, 0]);
check('game_start 恢复连庄数', app.state.honba, 1);
check('game_start 进入对局', app.state.phase, 'playing');

client.handleFrame({
  type: FRAME_TYPE.STATE_SYNC, seq: 7, remaining: 60, current_seat: 0,
  hand: ['1m', '2m', '3m', '4m', '5m', '6p', '7p', '8p', '9p', '1s', '1s', '2z', '2z'],
  melds: [],
  seats: seats.map((seat) => ({
    ...seat, hand_count: seat.seat === 2 ? 13 : 13, discards: [], melds: [], disconnected: false,
  })),
});
check('state_sync 恢复手牌', app.state.hands[0].length, 13);
check('state_sync 恢复牌山', app.state.remaining, 60);

client.handleFrame({
  type: FRAME_TYPE.ACTION_REQUEST, seq: 7, deadline_ms: Date.now() + 10000, window: 1, options: [],
});
ok('action_request 恢复窗口', client.window && client.window.seq === 7);

client.handleFrame({
  type: FRAME_TYPE.GAME_END, reason: REASON.WIN_RON, round_index: 1, dealer: 0,
  renchan_count: 1, match_will_continue: true, interlude_ms: 5000,
  results: [
    { seat: 0, delta_score: 0, match_total: -8, hand: [], win_type: [] },
    { seat: 1, delta_score: 0, match_total: 8, hand: [], win_type: [] },
    { seat: 2, delta_score: 0, match_total: 0, hand: [], win_type: [] },
    { seat: 3, delta_score: 0, match_total: 0, hand: [], win_type: [] },
  ],
  win: { seat: 1, tile: '6p', from_seat: 3, tsumo: false, tags: [], multiplier: 1, per_payer: 1,
    total: 1, yaku: [], han_breakdown: { base: 0, tsumo: 0, dealer: 0, total: 0 }, han: 0,
    dealer_bonus: false, rinshan: false },
});
check('game_end 重放进入结算', app.state.phase, 'ended');
ok('game_end 重放保留载荷', app.state.result && app.state.result.round_index === 1);

client.net.stopHeartbeat();
console.log(`\nWeb 恢复路径：${fail ? 'FAILED' : 'ALL PASS'}  ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
