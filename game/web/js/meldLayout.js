/**
 * 副露带的纯几何落位 —— 无 THREE 依赖，node 测试可直接 import。
 *
 * 为什么要单独一个模块：melds.js（渲染）、opponents.js（钉牌解算）、
 * tests/web/geo2.mjs（回归）三方必须用**同一份**落位数学。以前 opponents
 * 只拿 meldNearPoint 的「钉点」当参照，而被鸣横放牌经 tilt 旋转 + drop
 * 侧移后，它的角点会沿「朝手牌」方向凸出钉点 10~20px —— 钉点间距再大
 * 也挡不住角上的重叠（用户截图实锤）。把多边形角点也放在这份实现里，
 * 三方共享，接缝口径永远一致。
 */
import { MELD } from './config.js';

/** 组内顺序：服务端下发即牌面顺序（§2.2 已排序），绝不重排；
 *  被鸣那张按 rotated 下标标记。 */
export function meldOrder(meld) {
  const tiles = meld.tiles || [];
  const claimedAt = Number.isInteger(meld.rotated) ? meld.rotated : -1;
  return tiles.map((code, i) => ({ code, claimed: i === claimedAt }));
}

/**
 * 牌沿排布方向占的长度（含右侧金边与下缘金边）。
 * 牌是绕自身中心转过 tilt 的矩形，在方向 u 上的投影长度是
 *     (w+side)·|cos θ| + (h+thick)·|sin θ|,  θ = angle − tilt
 */
export function tileExtent(L, w, h) {
  const a = L.angle - (L.tilt || 0);
  return Math.abs((w + (L.side || 0)) * Math.cos(a))
    + Math.abs((h + L.thick) * Math.sin(a));
}

/**
 * 展开成一串牌并量出总长：组内顺序＝服务端牌面顺序，组与组按叫牌先后往后接。
 * `maxLen` 是副露带允许的长度，放不下时整块等比缩小（k < 1）。
 */
export function flatten(L, melds, maxLen = Infinity) {
  const build = (k) => {
    const flat = [];
    melds.forEach((meld, gi) => {
      for (const item of meldOrder(meld)) {
        // 被鸣牌宽高互换（麻将规则里的「横放」）
        const swap = item.claimed && L.swap;
        const w = (swap ? L.h : L.w) * k;
        const h = (swap ? L.w : L.h) * k;
        flat.push({ ...item, w, h, group: gi });
      }
    });
    const scaled = { ...L, side: L.side * k, thick: L.thick * k };
    let total = 0;
    flat.forEach((item, i) => {
      item.extent = tileExtent(scaled, item.w, item.h);
      total += item.extent;
      if (i < flat.length - 1) {
        total += (flat[i + 1].group === item.group ? L.seam : L.gap) * k;
      }
    });
    return { flat, total, k };
  };

  const natural = build(1);
  if (natural.total <= maxLen) return natural;
  return build(maxLen / natural.total);
}

/**
 * 副露每张牌在设计坐标系（x 右、y 下）里的落位与外轮廓角点。
 *
 * L     ：MELD.seat[n]（三家有 far；本家由 melds.js 自行算 near）
 * melds ：该家的副露数组
 * near  ：带子起点（靠手牌那一端）的设计坐标
 *
 * 返回 { flat, total, k, near, tiles }，tiles[i] = { ...牌面信息, cx, cy, corners }。
 * corners 是含金边（side/thick）的外轮廓四角 —— 拿它做 SAT 判定是
 * 略保守的（金边侧多算几px），对「间距」只会更安全。
 */
export function meldLayout(L, melds, near) {
  const ax = Math.cos(L.angle);
  const ay = Math.sin(L.angle);
  const { flat, total, k } = flatten(L, melds, L.maxLen || Infinity);
  if (!flat.length) return null;

  // makeFlatTile 收到 world 系 z 旋转 α = −tilt；映回 y 向下的设计系：
  //   局部 x 轴 -> (cos tilt, sin tilt)，局部 y 轴 -> (sin tilt, −cos tilt)
  const tilt = L.tilt || 0;
  const cs = Math.cos(tilt), sn = Math.sin(tilt);
  const Xd = { x: cs, y: sn };
  const Yd = { x: sn, y: -cs };

  let along = 0;
  // 被鸣横放牌比同组邻牌「窄一截」的那条边：沿带方向排布时（θ≈±90°，
  // 横跨带子的是 local-x），横放牌的横跨 = L.h+side，邻牌 = L.w+side；
  // 本家/对家 θ≈0°，横跨的是 local-y，正好反过来。对齐量取半差。
  const acrossSwap = Math.abs(Math.sin(L.angle - (L.tilt || 0))) > .5;
  const dropBase = acrossSwap ? (L.w - L.h) / 2 : (L.h - L.w) / 2;
  const tiles = flat.map((item, i) => {
    const dist = along + item.extent / 2;
    // 被鸣横放的牌沿**带的垂直方向**挪半截，让无金边的那条长边和邻牌
    // 对齐（底边落在同一条桌面上）。注意偏移必须垂直于带：早期版本
    // 世界/设计 y 没翻转干净，侧移混进了 8~12px 的沿带滑动，
    // 既压进邻牌、又加重压手牌（两处截图 bug 的共同根源之一）。
    const drop = item.claimed && L.swap ? dropBase : 0;
    const cx = near.x + ax * dist - ay * drop;
    const cy = near.y + ay * dist + ax * drop;
    // 外轮廓：牌面矩形 + 沿局部 +x 的金边(side)、−y 的厚边(thick)
    const lx0 = -item.w / 2, lx1 = item.w / 2 + L.side * k;
    const ly0 = -item.h / 2 - L.thick * k, ly1 = item.h / 2;
    const corners = [
      [lx0, ly0], [lx1, ly0], [lx1, ly1], [lx0, ly1],
    ].map(([lx, ly]) => ({
      x: cx + lx * Xd.x + ly * Yd.x,
      y: cy + lx * Xd.y + ly * Yd.y,
    }));
    const tile = { ...item, cx, cy, corners };
    if (i < flat.length - 1) {
      along += item.extent + (flat[i + 1].group === item.group ? L.seam : L.gap) * k;
    }
    return tile;
  });

  return { flat, total, k, near, tiles };
}

/** 三家副露：由 far 与总长反推 near 再落位；没副露/本家返回 null。 */
export function seatMeldLayout(seat, state) {
  const L = MELD.seat[seat];
  if (!L || !L.far) return null;
  const melds = (state.melds || []).filter((meld) => meld.seat === seat);
  if (!melds.length) return null;
  const { total } = flatten(L, melds, L.maxLen || Infinity);
  const ax = Math.cos(L.angle), ay = Math.sin(L.angle);
  return meldLayout(L, melds, {
    x: L.far.x - ax * total,
    y: L.far.y - ay * total,
  });
}

/**
 * 这一家副露「靠手牌那一端」的设计坐标；没副露返回 null。
 * 三家手牌拿它当锚：手牌靠近副露的一端钉在这里，张数变化只影响远端，
 * 于是一手牌 + 副露在画面上永远是一条连续的带子（录像就是这么连着的）。
 */
export function meldNearPoint(seat, state) {
  const layout = seatMeldLayout(seat, state);
  return layout ? layout.near : null;
}
