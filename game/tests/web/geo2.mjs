/**
 * geo2.mjs — 复算「三家透视立牌 × 副露带」的屏幕几何（多边形级判定）。
 *
 * 副露落位直接 import web 端共享模块 meldLayout.js（渲染/解算/回归三方
 * 同一份实现，杜绝「脚本一个口径、页面另一个口径」）；opponents.js 的
 * 透视相机 + placeGroup + 多边形级钉牌解算在此复刻，纯 node，无浏览器。
 * 手牌立牌 = 8 角投影凸包；副露躺牌 = meldLayout 的外轮廓角点；
 * 重叠用 SAT 分离轴量真实侵入深度。
 */
import { readFileSync } from 'node:fs';

// 纯常量/纯数学模块，无 DOM 依赖，可直接 import
const cfg = await import('../../web/js/config.js');
const { DESIGN, SEAT_LAYOUT, MELD } = cfg;
const layoutMod = await import('../../web/js/meldLayout.js');
const { seatMeldLayout, flatten, meldNearPoint } = layoutMod;

const CX = DESIGN.CX, CY = DESIGN.CY;
const D2R = Math.PI / 180;

/* ---------------- 透视相机（opponents.js 构造参数） ---------------- */
const CAM_POS = { x: 0, y: -1150, z: 1250 };
const CAM_TARGET = { x: 0, y: 0, z: 60 };
const FOV = 32, ASPECT = 16 / 9;
const TAN = Math.tan((FOV / 2) * D2R);

const sub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y, z: a.z - b.z });
const add = (a, b) => ({ x: a.x + b.x, y: a.y + b.y, z: a.z + b.z });
const mul = (a, s) => ({ x: a.x * s, y: a.y * s, z: a.z * s });
const dot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const cross = (a, b) => ({
  x: a.y * b.z - a.z * b.y,
  y: a.z * b.x - a.x * b.z,
  z: a.x * b.y - a.y * b.x,
});
const norm = (a) => { const l = Math.hypot(a.x, a.y, a.z); return mul(a, 1 / l); };

const ZAXIS = norm(sub(CAM_POS, CAM_TARGET));
const XAXIS = norm(cross({ x: 0, y: 1, z: 0 }, ZAXIS));
const YAXIS = cross(ZAXIS, XAXIS);
const FWD = mul(ZAXIS, -1);

function worldToDesign(p) {
  const d = sub(p, CAM_POS);
  const depth = -dot(d, ZAXIS);
  const xn = (dot(d, XAXIS) / TAN / ASPECT) / depth;
  const yn = (dot(d, YAXIS) / TAN) / depth;
  return { x: CX * (xn + 1), y: CY * (1 - yn) };
}

function unprojectToTable(dx, dy) {
  const nx = 2 * (dx - CX) / 1920;
  const ny = 2 * (CY - dy) / 1080;
  const dir = add(FWD, add(mul(XAXIS, nx * ASPECT * TAN), mul(YAXIS, ny * TAN)));
  const t = -CAM_POS.z / dir.z;
  return add(CAM_POS, mul(dir, t));
}

function hull(pts) {
  const p = [...pts].sort((a, b) => a.x - b.x || a.y - b.y);
  const cross2 = (o, a, b) => (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  const lo = [];
  for (const pt of p) {
    while (lo.length >= 2 && cross2(lo.at(-2), lo.at(-1), pt) <= 0) lo.pop();
    lo.push(pt);
  }
  const up = [];
  for (let i = p.length - 1; i >= 0; i--) {
    const pt = p[i];
    while (up.length >= 2 && cross2(up.at(-2), up.at(-1), pt) <= 0) up.pop();
    up.push(pt);
  }
  return lo.slice(0, -1).concat(up.slice(0, -1));
}

/** 两凸多边形分离轴判定：分离返回 0，相交返回近似最小侵入深度 */
function satPenetration(A, B) {
  let best = Infinity;
  for (const P of [A, B]) {
    for (let i = 0; i < P.length; i++) {
      const a = P[i], b = P[(i + 1) % P.length];
      const nx = b.y - a.y, ny = -(b.x - a.x);
      const l = Math.hypot(nx, ny) || 1;
      let minA = Infinity, maxA = -Infinity, minB = Infinity, maxB = -Infinity;
      for (const p of A) {
        const d = (p.x * nx + p.y * ny) / l;
        minA = Math.min(minA, d); maxA = Math.max(maxA, d);
      }
      for (const p of B) {
        const d = (p.x * nx + p.y * ny) / l;
        minB = Math.min(minB, d); maxB = Math.max(maxB, d);
      }
      const ov = Math.min(maxA, maxB) - Math.max(minA, minB);
      if (ov <= 0) return 0;
      best = Math.min(best, ov);
    }
  }
  return best;
}

/* ---------------- opponents.js 复刻（含多边形级钉牌解算） ---------------- */
class Group {
  constructor(pos, u, n, scale) {
    this.pos = pos; this.u = u; this.n = n; this.scale = scale;
  }
  point(x, y, z) {
    const s = this.scale;
    const w = add(this.pos, add(mul(this.u, x * s),
      add(mul({ x: 0, y: 0, z: 1 }, y * s), mul(this.n, z * s))));
    return worldToDesign(w);
  }
  base(t) { return this.point(t, 0, 0); }
}

function placeGroup(L) {
  const half = 12 * L.step / 2;
  const ax = Math.cos(L.angle), ay = Math.sin(L.angle);
  const drop = Math.round(L.h * .31);
  const p1 = unprojectToTable(L.cx - ax * half, L.cy + drop - ay * half);
  const p2 = unprojectToTable(L.cx + ax * half, L.cy + drop + ay * half);
  p1.z = 0; p2.z = 0;
  const pos = mul(add(p1, p2), .5);
  let u = norm(sub(p2, p1));
  const d = sub(p2, p1);
  const len3d = Math.hypot(d.x, d.y, d.z);
  const UP = { x: 0, y: 0, z: 1 };
  let n = norm(cross(u, UP));
  const toCamV = sub(CAM_POS, pos); toCamV.z = 0;
  const toCam = norm(toCamV);
  if (dot(n, toCam) < 0) { u = mul(u, -1); n = norm(cross(u, UP)); }
  return new Group(pos, u, n, len3d / (12 * L.step));
}

function solveSeat(seat, handCount, melds) {
  const L = SEAT_LAYOUT[seat];
  const ML = MELD.seat[seat];
  const group = placeGroup(L);
  const pin = meldNearPoint(seat, { melds });

  const meldAngle = ML?.angle ?? L.angle;
  const growing = Math.cos(L.angle) * Math.cos(meldAngle) + Math.sin(L.angle) * Math.sin(meldAngle);
  const towardMeld = growing >= 0 ? 1 : -1;

  let tNear = null, signAway = 1, gapUsed = null, kUsed = 1, clearUsed = null;
  if (pin) {
    const a = group.base(0);
    const b = group.base(100);
    const dx = (b.x - a.x) / 100;
    const dy = (b.y - a.y) / 100;
    const len = Math.hypot(dx, dy) || 1;
    const awayX = -Math.cos(L.angle) * towardMeld;
    const awayY = -Math.sin(L.angle) * towardMeld;
    signAway = dx * awayX + dy * awayY >= 0 ? 1 : -1;
    const baseGap = ML.pinGap ?? 8;

    const solve = (gap) => {
      const tx = pin.x + awayX * gap;
      const ty = pin.y + awayY * gap;
      const residual = (tt) => {
        const p = group.base(tt);
        return ((p.x - tx) * dx + (p.y - ty) * dy) / len;
      };
      let t0 = 0, f0 = residual(0);
      let tn = 240, f1 = residual(tn);
      for (let it = 0; it < 8 && Math.abs(f1) > .5; it++) {
        const t2 = tn - f1 * (tn - t0) / ((f1 - f0) || 1);
        t0 = tn; f0 = f1; tn = t2; f1 = residual(tn);
      }
      return tn;
    };

    // 多边形级间距（与 opponents.js 同口径）：手牌末端 8 角沿 away 的
    // 最小投影 − 副露全部牌角点最大投影
    const layout = seatMeldLayout(seat, { melds });
    let meldEdge = -Infinity;
    for (const t of layout.tiles) {
      for (const c of t.corners) {
        const d = c.x * awayX + c.y * awayY;
        if (d > meldEdge) meldEdge = d;
      }
    }
    const clearance = (tn) => {
      let handMin = Infinity;
      for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
        const c = group.point(tn + sx * L.w / 2, L.h / 2 + sy * L.h / 2, sz * L.d / 2);
        handMin = Math.min(handMin, c.x * awayX + c.y * awayY);
      }
      return handMin - meldEdge;
    };
    let gap = baseGap;
    for (let it = 0; it < 4; it++) {
      tNear = solve(gap);
      const want = gap + (6 - clearance(tNear));
      if (Math.abs(want - gap) < .5) break;
      gap = want;
    }
    tNear = solve(gap);
    gapUsed = gap;
    clearUsed = clearance(tNear);

    // 远端出屏压缩（复刻 opponents.js 同名逻辑）
    const LIMITS = { x0: 12, x1: 1908, y0: 12, y1: 1068 };
    const overflowOf = (kk) => {
      const tf = tNear + signAway * (handCount - 1) * L.step * kk;
      let worst = 0, worstPt = null;
      for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
        const c = group.point(
          tf + sx * L.w * kk / 2,
          L.h * kk / 2 + sy * L.h * kk / 2,
          sz * L.d * kk / 2);
        const ox = Math.max(LIMITS.x0 - c.x, c.x - LIMITS.x1, 0);
        const oy = Math.max(LIMITS.y0 - c.y, c.y - LIMITS.y1, 0);
        if (Math.max(ox, oy) > worst) { worst = Math.max(ox, oy); worstPt = c; }
      }
      return { worst, worstPt };
    };
    let kk = 1;
    let of1 = overflowOf(1);
    if (of1.worst > 0) {
      const cPin = group.base(tNear);
      const reach = Math.max(60, Math.hypot(of1.worstPt.x - cPin.x, of1.worstPt.y - cPin.y));
      kk = Math.max(.55, 1 - of1.worst / reach);
      const rest = overflowOf(kk).worst;
      if (rest > 0) kk = Math.max(.55, kk * (1 - rest / reach));
    }
    kUsed = kk;
  }

  const n = handCount;
  const tiles = [];
  for (let i = 0; i < n; i++) {
    const t = tNear == null ? (i - 6) * L.step * towardMeld : tNear + signAway * (n - 1 - i) * L.step * kUsed;
    const isDrawn = n > 13 && i === n - 1;
    const cy = L.h * kUsed / 2 + (isDrawn ? 7 * kUsed : 0);
    const cz = isDrawn ? 5 * kUsed : 0;
    const corners = [];
    for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
      corners.push(group.point(t + sx * L.w * kUsed / 2, cy + sy * L.h * kUsed / 2, cz + sz * L.d * kUsed / 2));
    }
    tiles.push({ i, t, isDrawn, poly: hull(corners) });
  }
  return { group, tiles, pin, tNear, signAway, gapUsed, kUsed, clearUsed, L };
}

/* ---------------- 主流程 ---------------- */
let fails = 0;
function runCase(seat, groups, drawn, quiet) {
  const melds = [];
  for (let g = 0; g < groups; g++) {
    melds.push({ seat, kind: 'chi', tiles: ['1m', '2m', '3m'], rotated: g === 0 ? 0 : -1 });
  }
  const handCount = 13 - 3 * groups + (drawn ? 1 : 0);
  const r = solveSeat(seat, handCount, melds);
  const fp = seatMeldLayout(seat, { melds }).tiles;
  const seatName = { 1: '右家', 3: '左家' }[seat];
  const tag = `${seatName} 副露${groups}组 ${drawn ? '摸牌待打' : '打完后'}`;

  // 1) 全部手牌 × 全部副露牌，多边形真实侵入
  let worst = null;
  for (const h of r.tiles) {
    for (const m of fp) {
      const pen = satPenetration(h.poly, m.corners);
      if (pen > .5 && (!worst || pen > worst.pen)) {
        worst = { pen, h: h.i, m: m.i, claimed: m.claimed };
      }
    }
  }
  if (worst) {
    fails++;
    if (!quiet) console.log(`✗ ${tag}: 手牌第${worst.h}张 × 副露第${worst.m}张` +
      `${worst.claimed ? '(被鸣横放)' : ''} 侵入 ${worst.pen.toFixed(1)}px`);
  } else if (!quiet) {
    console.log(`✓ ${tag}: 手牌×副露多边形级无重叠 ` +
      `(gap=${r.gapUsed?.toFixed(0)} clear=${r.clearUsed?.toFixed(1)})`);
  }

  // 2) 副露组内互压：设计上 seam=-3 允许 ~3px，>6px 判失败
  for (let i = 0; i < fp.length; i++) {
    for (let j = i + 1; j < fp.length; j++) {
      const pen = satPenetration(fp[i].corners, fp[j].corners);
      if (pen > 6) {
        fails++;
        if (!quiet) console.log(`✗ ${tag}: 副露第${i}张×第${j}张 多边形侵入 ${pen.toFixed(1)}px`);
      }
    }
  }

  // 3) 手牌不能被推出屏幕（与 opponents.js 的 LIMITS 完全一致）
  for (const h of r.tiles) {
    for (const p of h.poly) {
      if (p.x < 12 || p.x > 1908 || p.y < 12 || p.y > 1068) {
        fails++;
        if (!quiet) console.log(`✗ ${tag}: 手牌第${h.i}张越界 (${p.x.toFixed(0)},${p.y.toFixed(0)})`);
      }
    }
  }
  if (!quiet && r.kUsed < 1) {
    console.log(`  · ${tag}: 远端出屏压缩 k=${r.kUsed.toFixed(2)}`);
  }
  return r;
}

const only = process.argv.includes('--quiet');
console.log('=== 修后验证（多边形级钉牌） ===');
for (const seat of [3, 1]) {
  for (let g = 1; g <= 4; g++) {
    runCase(seat, g, false);
    runCase(seat, g, true);
  }
}

// 无副露时手牌应保持居中布局（原口径不动）
{
  const melds = [];
  const r = solveSeat(3, 13, melds);
  console.log(`✓ 无副露：左家 13 张居中布局，末端牌中心 t=${r.tiles.at(-1).t.toFixed(1)}（期望 ±288 附近）`);
}

console.log(`\nfail=${fails}`);
process.exit(fails ? 1 : 0);
