// 结算期验收：v4.5 摊牌（results[].hand 扣副露 → revealHands）+ 日麻语音 cue 口径
// + 摊牌渲染冒烟（真 three.js 驱动 OpponentHandsView，不开浏览器）
import { FRAME_TYPE, MELD_TYPE, YAKU } from '../../web/js/protocol_enums.js';

// DOM/canvas stub：tile3d 的牌背/侧面纹理要 2d canvas；本测试不做 GL 渲染，够用。
// 方法都返回 stub 自身，createLinearGradient().addColorStop() 这类链式调用才不炸
const ctx2d = new Proxy({}, { get: () => () => ctx2d });
globalThis.document = {
  createElement: () => ({
    width: 0, height: 0,
    getContext: () => ctx2d,
  }),
};

const { createState } = await import('../../web/js/state.js');
const { buildVoiceCues, voiceDuration, VOICE_MS } = await import('../../web/js/voice.js');

let pass = 0, fail = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a === e) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name}\n       got ${a}\n       exp ${e}`); }
};

// ---------------------------------------------------------------- 摊牌
const app = createState();
const s = app.state;
app.startRound({ seat: 1, dealer: 0, round_index: 0, round_count: 4,
  hand: ['1m', '2m', '3m'] });   // 本家坐协议 seat 1：viewSeat 换算必有位移

// state_sync：对家（协议 seat 3）一组碰 5m
app.applySync({
  seq: 1,
  current_seat: 0,
  seats: [
    { seat: 1, hand_count: 13, melds: [] },
    { seat: 2, hand_count: 13, melds: [] },
    { seat: 3, hand_count: 10, melds: [{ type: MELD_TYPE.PONG, tiles: ['5m', '5m', '5m'] }] },
    { seat: 0, hand_count: 13, melds: [] },
  ],
});

// 荒庄没人和牌 ⇒ 不摊牌（用户口径：只有和牌者亮牌）
app.applyGameEnd({
  type: FRAME_TYPE.GAME_END,
  results: [
    { seat: 1, delta_score: 0, hand: ['1m', '2m', '3m', '4m', '5m', '6m', '7m', '8m', '9m', '1p', '2p', '3p', '4p'] },
    { seat: 3, delta_score: 0, hand: ['1s', '2s', '3s', '5m', '5m', '5m', '7z', '7z', '2p', '3p', '4p', '6z', '6z'] },
  ],
  win: null,
});
check('摊牌：荒庄不摊牌', s.revealHands, null);
check('摊牌：荒庄无和牌者高亮', s.revealWinner, null);

// 和牌者高亮换算到屏幕视角（本家协议 seat 1 和牌 → view 0）；只有和牌者亮
app.applyGameEnd({
  type: FRAME_TYPE.GAME_END,
  win: { seat: 1, tsumo: false, han: 2 },
  results: [
    { seat: 1, delta_score: 2000, hand: ['1m', '2m', '3m'] },
    { seat: 3, delta_score: 0, hand: ['1s', '2s', '3s', '5m', '5m', '5m', '7z', '7z'] },
  ],
});
check('摊牌：和牌者视角换算（协议 seat1 = view0）', s.revealWinner, 0);
check('摊牌：和牌者门前牌 = 扁手牌', s.revealHands[0], ['1m', '2m', '3m']);
check('摊牌：非和牌家不亮牌', s.revealHands[2], null);
check('摊牌：放炮者也不亮', s.revealHands[3], null);

// 和牌者带副露：扁手牌扣掉碰出的 5m（对家 view2 的副露在 applySync 里已就位）
app.applyGameEnd({
  type: FRAME_TYPE.GAME_END,
  win: { seat: 3, tsumo: true, han: 1 },
  results: [
    { seat: 3, delta_score: 1000,
      hand: ['1s', '2s', '3s', '5m', '5m', '5m', '7z', '7z', '2p', '3p', '4p', '6z', '6z', '6z'] },
  ],
});
check('摊牌：和牌者扣副露', s.revealHands[2],
  ['1s', '2s', '3s', '7z', '7z', '2p', '3p', '4p', '6z', '6z', '6z']);
check('摊牌：他家暗杠 back 占位不会误删门前牌', (() => {
  // 给和牌家补一组暗杠占位（'back'），门前牌不应被减掉合法牌
  s.melds.push({ seat: 2, kind: 'ankan', type: MELD_TYPE.ANKAN, tiles: ['back', 'back', 'back', 'back'], rotated: -1 });
  const concealed = ['1s', '2s', '3s', '7z', '7z'];
  for (const meld of s.melds) {
    if (meld.seat !== 2) continue;
    for (const t of meld.tiles) {
      const at = concealed.indexOf(t);
      if (at >= 0) concealed.splice(at, 1);
    }
  }
  return concealed;
})(), ['1s', '2s', '3s', '7z', '7z']);
s.melds.pop();   // 还原：不影响后续断言

// game_start 复位
app.startRound({ seat: 1, dealer: 0, round_index: 1, round_count: 4, hand: ['1z'] });
check('复位：新一局不再摊牌', s.revealHands, null);

// ---------------------------------------------------------------- 语音 cue
// ① 荣和：立直 + 一发 + 断幺九 → 方式 + 三役 + 合计
check('语音：立直一发断幺', buildVoiceCues({ win: { tsumo: false, han: 3,
  yaku: [{ id: YAKU.RIICHI, han: 1 }, { id: YAKU.IPPATSU, han: 1 }, { id: YAKU.TANYAO, han: 1 }] } }).cues,
  ['vo_mode_ron', 'vo_yaku_riichi_1', 'vo_yaku_ippatsu_1', 'vo_yaku_tanyao', 'vo_han_3']);

// ② 自摸 + 门前自摸役 + 平和：mode_tsumo 与役 tsumo 是两条不同资产
check('语音：自摸含门前自摸役', buildVoiceCues({ win: { tsumo: true, han: 2,
  yaku: [{ id: YAKU.TSUMO, han: 1 }, { id: YAKU.PINFU, han: 1 }] } }).cues,
  ['vo_mode_tsumo', 'vo_yaku_tsumo_1', 'vo_yaku_pinfu', 'vo_han_2']);

// ③ 箭刻按组数分三条；役满整手只念一句役满、不念合计番数
check('语音：箭刻两组', buildVoiceCues({ win: { tsumo: false, han: 2,
  yaku: [{ id: YAKU.YAKUHAI, han: 2 }] } }).cues,
  ['vo_mode_ron', 'vo_yaku_yakuhai_2', 'vo_han_2']);
check('语音：役满不念番数', buildVoiceCues({ win: { tsumo: false, han: 13,
  yaku: [{ id: YAKU.KOKUSHI_13, han: 13 }] } }).cues,
  ['vo_mode_ron', 'vo_yakuman']);

// ④ 开/闭番差：副露三色同顺（1 番）念 _1 资产；门清（2 番）复用 classic
check('语音：三色同顺一番（副露）', buildVoiceCues({ win: { tsumo: false, han: 1,
  yaku: [{ id: YAKU.SANSHOKU_DOUJUN, han: 1 }] } }).cues,
  ['vo_mode_ron', 'vo_yaku_sanshoku_doujun_1', 'vo_han_1']);
check('语音：三色同顺两番（门清）', buildVoiceCues({ win: { tsumo: false, han: 2,
  yaku: [{ id: YAKU.SANSHOKU_DOUJUN, han: 2 }] } }).cues,
  ['vo_mode_ron', 'vo_yaku_sanshoku_doujun', 'vo_han_2']);

// ⑤ 番值对不上不念（classic 混一色两番 ≠ 日麻门清三番之外的情况兜底）
check('语音：混一色三番用日麻资产', buildVoiceCues({ win: { tsumo: false, han: 3,
  yaku: [{ id: YAKU.HONITSU, han: 3 }] } }).cues,
  ['vo_mode_ron', 'vo_yaku_honitsu_3', 'vo_han_3']);
check('语音：七对子复用 seven_pairs', buildVoiceCues({ win: { tsumo: false, han: 2,
  yaku: [{ id: YAKU.CHIITOITSU, han: 2 }] } }).cues,
  ['vo_mode_ron', 'vo_yaku_seven_pairs', 'vo_han_2']);

// ⑥ 宝牌不是役，不念
check('语音：宝牌静默', buildVoiceCues({ win: { tsumo: false, han: 4,
  yaku: [{ id: YAKU.RIICHI, han: 1 }, { id: YAKU.DORA, han: 3 }] } }).cues,
  ['vo_mode_ron', 'vo_yaku_riichi_1', 'vo_han_4']);

// ⑦ 荒庄 / 清一色六番 / 时长表完整
check('语音：荒庄', buildVoiceCues({}).cues, ['vo_mode_draw']);
check('语音：清一色六番', buildVoiceCues({ win: { tsumo: false, han: 6,
  yaku: [{ id: YAKU.CHINITSU, han: 6 }] } }).cues,
  ['vo_mode_ron', 'vo_yaku_chinitsu_6', 'vo_han_6']);
check('语音：时长表覆盖全部新句', ['vo_yaku_riichi_1', 'vo_yakuman', 'vo_yaku_honitsu_3']
  .every((id) => VOICE_MS[id] > 0), true);
check('语音：时长为正数', voiceDuration(['vo_mode_ron']) > 0, true);

// ---------------------------------------------------------------- 摊牌渲染冒烟
// 真 three.js + OpponentHandsView.render()：摊牌期手牌要换成 faceUp 牌面、
// 和牌者金条只挂在和牌家；新一局复位后回牌背、金条摘除。
const { OpponentHandsView } = await import('../../web/js/opponents.js');
const view = new OpponentHandsView();
const app2 = createState();
const s2 = app2.state;
app2.startRound({ seat: 0, dealer: 0, round_index: 0, round_count: 4, hand: ['1m'] });
for (let v = 1; v < 4; v++) s2.hands[v] = Array(13).fill('back');
app2.applyGameEnd({
  type: FRAME_TYPE.GAME_END,
  win: { seat: 2, tsumo: true, han: 1 },
  results: [
    { seat: 0, delta_score: 0, hand: Array(13).fill('1m') },
    { seat: 1, delta_score: 0, hand: Array(13).fill('2p') },
    { seat: 2, delta_score: 1000, hand: Array(13).fill('3s') },
    { seat: 3, delta_score: 0, hand: Array(13).fill('7z') },
  ],
});
view.render(s2);
const seatTiles = (seat) => view.seats.get(seat).tiles.map((t) => t.mesh.userData.faceUp);
check('渲染：和牌家(view2)亮牌面', seatTiles(2).length === 13 && seatTiles(2).every(Boolean), true);
check('渲染：其余两家保持牌背', [1, 3].every((seat) => seatTiles(seat).every((up) => !up)), true);
check('渲染：和牌家(view2)挂金条', !!view.seats.get(2).strip, true);
check('渲染：非和牌家无金条', [1, 3].every((seat) => !view.seats.get(seat).strip), true);
check('渲染：牌码与 revealHands 一致',
  view.seats.get(2).tiles.every((t) => t.mesh.userData.code === '3s'), true);
app2.startRound({ seat: 0, dealer: 0, round_index: 1, round_count: 4, hand: ['1m'] });
for (let v = 1; v < 4; v++) s2.hands[v] = Array(13).fill('back');
view.render(s2);
check('渲染：新一局回牌背', [1, 2, 3].map(seatTiles).every((row) => row.every((up) => !up)), true);
check('渲染：金条已摘除', [1, 2, 3].every((seat) => !view.seats.get(seat).strip), true);

// ---------------------------------------------------------------- 结算面板 · 和牌详情
// renderWinHand：大牌面行（副露分组 + 门前牌 + 和牌张单列高亮）+ 役种明细
const uiMod = await import('../../web/js/ui.js');
const makeStubEl = () => ({ hidden: true, innerHTML: '', style: {} });
const ui = Object.create(uiMod.UI.prototype);
ui.el = { rsHand: makeStubEl() };

const winFrame = {
  win: { seat: 2, tsumo: false, tile: '3s', han: 3,
    yaku: [{ id: YAKU.RIICHI, han: 1 }, { id: YAKU.TANYAO, han: 1 }, { id: YAKU.DORA, han: 1 }] },
  results: [{ seat: 2, delta_score: 0, hand: Array(13).fill('3s') }],
};
app2.applyGameEnd(winFrame);   // 上一步 startRound 清了摊牌，重放一次局终
ui.renderWinHand(winFrame, s2);
check('面板：和牌详情可见', ui.el.rsHand.hidden, false);
check('面板：门前牌 + 和牌张 = revealHands 张数',
  (ui.el.rsHand.innerHTML.match(/rstile/g) || []).length, 13);
check('面板：和牌张金框', ui.el.rsHand.innerHTML.includes('rstile agari'), true);
check('面板：役种明细含宝牌', ui.el.rsHand.innerHTML.includes('宝牌'), true);
check('面板：役种含立直与断幺九',
  ui.el.rsHand.innerHTML.includes('立直') && ui.el.rsHand.innerHTML.includes('断幺九'), true);
// 荒庄：整块隐藏
ui.renderWinHand({ win: null }, s2);
check('面板：荒庄不显示和牌详情', ui.el.rsHand.hidden, true);
// 缺数据（revealHands 为空）：给占位而不是崩
ui.renderWinHand({ win: { seat: 1, tsumo: true, tile: '', han: 1, yaku: [] } }, s2);
check('面板：数据缺失有占位', ui.el.rsHand.innerHTML.includes('rshand-empty'), true);

// 副露被鸣张：碰 2s / 吃 4m（用户截图实锤）——每组恰好一张横放，
// 且牌面贴图必须挂在独立内层上，否则会被 background-size 拉扁成「矮牌」。
const claimState = {
  mySeat: 0,
  melds: [
    { seat: 0, kind: 'pong', tiles: ['2s', '2s', '2s'], rotated: 0 },
    { seat: 0, kind: 'chi', tiles: ['4m', '5m', '6m'], rotated: 0 },
  ],
  revealHands: [['7m', '7m', '3p', '0p', '5z', '5z', '5z'], null, null, null],
};
const claimFrame = { win: { seat: 0, tsumo: false, tile: '3s', han: 1, yaku: [] } };
ui.renderWinHand(claimFrame, claimState);
const claimHtml = ui.el.rsHand.innerHTML;
check('面板：两组副露各有分组框', (claimHtml.match(/class="rsmeld"/g) || []).length, 2);
check('面板：每组恰好一张被鸣牌横放', (claimHtml.match(/class="rstile claim"/g) || []).length, 2);
check('面板：每张牌都有独立贴图内层',
  (claimHtml.match(/class="rsface"/g) || []).length,
  (claimHtml.match(/class="rstile/g) || []).length);
check('面板：横放牌的贴图在内层而非外层',
  claimHtml.includes('class="rstile claim"><i class="rsface"'), true);
const claimStyles = [...claimHtml.matchAll(/class="rstile claim"><i class="rsface" style="([^"]*)"/g)]
  .map((m) => m[1]);
// 图集顺序走 config 的 ALL_CODES（与 tile-codes.json 同源），避免硬编码浮点百分比
const { ALL_CODES } = await import('../../web/js/config.js');
const bgPos = (code) => {
  const idx = ALL_CODES.indexOf(code);
  return `${(idx % 8) / 7 * 100}% ${Math.floor(idx / 8) / 4 * 100}%`;
};
check('面板：横放的是 2 条与 4 万',
  claimStyles.length === 2
    && claimStyles.some((s) => s.includes(`background-position:${bgPos('2s')}`))
    && claimStyles.some((s) => s.includes(`background-position:${bgPos('4m')}`)), true);
ui.renderWinHand(claimFrame, {
  ...claimState,
  melds: [...claimState.melds,
    { seat: 0, kind: 'ankan', tiles: ['back', 'back', 'back', 'back'], rotated: -1 }],
});
check('面板：暗杠不参与横放', (ui.el.rsHand.innerHTML.match(/class="rstile claim"/g) || []).length, 2);
const { readFile } = await import('node:fs/promises');
const uiCss = await readFile(new URL('../../web/css/ui.css', import.meta.url), 'utf8');
const claimRule = uiCss.match(/\.rstile\.claim\s*>\s*\.rsface\s*\{([^}]*)\}/)?.[1] || '';
check('面板：横放牌面旋转 90°（与 3D 的 rot+1 一致）', claimRule.includes('rotate(90deg)'), true);
// 横放牌面与竖牌正面同为 34x52，旋转后图案比例不变
check('面板：横放牌面保持竖牌正面比例',
  /width:\s*34px/.test(claimRule) && /height:\s*52px/.test(claimRule), true);

console.log(`\n${pass} pass, ${fail} fail`);
process.exit(fail ? 1 : 0);
