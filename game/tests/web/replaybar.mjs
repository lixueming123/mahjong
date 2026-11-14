// 回放控制条的进度条分段（replaybar.js 的 roundMarksHtml）。
//
// 「442 / 442」这种一维进度看不出"一局一局"的结构。控制条按 steps[] 里 game_start 的位置
// 把滑轨切成一段一段，并给够宽的段标上局号。这里验证切分与抽稀的口径 —— 它决定玩家能
// 不能在拖动时看出"这一局还有多长"。纯函数，不开浏览器。

import { roundMarksHtml } from '../../web/js/replaybar.js';

let pass = 0;
let fail = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a === e) {
    pass++;
    console.log(`  ok   ${name}`);
  } else {
    fail++;
    console.log(`  FAIL ${name}\n       got ${a}\n       exp ${e}`);
  }
};

/** 从生成的 HTML 里抠出竖线的位置与标签文字（顺序按出现次序）。 */
function parse(html) {
  const lines = [...html.matchAll(/<i style="left:([\d.]+)%">/g)].map((m) => Number(m[1]));
  const labels = [...html.matchAll(/<span[^>]*style="left:([\d.]+)%">([^<]+)<\/span>/g)]
    .map((m) => ({ at: Number(m[1]), text: m[2] }));
  return { lines, labels };
}

// ---------------------------------------------------------------- 1. 基本切分

// 6 局 / 442 步（取自真实录制 r-3pk28g40n37k1 的 game_start 位置）。
const real = [
  { index: 0, label: '東1' },
  { index: 66, label: '東1' },   // 连庄：同一局号
  { index: 156, label: '東2' },
  { index: 204, label: '東3' },
  { index: 293, label: '東3' },  // 再连庄
  { index: 353, label: '東4' },
];
const html = roundMarksHtml(real, 442);
const { lines, labels } = parse(html);

// 第 0 段的那条线在左端，画出来只是压住滑轨起点 —— 不画。
check('竖线数 = 局数 - 1', lines.length, real.length - 1);
check('第一条竖线在第二局起点', lines[0], Number(((66 / 442) * 100).toFixed(3)));
check('末条竖线在最后一局起点', lines[lines.length - 1], Number(((353 / 442) * 100).toFixed(3)));

// 抽稀：320px 上 6 段平均只有 53px，中间那些放不下两个汉字，只保留首末。
const texts = labels.map((label) => label.text);
check('首末段必画', [texts[0], texts[texts.length - 1]], ['東1', '東4']);
check('挤不下的中间段不写字', texts.length < real.length, true);
check('标签数不超过局数', labels.length <= real.length, true);

// ---------------------------------------------------------------- 2. 段够宽就都写

// 4 局均分 400 步：每段 25% > 18% 阈值，四个标签都该出现。
const even = [0, 100, 200, 300].map((index, at) => ({ index, label: `東${at + 1}` }));
const evenOut = parse(roundMarksHtml(even, 400));
check('段够宽时每局都有标签', evenOut.labels.map((label) => label.text), ['東1', '東2', '東3', '東4']);
check('竖线仍是 3 条', evenOut.lines.length, 3);

// ---------------------------------------------------------------- 3. 两端标签的锚点

// 首段贴左端、末段贴右端：居中会把半个字挤出滑轨。用 class 让 CSS 改锚点。
check('首段标签带 first 类', html.includes('class="first"'), true);
check('末段标签带 last 类', html.includes('class="last"'), true);

// ---------------------------------------------------------------- 4. 退化输入不崩

check('空局列表', roundMarksHtml([], 442), '');
check('步数为 0', roundMarksHtml(real, 0), '');
check('不是数组', roundMarksHtml(null, 442), '');
// 单局：没有竖线，也没有"第 2 段"可分。
const single = parse(roundMarksHtml([{ index: 0, label: '東1' }], 100));
check('单局无竖线', single.lines, []);
check('单局有标签', single.labels.map((label) => label.text), ['東1']);
// 缺局名（损坏帧）时只画线不写字。
const noLabel = parse(roundMarksHtml([{ index: 0, label: '' }, { index: 50, label: '' }], 100));
check('缺局名时只画线', noLabel.labels.length, 0);
check('缺局名时线照画', noLabel.lines.length, 1);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
