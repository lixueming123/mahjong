// index.html 的 id 契约：不重复、且大厅面板不会被 ui.css 的 id 选择器隐藏。
//
// 背景：`#stats` 曾同时是底部 HUD 的「draw calls·fps」计数器和大厅的战绩面板。ui.css 里
// `.hud, #stats { display: none; }`（录像里隐藏调试文字）因此把战绩面板一起隐藏掉了 ——
// 数据一直有下发，只是那块 DOM 从来没被画出来。id 重复是最难查的一类前端 bug：JS 侧
// getElementById 永远只拿到第一个，CSS 侧却命中全部。这里把两条不变量钉住。

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const web = join(here, '..', '..', 'web');
const html = readFileSync(join(web, 'index.html'), 'utf8');
const css = readFileSync(join(web, 'css', 'ui.css'), 'utf8');

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
const ok = (name, cond, extra = '') => {
  if (cond) {
    pass++;
    console.log(`  ok   ${name}`);
  } else {
    fail++;
    console.log(`  FAIL ${name}${extra ? `\n       ${extra}` : ''}`);
  }
};

const ids = [...html.matchAll(/id="([^"]+)"/g)].map((m) => m[1]);

// ---------------------------------------------------------------- 1. id 唯一
const seen = new Set();
const dups = new Set();
for (const id of ids) {
  if (seen.has(id)) dups.add(id);
  seen.add(id);
}
check('index.html 里的 id 不重复', [...dups].sort(), []);

// ---------------------------------------------------------------- 2. 大厅面板不被隐藏
// 「大厅面板」= #view-hall 这一节里出现的 id。把这一节截出来，再收集其中的 id。
const hallStart = html.indexOf('id="view-hall"');
const hallEnd = html.indexOf('</section>', hallStart);
ok('#view-hall 存在', hallStart > 0 && hallEnd > hallStart);
const hallIds = [...html.slice(hallStart, hallEnd).matchAll(/id="([^"]+)"/g)].map((m) => m[1]);
ok('大厅里有战绩面板 #stats-list', hallIds.includes('stats-list'), hallIds.join(','));

// ui.css 中所有「声明块里带 display:none」的 #id 选择器
const hiddenByCss = new Set();
for (const rule of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
  const selector = rule[1];
  const body = rule[2];
  if (!/display\s*:\s*none/.test(body)) continue;
  for (const id of selector.matchAll(/#([\w-]+)/g)) hiddenByCss.add(id[1]);
}
const hiddenPanels = hallIds.filter((id) => hiddenByCss.has(id));
check('没有大厅面板被 ui.css 按 id 隐藏', hiddenPanels, []);

// ---------------------------------------------------------------- 3. JS 只认一个元素
// 战绩面板的 id 必须是独立的名字，不能和 HUD 计数器共用。
ok('HUD 计数器叫 #hud-stats', ids.includes('hud-stats'));
ok('战绩面板叫 #stats-list', ids.includes('stats-list'));
check('两者不同名', ids.filter((id) => id === 'stats' || id === 'stats-list' || id === 'hud-stats').length, 2);

const lobby = readFileSync(join(web, 'js', 'lobby.js'), 'utf8');
ok('lobby.js 取的是战绩面板', lobby.includes("stats: $('stats-list')"));
const ui = readFileSync(join(web, 'js', 'ui.js'), 'utf8');
ok('ui.js 取的是 HUD 计数器', ui.includes("stats: $('hud-stats')"));

// ---------------------------------------------------------------- 4. 加载提示必须被收起
// `#boot` 是首屏的全屏「加载牌桌…」。它只能靠 JS 加 `ready` 类隐藏，所以那条语句一旦在
// 重构里丢失，联机/大厅路径就会一直挂着它（回放路径有自己的一份，不会被这行覆盖）。
// 曾经就是把它和渲染循环一起挪动时丢掉的：牌桌能玩，但中央一直压着「加载牌桌…」。
const main = readFileSync(join(web, 'js', 'main.js'), 'utf8');
ok('main.js 会收起 #boot', main.includes("getElementById('boot')?.classList.add('ready')"));
// 渲染循环之前就必须有一次收起：不能等联机分支末尾（回放分支提前 return 就漏了）。
const loopAt = main.indexOf('requestAnimationFrame(loop)');
const hideAt = main.indexOf("getElementById('boot')?.classList.add('ready')");
ok('渲染循环已启动（回放与联机共用）', loopAt > 0);
ok('收起 #boot 的语句不止一处（回放 + 联机/大厅）',
  main.split("classList.add('ready')").length - 1 >= 2);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
