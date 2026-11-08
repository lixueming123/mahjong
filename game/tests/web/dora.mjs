/**
 * dora.mjs — 场况卡第一行（宝牌指示牌槽位）的 DOM 逻辑单测。
 *
 * ui.js 的 renderDoraRow 只碰 classList/textContent/background，用 20 行
 * DOM stub 就能在 node 里跑，不用开浏览器。覆盖：无宝牌（显示场风）、
 * 翻 1 张（贴图取格）、杠后多张、新的一局清干净 face 类与内联贴图。
 *
 * 贴图口径：图集 8x5，background-size 800%x500%，百分比位移
 * x = col/7、y = row/4（bg-position 百分比 = 图的 p% 对齐槽的 p%）。
 */
const els = new Map();
function makeEl(id) {
  const classes = new Set();
  return {
    id,
    classList: {
      add: (...c) => c.forEach((x) => classes.add(x)),
      remove: (...c) => c.forEach((x) => classes.delete(x)),
      toggle: (c, on) => (on ? classes.add(c) : classes.delete(c)),
    },
    textContent: '',
    style: {},
    _classes: classes,
  };
}
globalThis.document = {
  getElementById: (id) => {
    if (!els.has(id)) els.set(id, makeEl(id));
    return els.get(id);
  },
};

const mod = await import('../../web/js/ui.js');
// 绕过 constructor（它要抓全部 HUD 元素），手动造本测试需要的 el
const ui = Object.create(mod.UI.prototype);
ui.el = {
  roundMark: globalThis.document.getElementById('round-mark'),
  roundBars: [0, 1, 2, 3].map((i) => globalThis.document.getElementById('bar' + i)),
};
const slot0 = ui.el.roundMark;

let pass = 0, fail = 0;
const assert = (name, cond) => {
  if (cond) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name}`); }
};

/** ALL_CODES 的格位（与 assets.js UV 同源） */
const idx = await import('../../web/js/config.js').then((cfg) => {
  const m = new Map(cfg.ALL_CODES.map((c, i) => [c, i]));
  return (code) => m.get(code);
});
const posOf = (code) => {
  const i = idx(code);
  return `${(i % 8) / 7 * 100}% ${Math.floor(i / 8) / 4 * 100}%`;
};

// 无宝牌：slot0 显示场风，其余背面，且无贴图残留
ui.renderDoraRow({ round: '東1局', dora: [] });
assert('无宝牌 slot0=東', slot0.textContent === '東' && !slot0._classes.has('face'));
assert('无宝牌 bars 空', ui.el.roundBars.every((b) => b.textContent === '' && !b._classes.has('face')));
assert('无宝牌无贴图', !slot0.style.backgroundImage);

// 1 张宝牌：slot0 贴中（红中在图集里本来就是红色），其余背面
ui.renderDoraRow({ dora: ['7z'] });
assert('1张 slot0 贴中',
  slot0.textContent === '' && slot0._classes.has('face')
  && slot0.style.backgroundImage.includes('tile-atlas-hand.png')
  && slot0.style.backgroundPosition === posOf('7z'));
assert('1张 不再挂 red/sm 字色类', !slot0._classes.has('red') && !slot0._classes.has('sm'));
assert('1张 bar0 背面', !ui.el.roundBars[0]._classes.has('face') && ui.el.roundBars[0].textContent === '');

// 杠后 3 张：slot0/1/2 依次翻面，各自取自己的格
ui.renderDoraRow({ dora: ['7z', '5m', '1p'] });
assert('3张 slot1 贴5万', ui.el.roundBars[0].style.backgroundPosition === posOf('5m')
  && ui.el.roundBars[0]._classes.has('face'));
assert('3张 slot2 贴1筒', ui.el.roundBars[1].style.backgroundPosition === posOf('1p'));
assert('3张 slot3 背面', !ui.el.roundBars[2]._classes.has('face'));

// 从有到无（新的一局）：face 类与内联贴图都要清干净
ui.renderDoraRow({ round: '東2局', dora: [] });
assert('清空后 slot0 回场风',
  slot0.textContent === '東' && !slot0._classes.has('face') && !slot0._classes.has('red'));
assert('清空后 bar0 清 face 与贴图',
  !ui.el.roundBars[0]._classes.has('face') && !ui.el.roundBars[0].style.backgroundImage);

console.log(`宝牌槽位：${fail ? 'FAIL' : 'ALL PASS'}  ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
