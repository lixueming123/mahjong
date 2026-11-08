// 布局碰撞自检：副露带（最多 4 组，含被鸣横放）vs 牌河 18 张 vs 他家区域
import { MELD, RIVER, SEAT_LAYOUT, HAND } from '../../web/js/config.js';
import { MELD_TYPE } from '../../web/js/protocol_enums.js';

// —— rivers.js 的 riverSlot 原样拷贝 ——
function riverSlot(seat, n) {
  const col = n % RIVER.cols, row = Math.floor(n / RIVER.cols);
  const c = (RIVER.cols - 1) / 2;
  if (seat === 0) return { x: 960 + (col - c) * RIVER.colGap, y: RIVER.firstRowBottom + row * RIVER.rowGap };
  if (seat === 2) return { x: 960 + (col - c) * RIVER.colGap, y: RIVER.firstRowTop - row * RIVER.rowGap };
  const outward = seat === 3 ? -1 : 1;
  const baseX = seat === 3 ? RIVER.firstColLeft : RIVER.firstColRight;
  return { x: baseX + outward * row * (RIVER.hW + 2), y: RIVER.sideRowY0 + (col - 1) * (RIVER.hH + 2) };
}

// 牌河牌的屏幕包围盒（含下方 thick 金边；横躺牌宽高互换）
function riverBox(seat, n) {
  const p = riverSlot(seat, n);
  const horiz = seat === 1 || seat === 3;
  const w = (horiz ? RIVER.hW : RIVER.vW), h = (horiz ? RIVER.hH : RIVER.vH) + RIVER.thick;
  return { seat, kind: 'river', x0: p.x - w / 2, x1: p.x + w / 2, y0: p.y - (h - RIVER.thick) / 2 - RIVER.thick / 2 + RIVER.thick / 2, y1: 0, w, h, cx: p.x, cy: p.y };
}

// 副露带：每张牌是一个盒，沿 angle 从 near 排到 far（复刻 flatten/renderSeat 几何）
function meldBoxes(seat, groups) {
  const L = MELD.seat[seat];
  if (!L.far) return [];   // 本家另有 ownNear 逻辑，单独验
  const ax = Math.cos(L.angle), ay = Math.sin(L.angle);
  const melds = Array.from({ length: groups }, (_, gi) =>
    ({ tiles: ['x', 'x', 'x'], type: MELD_TYPE.PONG, rotated: 0 }));   // 每组 3 张、第 1 张被鸣(横放)
  const items = [];
  melds.forEach((m, gi) => {
    m.tiles.forEach((_, i) => items.push({ claimed: i === 0, group: gi }));
  });
  let total = 0;
  const sized = items.map((it) => {
    const swap = it.claimed && L.swap;
    return { ...it, w: (swap ? L.h : L.w), h: (swap ? L.w : L.h) };
  });
  sized.forEach((it, i) => {
    total += Math.max(it.w, it.h) > 0 ? it.w : it.w;  // extent ≈ w（躺牌沿排布方向）
    if (i < sized.length - 1) total += sized[i + 1].group === it.group ? L.seam : L.gap;
  });
  // seam/gap 可能为负，修正总长
  total = 0;
  sized.forEach((it, i) => {
    total += it.w;
    if (i < sized.length - 1) total += (sized[i + 1].group === it.group ? L.seam : L.gap);
  });
  const k = Math.min(1, (L.maxLen || Infinity) / Math.max(total, 1));
  const near = { x: L.far.x - ax * total * k, y: L.far.y - ay * total * k };
  let along = 0;
  const boxes = [];
  sized.forEach((it, i) => {
    const w = it.w * k, h = it.h * k;
    const dist = along + w / 2;
    let x = near.x + ax * dist, y = near.y + ay * dist;
    const sideways = it.claimed && L.swap;
    const drop = sideways ? (L.h - L.w) / 2 * k : 0;
    x -= ay * drop; y += ax * drop;
    // 盒子：沿排布方向 w，垂直方向 h（再加 thick 金边一律往屏幕下方）
    boxes.push({ seat, kind: `meld#g${it.group}${it.claimed ? '(横)' : ''}`,
      x0: x - Math.abs(ax) * w / 2 - Math.abs(ay) * h / 2,
      x1: x + Math.abs(ax) * w / 2 + Math.abs(ay) * h / 2,
      y0: y - Math.abs(ay) * w / 2 - Math.abs(ax) * h / 2 - RIVER.thick * 0,
      y1: y + Math.abs(ay) * w / 2 + Math.abs(ax) * h / 2 + L.thick * k,
      cx: x, cy: y });
    if (i < sized.length - 1) along += w + (sized[i + 1].group === it.group ? L.seam : L.gap) * k;
  });
  return boxes;
}

function hit(a, b) {
  return a.x0 < b.x1 - 2 && b.x0 < a.x1 - 2 && a.y0 < b.y1 - 2 && b.y0 < a.y1 - 2;
}

const bad = [];
// 每家：0~4 组副露 × 满 18 张牌河
for (const seat of [1, 2, 3]) {
  for (let g = 1; g <= 4; g++) {
    const melds = meldBoxes(seat, g);
    for (let n = 0; n < RIVER.cap; n++) {
      const rb = riverBox(seat, n);
      const box = { x0: rb.cx - rb.w / 2, x1: rb.cx + rb.w / 2, y0: rb.cy - (rb.h - RIVER.thick) / 2 - RIVER.thick / 2 + RIVER.thick / 2, y1: rb.cy + (rb.h + RIVER.thick) / 2 };
      for (const m of melds) if (hit(box, m)) bad.push(`seat${seat} ${g}组副露(${m.kind}) × 牌河第${n}张 @(${m.cx.toFixed(0)},${m.cy.toFixed(0)})`);
    }
    // 副露 vs 别家牌河
    for (const other of [1, 2, 3]) {
      if (other === seat) continue;
      for (let n = 0; n < RIVER.cap; n++) {
        const p = riverSlot(other, n);
        const horiz = other === 1 || other === 3;
        const w = (horiz ? RIVER.hW : RIVER.vW), h = (horiz ? RIVER.hH : RIVER.vH) + RIVER.thick;
        const box = { x0: p.x - w / 2, x1: p.x + w / 2, y0: p.y - h / 2, y1: p.y + h / 2 };
        for (const m of meldBoxes(seat, g)) if (hit(box, m)) bad.push(`seat${seat} ${g}组副露 × seat${other}牌河第${n}张`);
      }
    }
  }
}
// 对家副露 vs 宝牌指示排（y≈55, x 385~625）与对家手牌背（y≈21~65）
for (let g = 1; g <= 4; g++) {
  for (const m of meldBoxes(2, g)) {
    const dora = { x0: 365, x1: 645, y0: 25, y1: 85 };
    const back = { x0: 560, x1: 1660, y0: 10, y1: 80 };
    if (hit(dora, m)) bad.push(`对家副露 × 宝牌排 @(${m.cx.toFixed(0)},${m.cy.toFixed(0)})`);
    if (hit(back, m)) bad.push(`对家副露 × 对家手牌背 @(${m.cx.toFixed(0)},${m.cy.toFixed(0)})`);
  }
}
// 本家副露 vs 本家手牌：副露 g 组时打完手牌剩 13-3g 张（+摸牌槽），
// ownNear 取 max(手牌右沿+摸牌槽, 1760-total)，两种情形分别验
{
  const L = MELD.seat[0];
  const pitch = HAND.w + HAND.gap;
  for (let g = 1; g <= 4; g++) {
    const afterDiscard = 13 - 3 * g;
    const handRight = HAND.left + (afterDiscard - 1) * pitch + HAND.w;
    const minLeft = handRight + HAND.drawGap + HAND.w + 10;
    // 一组 3 张（首张横放 w=h=150），组间 gap+seam
    let total = 0;
    for (let gi = 0; gi < g; gi++) {
      total += L.h + 2 * L.w;                     // 被鸣横放(150) + 两张(94)
      if (gi < g - 1) total += L.gap + L.seam;
    }
    const k = Math.min(1, Math.max(180, 1760 - minLeft) / total);
    const left = Math.max(minLeft, 1760 - total * k);
    const right = left + total * k;
    if (left < handRight + HAND.drawGap - 2) bad.push(`本家 ${g}组副露左沿(${left.toFixed(0)}) 压手牌右沿(${handRight.toFixed(0)})`);
    if (right > 1760 + 2) bad.push(`本家 ${g}组副露右沿(${right.toFixed(0)}) 冲出牌山边界(1760)`);
  }
}
console.log(bad.length ? bad.join('\n') : 'GEO PASS：副露/牌河/宝牌排/手牌背 无碰撞');

