/**
 * 程序化纹理：台布 / 木框 / 单张牌背 / 舍牌牌身 / 中央八角情报板。
 * 只产出「素材」，不承担任何摆放逻辑：牌是一张张独立对象，由 opponents/rivers/hand 装配。
 */
import * as THREE from 'three';
import { DESIGN, BOARD, COLOR, P } from './config.js';
import { FONT } from './assets.js';

function canvas(w, h) {
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  return c;
}

function texture(c) {
  const t = new THREE.CanvasTexture(c);
  t.colorSpace = THREE.SRGBColorSpace;
  t.anisotropy = 8;
  t.needsUpdate = true;
  return t;
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

/* ---------------------------------------------------------------- 台布 + 木框 */

export function makeClothTexture() {
  const W = DESIGN.W, H = DESIGN.H;
  const c = canvas(W, H), g = c.getContext('2d');

  // 底层：深红木
  const wood = g.createLinearGradient(0, 0, W, H);
  wood.addColorStop(0, '#180706');
  wood.addColorStop(.35, COLOR.woodMid);
  wood.addColorStop(.7, COLOR.woodDark);
  wood.addColorStop(1, COLOR.woodLight);
  g.fillStyle = wood;
  g.fillRect(0, 0, W, H);

  // 木纹
  g.save();
  g.globalAlpha = .12;
  for (let i = 0; i < 90; i++) {
    const y = (i * 37) % H;
    g.strokeStyle = i % 3 ? '#000' : '#e8a06a';
    g.lineWidth = 1 + (i % 4) * .5;
    g.beginPath(); g.moveTo(0, y); g.bezierCurveTo(W * .3, y + 12, W * .7, y - 12, W, y + 5); g.stroke();
  }
  g.restore();

  // 木框高光：左上受光、右下压暗
  const woodSheen = g.createLinearGradient(0, 0, W, H);
  woodSheen.addColorStop(0, 'rgba(255,190,130,.14)');
  woodSheen.addColorStop(.5, 'rgba(0,0,0,0)');
  woodSheen.addColorStop(1, 'rgba(0,0,0,.25)');
  g.fillStyle = woodSheen;
  g.fillRect(0, 0, W, H);

  // 台布八边形（四角切掉露木框）
  const oct = [[110, 0], [1810, 0], [W, 70], [W, 1010], [1810, H], [110, H], [0, 1010], [0, 70]];
  g.save();
  g.beginPath();
  oct.forEach(([x, y], i) => i ? g.lineTo(x, y) : g.moveTo(x, y));
  g.closePath();
  g.clip();

  const rg = g.createRadialGradient(960, 420, 60, 960, 500, 1150);
  rg.addColorStop(0, COLOR.clothLight);
  rg.addColorStop(.42, COLOR.clothMid);
  rg.addColorStop(1, COLOR.clothDark);
  g.fillStyle = rg;
  g.fillRect(0, 0, W, H);

  // 中央柔和光池：模拟头顶灯光打在台布上
  const pool = g.createRadialGradient(960, 470, 40, 960, 470, 760);
  pool.addColorStop(0, 'rgba(120,175,235,.22)');
  pool.addColorStop(.6, 'rgba(80,130,200,.06)');
  pool.addColorStop(1, 'rgba(0,0,0,0)');
  g.fillStyle = pool;
  g.fillRect(0, 0, W, H);

  // 毛毡细纤维：固定种子的伪随机，避免规则点阵出现摩尔纹
  let seed = 20240607;
  const rnd = () => {
    seed = (seed * 1664525 + 1013904223) >>> 0;
    return seed / 4294967296;
  };
  for (let i = 0; i < 22000; i++) {
    const x = rnd() * W, y = rnd() * H;
    const light = rnd() < .5;
    g.globalAlpha = .02 + rnd() * .04;
    g.fillStyle = light ? '#cfe4ff' : '#000814';
    g.fillRect(x, y, 1 + rnd() * 2, 1 + rnd() * 2);
  }
  g.globalAlpha = 1;

  // 四周暗角，把视线收向牌桌中央
  const vg = g.createRadialGradient(960, 540, 520, 960, 540, 1180);
  vg.addColorStop(0, 'rgba(0,0,0,0)');
  vg.addColorStop(1, 'rgba(0,6,18,.2)');
  g.fillStyle = vg;
  g.fillRect(0, 0, W, H);

  // 放射线：外框角 -> 中央板四角
  g.strokeStyle = COLOR.clothLine;
  g.lineWidth = 2;
  const bx0 = BOARD.x - BOARD.w / 2, bx1 = BOARD.x + BOARD.w / 2;
  const by0 = BOARD.y - BOARD.h / 2, by1 = BOARD.y + BOARD.h / 2;
  [[540, 115], [1380, 115], [1500, 800], [420, 800]].forEach(([ox, oy], i) => {
    const tx = i === 0 || i === 3 ? bx0 : bx1;
    const ty = i < 2 ? by0 : by1;
    g.beginPath(); g.moveTo(ox, oy); g.lineTo(tx, ty); g.stroke();
  });

  // 内框透视梯形
  g.beginPath();
  g.moveTo(540, 115); g.lineTo(1380, 115); g.lineTo(1500, 800); g.lineTo(420, 800); g.closePath();
  g.stroke();

  // 中央板外圈细框
  g.strokeStyle = 'rgba(8,22,42,.45)';
  g.lineWidth = 3;
  g.strokeRect(bx0 - 42, by0 - 34, BOARD.w + 84, BOARD.h + 68);
  g.restore();

  // 台布金边
  g.beginPath();
  oct.forEach(([x, y], i) => i ? g.lineTo(x, y) : g.moveTo(x, y));
  g.closePath();
  g.strokeStyle = 'rgba(214,164,88,.75)'; g.lineWidth = 4; g.stroke();
  g.strokeStyle = 'rgba(0,0,0,.55)'; g.lineWidth = 10;
  g.beginPath();
  oct.forEach(([x, y], i) => i ? g.lineTo(x, y) : g.moveTo(x, y));
  g.closePath();
  g.globalCompositeOperation = 'destination-over'; g.stroke(); g.globalCompositeOperation = 'source-over';

  return texture(c);
}

/* ---------------------------------------------------------------- 中央情报板

   注：三家立牌的牌背不再由这里预渲染。旧版把「白色侧棱 + 落影 + 高光」
   全部画进 Canvas 贴图，正是和 pygame 的 Surface.blit 撞脸的原因。
   现在牌背只保留印刷纹样（tile3d.js），侧面与落影交给几何 + 灯光。

--------------------------------------------------------------------------- */

function windBadge(g, x, y, char, bg, fg) {
  g.save();
  g.translate(x, y);
  g.fillStyle = 'rgba(0,0,0,.45)';
  roundRect(g, -27, -25, 58, 52, 9); g.fill();
  g.fillStyle = bg;
  roundRect(g, -29, -27, 58, 52, 9); g.fill();
  g.strokeStyle = '#8b8f9a'; g.lineWidth = 2; g.stroke();
  g.fillStyle = fg;
  g.font = `bold 33px ${FONT}`;
  g.textAlign = 'center'; g.textBaseline = 'middle';
  g.fillText(char, 0, 1);
  g.restore();
}

export function makeBoardTexture(state) {
  const PW = BOARD.w + 130, PH = BOARD.h + 110;
  const c = canvas(PW, PH), g = c.getContext('2d');
  const cx = PW / 2, cy = PH / 2;
  const w = BOARD.w, h = BOARD.h;
  const cut = 34;

  const octagon = (ow, oh, ct) => {
    g.beginPath();
    g.moveTo(-ow / 2 + ct, -oh / 2);
    g.lineTo(ow / 2 - ct, -oh / 2);
    g.lineTo(ow / 2, -oh / 2 + ct);
    g.lineTo(ow / 2, oh / 2 - ct);
    g.lineTo(ow / 2 - ct, oh / 2);
    g.lineTo(-ow / 2 + ct, oh / 2);
    g.lineTo(-ow / 2, oh / 2 - ct);
    g.lineTo(-ow / 2, -oh / 2 + ct);
    g.closePath();
  };

  g.save();
  g.translate(cx, cy);

  // 投影
  g.save(); g.translate(4, 6);
  octagon(w + 8, h + 8, cut + 2);
  g.fillStyle = 'rgba(0,0,0,.5)'; g.fill(); g.restore();

  // 外层
  octagon(w, h, cut);
  g.fillStyle = '#2b2c33'; g.fill();
  g.strokeStyle = '#7d818c'; g.lineWidth = 3; g.stroke();

  // 内层斜切装饰
  octagon(w - 16, h - 16, cut - 4);
  g.fillStyle = COLOR.board; g.fill();
  g.strokeStyle = '#55575f'; g.lineWidth = 2; g.stroke();

  // 上下左右四个梯形分区
  g.fillStyle = 'rgba(255,255,255,.045)';
  const seg = [
    [[-58, -h / 2 + 10], [58, -h / 2 + 10], [78, -30], [-78, -30]],
    [[-58, h / 2 - 10], [58, h / 2 - 10], [78, 30], [-78, 30]],
  ];
  seg.forEach((pts) => {
    g.beginPath(); pts.forEach(([x, y], i) => i ? g.lineTo(x, y) : g.moveTo(x, y));
    g.closePath(); g.fill();
  });

  // 中心圆角方块
  roundRect(g, -66, -48, 132, 96, 12);
  g.fillStyle = '#202128'; g.fill();
  g.strokeStyle = '#6b6e78'; g.lineWidth = 2; g.stroke();
  g.textAlign = 'center'; g.textBaseline = 'middle';
  g.fillStyle = '#4ad6cf'; g.font = `bold 27px ${FONT}`;
  g.fillText(state.round || '東2局', 0, -18);
  g.fillStyle = '#f0c04a'; g.font = `bold 22px ${FONT}`;
  g.fillText(`余 ${state.remaining ?? 57}`, 0, 19);

  // 四家点数（左右竖排）
  const pts = state.scores || [29500, 27400, 22800, 20300];
  g.fillStyle = '#f2b23c'; g.font = `bold 19px ${FONT}`;
  g.fillText(String(pts[2]), 0, -h / 2 + 22);
  g.fillText(String(pts[0]), 0, h / 2 - 22);
  [-1, 1].forEach((s) => {
    const txt = String(pts[s === -1 ? 3 : 1]);
    const x = s * (w / 2 - 30);
    [...txt].forEach((ch, i) => g.fillText(ch, x, -42 + i * 21));
  });

  // 顶部余牌金条
  roundRect(g, -40, -h / 2 + 4, 80, 7, 3);
  g.fillStyle = '#f5c55a'; g.fill();

  g.restore();

  // 四角自风徽标
  // 贴在八角板的**内侧**四角（录像：徽标在板角里，牌河紧贴着板边），
  // 往外挪会被牌河盖住一半，往中间挪又会压到点数
  const bx = 72, by = 84;
  windBadge(g, bx, by, '北', '#1b2340', '#dfe6f5');
  windBadge(g, PW - bx, by, '東', '#8e1f1f', '#1a1a1a');
  windBadge(g, bx, PH - by, '西', '#1b2340', '#dfe6f5');
  windBadge(g, PW - bx, PH - by, '南', '#8e1f1f', '#1a1a1a');

  return { texture: texture(c), width: PW, height: PH };
}

/** 通用纯色/渐变矩形纹理（按钮、指示条等） */
export function planeTexture(w, h, draw) {
  const c = canvas(w, h), g = c.getContext('2d');
  draw(g, w, h);
  return texture(c);
}

export { roundRect };
