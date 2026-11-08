import { createState, drawnIndex, sortHand } from '../../web/js/state.js';
import { ACTION, MELD_TYPE } from '../../web/js/protocol_enums.js';

let pass = 0; let fail = 0;
const ok = (name, cond, extra = '') => {
  if (cond) { pass++; console.log('  ok  ' + name); }
  else { fail++; console.log('  FAIL ' + name + (extra ? ' :: ' + extra : '')); }
};

const seatFrame = (bySeat = []) => ({
  seats: [0, 1, 2, 3].map((seat) => ({
    seat,
    name: ['我', 'A', 'B', 'C'][seat],
    hand_count: 13,
    melds: bySeat[seat] || [],
    discards: [],
  })),
});

// ---------- 1. 摸牌后 state_sync 不许把牌插进牌列 ----------
{
  const app = createState();
  app.startRound({ hand: ['1m', '2m', '3m', '4m', '5p', '6p', '7p', '8p', '9p', '1s', '2s', '3s', '1z'], seat: 0, dealer: 0, round_index: 0, round_count: 4 });
  app.drawTile({ tile: '5m', seat: 0 });
  ok('摸牌后手牌 14 张', app.state.hand.length === 14);
  ok('摸到的那张在末尾', app.state.hand[13] === '5m', app.state.hand.join(','));
  ok('drawnIndex 指到末尾', drawnIndex(app.state) === 13);

  // 服务端下发排序好的 14 张（5m 在中间）
  const sorted = sortHand(app.state.hand);
  app.applySync({ ...seatFrame(), hand: sorted, melds: [], remaining: 68, current_seat: 0, seq: 10 });
  ok('sync 之后 5m 仍在末尾', app.state.hand[13] === '5m', app.state.hand.join(','));
  ok('其余 13 张是有序的', JSON.stringify(app.state.hand.slice(0, 13)) === JSON.stringify(sorted.filter((c, i) => i !== sorted.indexOf('5m'))), app.state.hand.join(','));
  ok('drawn 仍然记着', app.state.drawn === '5m');

  // 打出 5m（摸到的那张）之后要归位
  app.discardTile({ seat: 0, tile: '5m' });
  ok('打出后 13 张', app.state.hand.length === 13);
  ok('打出后已排序', JSON.stringify(app.state.hand) === JSON.stringify(sortHand(app.state.hand)), app.state.hand.join(','));
  ok('打出后没有摸牌标记', app.state.drawn === null && drawnIndex(app.state) === -1);
}

// ---------- 2. 打出非摸牌那张，也要在打完后排序 ----------
{
  const app = createState();
  app.startRound({ hand: ['1m', '2m', '3m', '4m', '5p', '6p', '7p', '8p', '9p', '1s', '2s', '3s', '1z'], seat: 0, dealer: 0, round_index: 0, round_count: 4 });
  app.drawTile({ tile: '5m', seat: 0 });
  app.discardTile({ seat: 0, tile: '1m' });
  ok('打出非摸牌张后 13 张且有序', JSON.stringify(app.state.hand) === JSON.stringify(sortHand(app.state.hand)), app.state.hand.join(','));
  ok('5m 已归位到中间', app.state.hand.indexOf('5m') === sortHand([...app.state.hand]).indexOf('5m'));
}

// ---------- 3. 副露横放：碰 ----------
{
  const app = createState();
  app.startRound({ hand: ['1m', '2m', '3m', '4m', '5p', '6p', '7p', '8p', '9p', '1s', '2s', '3s', '1z'], seat: 0, dealer: 0, round_index: 0, round_count: 4 });
  app.state.rivers[3] = ['5m'];
  app.applyActionEvent({ seat: 0, action: ACTION.PONG, tiles: ['5m'], from_seat: 3 });
  ok('碰：牌河末张被记为被鸣牌', app.state.meldClaims[0][0] === '5m', JSON.stringify(app.state.meldClaims[0]));
  app.applySync({ ...seatFrame(), hand: ['2m', '3m', '4m', '5p', '6p', '7p', '8p', '9p', '1s', '2s'], melds: [{ type: MELD_TYPE.PONG, tiles: ['5m', '5m', '5m'] }], remaining: 60, current_seat: 1, seq: 12 });
  const pong = app.state.melds.find((m) => m.seat === 0 && m.type === MELD_TYPE.PONG);
  ok('碰：横放对到组内下标', pong && pong.rotated === 0, JSON.stringify(pong));
  ok('碰：牌面保持服务端顺序', JSON.stringify(pong.tiles) === JSON.stringify(['5m', '5m', '5m']));
}

// ---------- 4. 副露横放：吃（被鸣的是中间那张） ----------
{
  const app = createState();
  app.startRound({ hand: ['2m', '4m', '1p', '2p', '3p', '4p', '5p', '6p', '7p', '8p', '9p', '1s', '1z'], seat: 0, dealer: 0, round_index: 0, round_count: 4 });
  app.state.rivers[3] = ['3m'];
  app.applyActionEvent({ seat: 0, action: ACTION.CHOW, tiles: ['2m', '4m'], from_seat: 3 });
  app.applySync({ ...seatFrame(), hand: ['1p', '2p', '3p', '4p'], melds: [{ type: MELD_TYPE.CHOW, tiles: ['2m', '3m', '4m'] }], remaining: 60, current_seat: 1, seq: 14 });
  const chi = app.state.melds.find((m) => m.seat === 0 && m.type === MELD_TYPE.CHOW);
  ok('吃：横放的是被鸣的 3m（下标 1）', chi && chi.rotated === 1, JSON.stringify(chi));
  ok('吃：组内仍是 2m3m4m 顺序', JSON.stringify(chi.tiles) === JSON.stringify(['2m', '3m', '4m']));
  ok('吃：手牌被对家副露占用后只剩 4 张', app.state.hand.length === 4);
}

// ---------- 5. 他家副露也要横放 ----------
{
  const app = createState();
  app.startRound({ hand: ['1m', '2m', '3m', '4m', '5p', '6p', '7p', '8p', '9p', '1s', '2s', '3s', '1z'], seat: 0, dealer: 0, round_index: 0, round_count: 4 });
  app.state.rivers[0] = ['7p'];
  // 下家（seat 1）吃我打出的 7p
  app.applyActionEvent({ seat: 1, action: ACTION.CHOW, tiles: ['6p', '8p'], from_seat: 0 });
  ok('他家：牌河末张被记录', app.state.meldClaims[1][0] === '7p', JSON.stringify(app.state.meldClaims[1]));
  const frame = seatFrame({ 1: [{ type: MELD_TYPE.CHOW, tiles: ['6p', '7p', '8p'] }] });
  app.applySync({ ...frame, hand: app.state.hand.slice(), melds: [], remaining: 60, current_seat: 1, seq: 16 });
  const chi = app.state.melds.find((m) => m.seat === 1 && m.type === MELD_TYPE.CHOW);
  ok('他家：吃也横放被鸣那张', chi && chi.rotated === 1, JSON.stringify(chi));
}

// ---------- 6. 拿不到 claim 时退化：碰仍然横放 ----------
{
  const app = createState();
  app.startRound({ hand: ['1m', '2m', '3m', '4m', '5p', '6p', '7p', '8p', '9p', '1s', '2s', '3s', '1z'], seat: 0, dealer: 0, round_index: 0, round_count: 4 });
  const frame = seatFrame({ 1: [{ type: MELD_TYPE.PONG, tiles: ['9p', '9p', '9p'] }] });
  app.applySync({ ...frame, hand: app.state.hand.slice(), melds: [], remaining: 60, current_seat: 1, seq: 18 });
  const pong = app.state.melds.find((m) => m.seat === 1 && m.type === MELD_TYPE.PONG);
  ok('无 claim 记录时碰仍横放组首', pong && pong.rotated === 0, JSON.stringify(pong));
  const ankan = seatFrame({ 1: [{ type: MELD_TYPE.ANKAN, tiles: [] }] });
  app.applySync({ ...ankan, hand: app.state.hand.slice(), melds: [], remaining: 60, current_seat: 1, seq: 20 });
  const kan = app.state.melds.find((m) => m.seat === 1 && m.type === MELD_TYPE.ANKAN);
  ok('他家暗杠 4 张背牌、不横放', kan && kan.tiles.length === 4 && kan.rotated === -1, JSON.stringify(kan));
}

console.log('');
console.log(fail ? `FAILED ${fail}` : `ALL PASS  ${pass} passed`);
process.exit(fail ? 1 : 0);
