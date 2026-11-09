// 布局碰撞自检：副露带（最多 4 组，含被鸣横放）vs 牌河 18 张 vs 他家区域
import { MELD, RIVER, SEAT_LAYOUT, HAND, DESIGN } from '../../web/js/config.js';
import { MELD_TYPE } from '../../web/js/protocol_enums.js';
import { OpponentHandsView } from '../../web/js/opponents.js';
import { TablePass } from '../../web/js/tablepass.js';
import { riverPlacement } from '../../web/js/riverLayout.js';
import { seatMeldLayout } from '../../web/js/meldLayout.js';

// 牌河走透视通道：用真实相机把每张牌的 8 个角投影回设计坐标（1920x1080 窗口）
const view = new OpponentHandsView();
view.resize(DESIGN.W / DESIGN.H, DESIGN.W, DESIGN.H);
const pass = new TablePass(view);

// 牌河牌的屏幕包围盒（含朝镜头的前侧面；横躺牌宽高互换）
function riverBox(seat, n) {
  const p = riverPlacement(pass, seat, n);
  const hx = (p.horizontal ? RIVER.h : RIVER.w) * p.sx / 2;
  const hy = (p.horizontal ? RIVER.w : RIVER.h) * p.sx / 2;
  const hz = RIVER.d * p.sx / 2;
  const box = { seat, kind: 'river', x0: Infinity, x1: -Infinity, y0: Infinity, y1: -Infinity };
  for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
    const q = pass.toDesign(p.x + sx * hx, p.y + sy * hy, p.z + sz * hz);
    box.x0 = Math.min(box.x0, q.x); box.x1 = Math.max(box.x1, q.x);
    box.y0 = Math.min(box.y0, q.y); box.y1 = Math.max(box.y1, q.y);
  }
  box.cx = (box.x0 + box.x1) / 2;
  box.cy = (box.y0 + box.y1) / 2;
  return box;
}

// 副露带：每张牌的屏幕包围盒，取 meldLayout 的外轮廓角点（与钉牌解算同一份估算）
function meldBoxes(seat, groups) {
  const melds = Array.from({ length: groups }, () =>
    ({ seat, tiles: ['x', 'x', 'x'], type: MELD_TYPE.PONG, rotated: 0 }));   // 每组 3 张、第 1 张被鸣(横放)
  const layout = seatMeldLayout(seat, { melds });
  if (!layout) return [];   // 本家另有 ownNear 逻辑，单独验
  return layout.tiles.map((t) => {
    const xs = t.corners.map((c) => c.x);
    const ys = t.corners.map((c) => c.y);
    return { seat, kind: `meld#g${t.group}${t.claimed ? '(横)' : ''}`,
      x0: Math.min(...xs), x1: Math.max(...xs), y0: Math.min(...ys), y1: Math.max(...ys),
      cx: t.cx, cy: t.cy };
  });
}

function hit(a, b) {
  return a.x0 < b.x1 - 2 && b.x0 < a.x1 - 2 && a.y0 < b.y1 - 2 && b.y0 < a.y1 - 2;
}

const bad = [];
// 每家：0~4 组副露 × 满 18 张牌河
for (const seat of [1, 2, 3]) {
  for (let g = 1; g <= 4; g++) {
    const melds = meldBoxes(seat, g);
    // 副露 vs 四家牌河（含自家）
    for (const other of [0, 1, 2, 3]) {
      for (let n = 0; n < RIVER.cap; n++) {
        const box = riverBox(other, n);
        for (const m of melds) if (hit(box, m)) bad.push(`seat${seat} ${g}组副露(${m.kind}) × seat${other}牌河第${n}张 @(${m.cx.toFixed(0)},${m.cy.toFixed(0)})`);
      }
    }
  }
}
// 四家牌河两两不相交、也不压情报板（板角风位徽章外凸约 22px）
{
  const board = { x0: 800 - 22, x1: 1120 + 22, y0: 298, y1: 538 };
  const boxes = [0, 1, 2, 3].flatMap((seat) => Array.from({ length: RIVER.cap }, (_, n) => ({ ...riverBox(seat, n), n })));
  for (const a of boxes) {
    if (hit(a, board)) bad.push(`seat${a.seat}牌河第${a.n}张 × 情报板`);
    for (const b of boxes) {
      if (a.seat < b.seat && hit(a, b)) bad.push(`seat${a.seat}牌河第${a.n}张 × seat${b.seat}牌河第${b.n}张`);
    }
  }
}
// 对家副露 vs 左上角场况卡（宝牌指示在卡片里，x≈20~330）与对家手牌背：
// 对家手牌右端钉死（pin: false），张数 13-3g，从右往左排
for (let g = 1; g <= 4; g++) {
  const L2 = SEAT_LAYOUT[2];
  const count = 13 - 3 * g + 1;   // 摸牌待打最长
  const right = L2.cx + 6 * L2.step + L2.w / 2;
  const back = { x0: right - (count - 1) * L2.step - L2.w, x1: right, y0: 10, y1: 80 };
  const card = { x0: 20, x1: 330, y0: 20, y1: 200 };
  for (const m of meldBoxes(2, g)) {
    if (hit(card, m)) bad.push(`对家副露 × 场况卡 @(${m.cx.toFixed(0)},${m.cy.toFixed(0)})`);
    if (hit(back, m)) bad.push(`对家${g}组副露 × 对家手牌背 @(${m.cx.toFixed(0)},${m.cy.toFixed(0)})`);
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

