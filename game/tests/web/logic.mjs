// 逻辑验收：按 PROTOCOL 的 tiles 语义 + 参考客户端口径，断言按钮→帧、窗口/seq/pending、状态归约
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

const { createState } = await import('../../web/js/state.js');
const { MahjongClient } = await import('../../web/js/client.js');

const app = createState();
const client = new MahjongClient({
  app, ui: { update() {}, el: { toggleSfx: mk() } }, sfx: { play() {} },
  url: 'ws://x/ws', options: { name: 't' },
});
const sent = [];
client.net.send = (frame) => { sent.push(frame); return true; };
client.net.connect = async () => true;
client.net.connected = true;
const btn = (label) => buttons[labels.indexOf(label)];
const take = () => sent.splice(0);

let pass = 0, fail = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a === e) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name}\n       got ${a}\n       exp ${e}`); }
};
const window_ = (seq, kind = 'claim', options = [], extra = {}) => ({
  type: 'action_request', seq, window: kind, deadline_ms: Date.now() + 15000, options, ...extra,
});

app.startRound({ seat: 0, dealer: 0, round_index: 0, round_count: 4,
  hand: ['0m', '1m', '2m', '4m', '5m', '3p', '4p', '5p', '7p', '2p', '2p', '1z', '9s', '9s'] });
client.lastSeq = 0;

// 1. 吃：两条 options 时按选中的手牌消歧（不再永远取第一条）
client.handleFrame(window_(10, 'claim', [
  { action: 'chow', tiles: ['1m', '2m'] },
  { action: 'chow', tiles: ['4m', '5m'] },
  { action: 'pass', tiles: [] },
]));
app.select(3);                                   // 4m
btn('吃').click();
check('吃：选中 4m → 发第二组 (4m,5m)', take(), [{ type: 'action', action: 'chow', tiles: ['4m', '5m'], seq: 10 }]);

// 2. 提交后 pending 锁：再点不会重复发帧
check('提交后按钮全灰（pending）', { discard: btn('打出').disabled, chi: btn('吃').disabled }, { discard: true, chi: true });
btn('吃').click();
check('pending 期间再点「吃」不发帧', take(), []);

// 3. 碰：tiles 语义 = [被鸣的那张]（1 张）
client.handleFrame(window_(11, 'claim', [{ action: 'pong', tiles: ['2p'] }, { action: 'pass', tiles: [] }]));
check('新窗口清掉 pending', client.pending, false);
btn('碰').click();
check('碰 → [2p]', take(), [{ type: 'action', action: 'pong', tiles: ['2p'], seq: 11 }]);

// 4. 明杠：tiles 语义 = [被鸣的那张]（1 张），不是 3 张
client.handleFrame(window_(12, 'claim', [{ action: 'kong', tiles: ['1z'] }, { action: 'pass', tiles: [] }]));
btn('杠').click();
check('明杠 → [1z]', take(), [{ type: 'action', action: 'kong', tiles: ['1z'], seq: 12 }]);

// 5. 暗杠：多条 options 时按选中手牌消歧；tiles 恰 1 个牌面
client.handleFrame(window_(13, 'discard', [
  { action: 'ankan', tiles: ['7p'] },
  { action: 'ankan', tiles: ['9s'] },
]));
app.select(8);                                   // 7p
btn('杠').click();
check('暗杠：选中 7p → [7p]', take(), [{ type: 'action', action: 'ankan', tiles: ['7p'], seq: 13 }]);

// 6. 加杠
client.handleFrame(window_(14, 'discard', [{ action: 'kakan', tiles: ['2p'] }]));
btn('杠').click();
check('加杠 → [2p]', take(), [{ type: 'action', action: 'kakan', tiles: ['2p'], seq: 14 }]);

// 7. 和牌
client.handleFrame(window_(15, 'claim', [{ action: 'win', tiles: [] }, { action: 'pass', tiles: [] }]));
btn('和').click();
check('和 → []', take(), [{ type: 'action', action: 'win', tiles: [], seq: 15 }]);
client.handleFrame({ type: 'action_event', seat: 0, action: 'win', tiles: ['9s'], seq: 15 });
check('和牌后冻结输入', client.frozen, true);
check('和牌后按钮全灰', { win: btn('和').disabled, pass: btn('过').disabled, chi: btn('吃').disabled },
  { win: true, pass: true, chi: true });

// 8. 过
client.frozen = false;
client.handleFrame(window_(16, 'claim', [{ action: 'pass', tiles: [] }]));
btn('过').click();
check('过 → []', take(), [{ type: 'action', action: 'pass', tiles: [], seq: 16 }]);
client.pending = false;

// 9. 立直两段式：点立直先进宣言模式，不发帧
client.handleFrame(window_(17, 'discard', [{ action: 'discard', tiles: [] }, { action: 'riichi', tiles: [] }],
  { riichi_tiles: ['5m', '3p'] }));
check('立直候选归一为牌面', client.window.riichiTiles, ['5m', '3p']);
btn('立直').click();
check('点立直 → 进入宣言模式、不发帧', { arming: client.riichiArming, sent: take() }, { arming: true, sent: [] });
check('宣言模式提示候选', byId['log-text'].textContent.includes('5m'), true);

// 10. 宣言模式点非候选牌 → 只提示
app.select(1);                                   // 1m，不是候选
client.commitRiichiAt(1);
check('点非候选牌不发帧', take(), []);

// 11. 宣言模式点候选牌 → 发手上那张物理牌（赤 0m 也原样发）
app.select(0);                                   // 0m（牌面 5m）
client.commitRiichiAt(0);
check('立直 → tiles 为手上那张 0m', take(), [{ type: 'action', action: 'riichi', tiles: ['0m'], seq: 17 }]);

// 12. 已选中候选牌时点立直 = 直接宣言
client.pending = false;
client.handleFrame(window_(18, 'discard', [{ action: 'riichi', tiles: [] }], { riichi_tiles: ['5m'] }));
app.select(4);                                   // 5m
btn('立直').click();
check('选中候选后点立直 → 直接发帧', take(), [{ type: 'action', action: 'riichi', tiles: ['5m'], seq: 18 }]);

// 13. 打牌：带 seq；pending 后禁止再发
client.pending = false;
client.handleFrame(window_(19, 'discard', []));
app.select(2);                                   // 2m
btn('打出').click();
check('打出 → tile + seq', take(), [{ type: 'discard', tile: '2m', seq: 19 }]);
btn('打出').click();
check('pending 期间再点「打出」不发帧', take(), []);

// 14. 窗口到期：按钮全灰、不发帧
client.pending = false;
client.handleFrame({ type: 'action_request', seq: 20, window: 'claim', deadline_ms: Date.now() - 500,
  options: [{ action: 'pass', tiles: [] }, { action: 'pong', tiles: ['2p'] }] });
check('过期窗口剩余为 0', client.remainingMs(), 0);
check('过期窗口按钮全灰', { pass: btn('过').disabled, pong: btn('碰').disabled }, { pass: true, pong: true });

// 15. 更新的 seq 推进窗口（旧窗口自动关、pending 作废）
client.handleFrame(window_(21, 'claim', [{ action: 'pass', tiles: [] }]));
check('窗口已开', !!client.window, true);
client.handleFrame({ type: 'state_sync', seq: 22, remaining: 60, current_seat: 1, hand: [],
  melds: [], seats: [
    { seat: 0, name: '我', hand_count: 13, discards: [], melds: [], disconnected: false },
    { seat: 1, name: 'AI-1', hand_count: 13, discards: [], melds: [], disconnected: false },
    { seat: 2, name: 'AI-2', hand_count: 13, discards: [], melds: [], disconnected: false },
    { seat: 3, name: 'AI-3', hand_count: 13, discards: [], melds: [], disconnected: false },
  ] });
check('更新 seq 后窗口关闭', client.window, null);
check('窗口关闭后按钮全灰', btn('过').disabled, true);

// 16. 过期 action_request 不重建窗口
client.handleFrame(window_(20, 'claim', [{ action: 'pass', tiles: [] }]));
check('过期 action_request 被丢弃', client.window, null);

console.log(`\n按钮/窗口：${fail ? 'FAILED' : 'ALL PASS'}  ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
