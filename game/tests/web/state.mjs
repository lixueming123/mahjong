// 状态归约验收：action_event 不拼副露、牌河冻结规则、tenpai_seq 单调、场终
import { ACTION, MELD_TYPE, REASON } from '../../web/js/protocol_enums.js';

const { createState } = await import('../../web/js/state.js');

let pass = 0, fail = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a === e) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name}\n       got ${a}\n       exp ${e}`); }
};

const app = createState();
const s = app.state;
app.startRound({ seat: 0, dealer: 0, round_index: 0, round_count: 4,
  hand: ['1m', '2m', '3m', '4m', '5m', '6p', '7p', '8p', '9p', '1s', '1s', '2z', '2z', '3z'] });

// 1. 碰：牌河移除一张，但副露不在这里拼（等 state_sync）；手牌也不动
s.rivers[1] = ['9s', '4p'];
app.applyActionEvent({ seat: 0, action: ACTION.PONG, tiles: ['4p'], from_seat: 1, seq: 2 });
check('碰：from_seat 牌河弹出被鸣张', s.rivers[1], ['9s']);
check('碰：不伪造副露（等 state_sync）', s.melds, []);
check('碰：手牌不动', s.hands[0].length, 14);

// 2. 吃：同样只动牌河
s.rivers[3] = ['7m'];
app.applyActionEvent({ seat: 0, action: ACTION.CHOW, tiles: ['1m', '2m'], from_seat: 3, seq: 4 });
check('吃：牌河弹出', s.rivers[3], []);
check('吃：不伪造副露', s.melds, []);

// 3. 暗杠/加杠：不动任何牌河（§5.8 规则 4）
s.rivers[2] = ['5z', '6z'];
app.applyActionEvent({ seat: 1, action: ACTION.ANKAN, tiles: [], seq: 6 });
app.applyActionEvent({ seat: 2, action: ACTION.KAKAN, tiles: ['3p'], seq: 8 });
check('暗杠/加杠不动牌河', s.rivers[2], ['5z', '6z']);
check('暗杠不伪造副露', s.melds, []);

// 4. 立直事件：只置公开标记
app.applyActionEvent({ seat: 2, action: ACTION.RIICHI, tiles: [], seq: 10 });
check('立直事件置标记', s.riichi[2], true);

// 5. state_sync：本家副露取顶层（含暗杠真牌面），他家暗杠 tiles=[] 画 4 张牌背
app.applyActionEvent({ seat: 1, action: ACTION.ANKAN, tiles: [], seq: 12 });
app.applySync({ seq: 13, remaining: 70, current_seat: 1,
  hand: ['1m', '2m', '3m', '4m', '5m'],
  melds: [{ type: MELD_TYPE.ANKAN, tiles: ['6p', '6p', '6p', '6p'] }],
  seats: [
    { seat: 0, name: '我', is_bot: false, hand_count: 5, discards: [], melds: [
      { type: MELD_TYPE.ANKAN, tiles: ['6p', '6p', '6p', '6p'] }], disconnected: false },
    { seat: 1, name: 'AI-1', is_bot: true, hand_count: 10, discards: [], melds: [
      { type: MELD_TYPE.ANKAN, tiles: [] }], disconnected: false },
    { seat: 2, name: 'AI-2', is_bot: true, hand_count: 13, discards: [], melds: [], disconnected: false },
    { seat: 3, name: 'AI-3', is_bot: true, hand_count: 13, discards: [], melds: [], disconnected: false },
  ],
  tenpai: [true, false, true, false] });
const ownMeld = s.melds.find((m) => m.seat === 0);
const otherMeld = s.melds.find((m) => m.seat === 1);
check('本家暗杠 4 张真牌面', ownMeld.tiles, ['6p', '6p', '6p', '6p']);
check('他家暗杠 4 张牌背', otherMeld.tiles, ['back', 'back', 'back', 'back']);
check('state_sync 吸收四家听牌标记', s.tenpaiFlags, [true, false, true, false]);
check('本家手牌覆盖', s.hands[0].length, 5);

// 6. tenpai_seq 只认严格递增
app.applyTenpai({ tenpai_seq: 5, is_tenpai: true, waits: [{ tile: '3m', remaining: 2, yaku: true }] });
check('tenpai 生效', { isTenpai: s.tenpai.isTenpai, waits: s.tenpai.waits.length }, { isTenpai: true, waits: 1 });
app.applyTenpai({ tenpai_seq: 4, is_tenpai: true, waits: [{ tile: '9z', remaining: 4 }] });
check('过期 tenpai 帧被丢弃', s.tenpai.waits[0].tile, '3m');
app.applyTenpai({ tenpai_seq: 6, is_tenpai: false, waits: [{ tile: '9z', remaining: 4 }] });
check('is_tenpai=false 清空 waits', s.tenpai, { isTenpai: false, waits: [], noYaku: false });
app.applyTenpai({ tenpai_seq: 7, is_tenpai: true, waits: [{ tile: '5m', remaining: 9 }] });
check('waits.remaining 越界被夹到 4', s.tenpai.waits[0].remaining, 4);

// 7. 和牌事件清听牌 + 局终
app.applyActionEvent({ seat: 0, action: ACTION.WIN, tiles: ['5m'], seq: 20 });
check('和牌清听牌', s.tenpai.isTenpai, false);
check('和牌置终局态', s.phase, 'ended');
app.applyGameEnd({ reason: REASON.WIN_TSUMO, renchan_count: 2, tenpai: [true, false, true, false],
  points_after: [27000, 25000, 24000, 24000],
  results: [{ seat: 0, delta_score: 2000 }, { seat: 1, delta_score: -1000 },
            { seat: 2, delta_score: 0 }, { seat: 3, delta_score: -1000 }] });
check('局终连庄数', s.honba, 2);
check('局终持点', s.scores, [27000, 25000, 24000, 24000]);

// 8. 场终
app.applyMatchEnd({ hands_played: 8, renchan_count: 2,
  rankings: [{ seat: 0, final_score: 32000, rank: 1 }] });
check('场终载荷落地', { phase: s.phase, hands: s.matchEnd.hands_played }, { phase: 'ended', hands: 8 });

console.log(`\n状态归约：${fail ? 'FAILED' : 'ALL PASS'}  ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
