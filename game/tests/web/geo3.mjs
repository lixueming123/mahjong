/**
 * geo3 —— 用**真实 three.js**（vendor/three.module.js）驱动 OpponentHandsView
 * 的投影链路做端到端几何回归。
 *
 * 为什么要有它：geo2 是纯数学复刻（假设窗口恰好 1920x1080），而真机上
 * 窗口比例不一时主相机 frustum 是 vw=viewW 的宽设计区 —— pointAt 曾把
 * NDC 写死映射回 1920 宽，投影 x 向中心压缩 ~7%，钉牌解算据此把整条
 * 手牌链放错位（左家压副露、右家出屏）。这类 bug 只有真投影管线能抓到。
 *
 * 覆盖：窗口 1920x1080 与 2062x1080 两档 × 左/右/对家 × 副露 0~4 组 ×
 * 摸牌待打/打完。检查：
 *   1) 手牌链每张牌（8 角投影凸包）与该家副露每张牌（meldLayout 角点）
 *      无 SAT 相交；
 *   2) 手牌链全程在屏幕界内（压缩 k 生效后）；
 *   3) 右家手牌链（无副露居中态最坏）与自家副露带右缘留缝 ≥ 6px。
 */
import * as THREE from '../../web/vendor/three.module.js';
import { DESIGN, SEAT_LAYOUT, MELD, HAND } from '../../web/js/config.js';
import { OpponentHandsView } from '../../web/js/opponents.js';
import { meldLayout, meldNearPoint, flatten } from '../../web/js/meldLayout.js';

let fails = 0, checks = 0;
const fail = (msg) => { fails++; console.log('  ✗ ' + msg); };

const hull = (pts) => {
  pts = [...pts].sort((a, b) => a.x - b.x || a.y - b.y);
  const cr = (o, a, b) => (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  const lo = [];
  for (const p of pts) { while (lo.length >= 2 && cr(lo[lo.length - 2], lo[lo.length - 1], p) <= 0) lo.pop(); lo.push(p); }
  const up = [];
  for (let i = pts.length - 1; i >= 0; i--) { const p = pts[i]; while (up.length >= 2 && cr(up[up.length - 2], up[up.length - 1], p) <= 0) up.pop(); up.push(p); }
  return lo.slice(0, -1).concat(up.slice(0, -1));
};
/** 凸多边形 SAT，返回重叠深度（0 = 不相交） */
const satOverlap = (A, B) => {
  let best = Infinity;
  for (const poly of [A, B]) for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length];
    const nx = -(b.y - a.y), ny = b.x - a.x, l = Math.hypot(nx, ny) || 1;
    let minA = Infinity, maxA = -Infinity, minB = Infinity, maxB = -Infinity;
    for (const p of A) { const d = (p.x - a.x) * nx + (p.y - a.y) * ny; minA = Math.min(minA, d); maxA = Math.max(maxA, d); }
    for (const p of B) { const d = (p.x - a.x) * nx + (p.y - a.y) * ny; minB = Math.min(minB, d); maxB = Math.max(maxB, d); }
    const ov = Math.min(maxA, maxB) - Math.max(minA, minB);
    if (ov <= 0) return 0;
    best = Math.min(best, ov);
  }
  return best;
};

const LIMITS = { x0: 12, x1: 1908, y0: 12, y1: 1068 };

/** 复刻 opponents.js render() 的几何部分（真实 placeGroup/pointAt），
 *  返回该家手牌每张牌的 8 角投影。 */
function handPolys(view, seat, state) {
  const L = SEAT_LAYOUT[seat];
  const hand = state.hands?.[seat] ?? [];
  const n = hand.length;
  if (!n) return [];
  const group = new THREE.Group();
  view.placeGroup(group, L);

  const meldAngle = MELD.seat[seat]?.angle ?? L.angle;
  const growing = Math.cos(L.angle) * Math.cos(meldAngle) + Math.sin(L.angle) * Math.sin(meldAngle);
  const towardMeld = growing >= 0 ? 1 : -1;
  const pin = MELD.seat[seat]?.pin === false ? null : meldNearPoint(seat, state);
  let tNear = null, signAway = 1, k = 1;
  if (pin) {
    group.updateMatrixWorld(true);
    const a = view.basePoint(group, 0);
    const b = view.basePoint(group, 100);
    const dx = (b.x - a.x) / 100, dy = (b.y - a.y) / 100;
    const len = Math.hypot(dx, dy) || 1;
    const awayX = -Math.cos(L.angle) * towardMeld;
    const awayY = -Math.sin(L.angle) * towardMeld;
    signAway = dx * awayX + dy * awayY >= 0 ? 1 : -1;
    const baseGap = MELD.seat[seat].pinGap ?? 8;
    const solve = (gap) => {
      const tx = pin.x + awayX * gap, ty = pin.y + awayY * gap;
      const residual = (tt) => {
        const p = view.basePoint(group, tt);
        return ((p.x - tx) * dx + (p.y - ty) * dy) / len;
      };
      let t0 = 0, f0 = residual(t0), tn = 240, f1 = residual(tn);
      for (let i = 0; i < 8 && Math.abs(f1) > .5; i++) {
        const t2 = tn - f1 * (tn - t0) / ((f1 - f0) || 1);
        t0 = tn; f0 = f1; tn = t2; f1 = residual(tn);
      }
      return tn;
    };
    let meldEdge = -Infinity;
    const ml = meldLayout(MELD.seat[seat], state.melds.filter((m) => m.seat === seat),
      meldNearPoint(seat, state));
    if (ml) for (const t of ml.tiles) for (const c of t.corners) {
      const d = c.x * awayX + c.y * awayY;
      if (d > meldEdge) meldEdge = d;
    }
    const clearance = (tn) => {
      let handMin = Infinity;
      for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
        const c = view.pointAt(group, tn + sx * L.w / 2, L.h / 2 + sy * L.h / 2, sz * L.d / 2);
        handMin = Math.min(handMin, c.x * awayX + c.y * awayY);
      }
      return handMin - meldEdge;
    };
    let gap = baseGap;
    for (let i = 0; i < 4; i++) {
      tNear = solve(gap);
      const want = gap + (6 - clearance(tNear));
      if (Math.abs(want - gap) < .5) break;
      gap = want;
    }
    tNear = solve(gap);
  }

  // 远端出屏压缩（复刻 opponents.js：两端量测 + 二分求最大 k，无副露也查）
  group.updateMatrixWorld(true);
  const LIMITS = { x0: 12, x1: 1908, y0: 12, y1: 1068 };
  const overflowOf = (kk) => {
    let worst = 0;
    for (const i of [0, n - 1]) {
      const t = tNear == null
        ? (i - 6) * L.step * kk * towardMeld
        : tNear + signAway * (n - 1 - i) * L.step * kk;
      const isDrawn = n > 13 && i === n - 1;
      const fy = L.h * kk / 2 + (isDrawn ? 7 * kk : 0);
      const fz = isDrawn ? 5 * kk : 0;
      for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
        const c = view.pointAt(group, t + sx * L.w * kk / 2, fy + sy * L.h * kk / 2, fz + sz * L.d * kk / 2);
        const ox = Math.max(LIMITS.x0 - c.x, c.x - LIMITS.x1, 0);
        const oy = Math.max(LIMITS.y0 - c.y, c.y - LIMITS.y1, 0);
        worst = Math.max(worst, ox, oy);
      }
    }
    return worst;
  };
  if (overflowOf(1) > 0) {
    const K_MIN = .55;
    if (overflowOf(K_MIN) <= 0) {
      let lo = K_MIN, hi = 1;
      for (let it = 0; it < 12; it++) {
        const mid = (lo + hi) / 2;
        if (overflowOf(mid) > 0) hi = mid; else lo = mid;
      }
      k = lo;
    } else {
      k = K_MIN;
    }
  }
  group.updateMatrixWorld(true);
  const polys = [];
  for (let i = 0; i < n; i++) {
    const t = tNear == null ? (i - 6) * L.step * k * towardMeld
      : tNear + signAway * (n - 1 - i) * L.step * k;
    const isDrawn = n > 13 && i === n - 1;
    const cy = L.h * k / 2 + (isDrawn ? 7 * k : 0);
    const cz = isDrawn ? 5 * k : 0;
    const pts = [];
    for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
      pts.push(view.pointAt(group, t + sx * L.w * k / 2, cy + sy * L.h * k / 2, cz + sz * L.d * k / 2));
    }
    polys.push({ i, t, poly: hull(pts) });
  }
  return polys;
}

/** 自家副露落位（复刻 melds.js ownNear/ownMinLeft），返回每张牌的角点 */
function ownMeldPolys(melds, handCount, drawnPending) {
  if (!melds.length) return [];
  const L = MELD.seat[0];
  const drawn = drawnPending ? 1 : 0;
  const count = drawnPending ? handCount - 1 : handCount;
  const pitch = HAND.w + HAND.gap;
  const handRight = count ? HAND.left + (count - 1) * pitch + HAND.w : HAND.left + HAND.w;
  const minLeft = handRight + HAND.drawGap + HAND.w + 10;
  const { total } = flatten(L, melds, Infinity);
  const near = { x: Math.max(minLeft, L.right - total), y: L.y };
  const layout = meldLayout(L, melds, near);
  return layout.tiles.map((t) => ({ poly: hull(t.corners) }));
}

const mkState = (melds, hands) => ({ melds, hands });

// ---- 场景矩阵 ----
const WINDOWS = [
  { tag: '1920x1080', w: 1920, h: 1080 },
  { tag: '2062x1080', w: 2062, h: 1080 },
];
const meldSets = {
  1: [[], ['pon'], ['pon', 'pon'], ['pon', 'chi', 'pon'], ['pon', 'chi', 'pon', 'kan']],
  3: [[], ['chi'], ['chi', 'chi'], ['chi', 'pon', 'chi'], ['chi', 'pon', 'chi', 'kan']],
  2: [[], ['pon'], ['pon', 'pon'], ['pon', 'chi', 'pon'], ['kan', 'pon', 'chi', 'pon']],
};
const mkMelds = (seat, kinds) => kinds.map((kind, i) => ({
  seat, kind,
  tiles: kind === 'kan' ? ['1z', '1z', '1z', '1z'] : ['6m', '7m', '8m'],
  rotated: kind === 'kan' ? 3 : 0,
}));

for (const win of WINDOWS) {
  const scale = Math.min(win.w / DESIGN.W, win.h / DESIGN.H);
  const vw = win.w / scale, vh = win.h / scale;
  const view = new OpponentHandsView();
  view.resize(win.w / win.h, vw, vh);

  for (const seat of [1, 2, 3]) {
    for (let gi = 0; gi < meldSets[seat].length; gi++) {
      for (const drawn of [false, true]) {
        const melds = mkMelds(seat, meldSets[seat][gi]);
        // 手牌数：每副露一组净减 3 张，杠不例外 —— 服务端 room.cc
        // ApplyMeld/ApplySelfKan 都是「扣牌 → 岭上补 1 张 → 才发 state_sync」：
        // 杠拿 3 张离手（meld 4 张里 1 张是被鸣牌）+ 补 1 张 = 净 -3；
        // 碰/吃扣 3 张后补打 1 张（13 基准口径）也是净 -3。所以 static 13-3m、
        // 摸牌待打 14-3m，对碰/吃/明杠/暗杠/加杠统一成立（本家 13-3g 同源）。
        const handCount = 13 - melds.length * 3 + (drawn ? 1 : 0);
        const state = mkState(melds, {
          [seat]: Array(Math.max(1, handCount)).fill('back'),
        });
        const tag = `${win.tag} seat${seat} melds=${meldSets[seat][gi].length}${drawn ? '+摸' : ''}`;

        const polys = handPolys(view, seat, state);
        // 1) 手牌 × 副露
        const ml = melds.length
          ? meldLayout(MELD.seat[seat], melds, meldNearPoint(seat, state))
          : null;
        if (ml) for (const h of polys) for (const m of ml.tiles) {
          const ov = satOverlap(h.poly, hull(m.corners));
          if (ov > .5) fail(`${tag}: 手牌#${h.i} × 副露(${m.code}) SAT 重叠 ${ov.toFixed(1)}px`);
        }
        checks++;
        // 2) 出屏
        for (const h of polys) for (const p of h.poly) {
          if (p.x < LIMITS.x0 - 2 || p.x > LIMITS.x1 + 2 || p.y < LIMITS.y0 - 2 || p.y > LIMITS.y1 + 2) {
            fail(`${tag}: 手牌#${h.i} 出屏 (${p.x.toFixed(0)},${p.y.toFixed(0)})`);
            break;
          }
        }
        checks++;
      }
    }
  }

  // 3) 右家手牌（居中态最坏）× 自家副露右缘
  for (const drawn of [false, true]) {
    for (let ownGroups = 1; ownGroups <= 3; ownGroups++) {
      const seat1Melds = mkMelds(1, meldSets[1][0]);           // 右家无副露（居中，最坏）
      const ownMelds = mkMelds(0, Array(ownGroups).fill('pon'));
      const state = mkState([...seat1Melds, ...ownMelds], {
        1: Array(13 + (drawn ? 1 : 0)).fill('back'),
        0: Array(13 - ownGroups * 3 + (drawn ? 1 : 0)).fill('back'),
      });
      const polys = handPolys(view, 1, state);
      const ownPolys = ownMeldPolys(ownMelds, 13 - ownGroups * 3, drawn);
      let minGap = Infinity;
      for (const h of polys) for (const o of ownPolys) {
        // 两凸包最近距离（都分离时）：用 SAT 的反向量不好拿，直接取顶点对最小距
        for (const a of h.poly) for (const b of o.poly) minGap = Math.min(minGap, Math.hypot(a.x - b.x, a.y - b.y));
      }
      if (minGap < 6) fail(`2062 右家(居中)×自家副露${ownGroups}组 擦边 ${minGap.toFixed(1)}px`);
      checks++;
    }
  }
}

console.log(`geo3: ${checks} 组场景，${fails} 处失败`);
process.exit(fails ? 1 : 0);
