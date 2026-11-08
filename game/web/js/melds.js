/**
 * 副露视图：吃/碰/杠之后摊开亮给另外三家看的牌。
 *
 * 录像口径（这一版重做的原因，见 config.js 的 MELD 注释）：
 *  - 副露是**平铺在桌面上的牌**，不是缩小版的牌河牌：牌面尺寸跟这一家的
 *    手牌同一档（本家 94x150，左右家 86x64 横躺，对家 48x62），
 *    所以它一眼看去是「手牌旁边挪出来的一组大牌」，而不是角落里的碎牌；
 *  - 组内相邻牌**轻微互压**（录像实测 pitch 62 < 牌高 66），连成一条斜带；
 *  - 被鸣那张排在**最靠手牌的一端**，横放（宽高互换）；
 *  - 整条带子沿该家手牌的同一条斜线往桌角方向延长，并按 tilt 微倾。
 *
 * 坐标语义：MELD.seat[n].far 是这一家副露区的**远端**（离手牌最远的那条边），
 * 新的一组长在手牌那一侧；本家没有 far —— 它贴着会伸缩的手牌右端往外排。
 *
 * 落位数学（flatten / tileExtent / 被鸣牌 drop 位移 / tilt 朝向 / 多边形角点）
 * 全部在 meldLayout.js —— 渲染、opponents 钉牌解算、geo 回归三方共用。
 *
 * 数据来自 state.melds，渲染层只读。
 */
import * as THREE from 'three';
import { MELD, HAND, P, Z } from './config.js';
import { makeFlatTile } from './flat3d.js';
import { drawnIndex } from './state.js';
import { flatten, meldLayout } from './meldLayout.js';

export { meldNearPoint } from './meldLayout.js';

const QUARTER = 4;

export class MeldView {
  constructor(scene) {
    this.group = new THREE.Group();
    scene.add(this.group);
  }

  render(state) {
    this.group.clear();
    const bySeat = new Map();
    for (const meld of state.melds || []) {
      if (!bySeat.has(meld.seat)) bySeat.set(meld.seat, []);
      bySeat.get(meld.seat).push(meld);
    }
    for (const [seat, melds] of bySeat) {
      const layout = MELD.seat[seat];
      if (!layout) continue;
      this.renderSeat(state, seat, layout, melds);
    }
  }

  renderSeat(state, seat, L, melds) {
    const maxLen = L.far ? (L.maxLen || Infinity) : ownMaxLen(state);
    const { flat, total, k } = flatten(L, melds, maxLen);
    if (!flat.length) return;

    // 起点（靠手牌那一端）：三家从远端倒推，本家贴着会伸缩的手牌右端
    const ax = Math.cos(L.angle);
    const ay = Math.sin(L.angle);
    const near = L.far
      ? { x: L.far.x - ax * total, y: L.far.y - ay * total }
      : ownNear(state, total);

    // 落位（含被鸣横放的 drop 位移与 tilt 朝向）——与钉牌解算/回归共用一份
    const layout = meldLayout(L, melds, near);
    layout.tiles.forEach((item, i) => {
      // 副露一律是「躺下但有厚度」的真 3D 牌：牌面用手牌同一套白牌身，
      // 厚度/斜棱/橙金下缘由 flat3d 的斜切盒给出，投影走 shadowMap。
      const tile = makeFlatTile({
        code: item.code,
        w: item.w,
        h: item.h,
        d: L.d * k,
        thick: L.thick * k,
        // 右侧那条橙金牌背窄边：牌立着时从斜前方能看到，是副露
        // 一眼「有厚度」的来源
        side: L.side * k,
        // 被鸣那张横放：宽高互换（flatten 已做），图案再转 90°
        rot: item.claimed ? (L.rot + 1) % QUARTER : L.rot,
        // 牌面相对屏幕的倾角：让牌的短边正好垂直于排布方向，一串牌才码得齐。
        // 屏幕 y 向下、Three 的 z 旋转是 y 向上的角度，所以喂给几何时要取负。
        angle: -(L.tilt || 0),
      });
      // 平铺在桌面上；后面的组/牌略微抬高，深度测试自然分出前后
      // 横放的牌短边朝上：沿「垂直于排布方向」比邻居宽出一截。
      // 中心不动会两侧各凸一半，看着像摆歪了；统一把多出来的部分
      // 全部推到同一侧，让这一侧和同组竖牌对齐（底边落在同一条桌面上）。
      // （drop 位移已折进 layout 的 cx/cy）
      const [wx, wy] = P(item.cx, item.cy);
      tile.position.set(wx, wy, 6 + item.group * 2 + i * .6);
      tile.renderOrder = Z.RIVER + 2 + item.group * .1 + i * .01;
      this.group.add(tile);
    });
  }
}

/**
 * 本家副露起点：**右端钉死在牌山左边**，整块从右往左排。
 *
 * 以前是从手牌右端往右长，组数一多就顶到牌山（橙金牌墙）上；现在按总量反推左端，
 * 既不会压手牌，也不会冲出桌面。张数变化只影响这块的左端，右端一动不动。
 */
function ownMinLeft(state) {
  const hand = state.hand || [];
  const drawn = drawnIndex(state);
  const count = drawn >= 0 ? hand.length - 1 : hand.length;   // 打完后的张数
  const pitch = HAND.w + HAND.gap;
  const handRight = count
    ? HAND.left + (count - 1) * pitch + HAND.w
    : HAND.left + HAND.w;
  // 摸牌槽常驻保留：摸牌/打牌时副露不跟着左右跳
  return handRight + HAND.drawGap + HAND.w + 10;
}

function ownNear(state, total = 0) {
  const rightEdge = MELD.seat[0].right ?? 1700;
  return { x: Math.max(ownMinLeft(state), rightEdge - total), y: MELD.seat[0].y };
}

/** 本家副露带能用的长度：手牌右沿（含摸牌槽）到牌山左边 */
function ownMaxLen(state) {
  const rightEdge = MELD.seat[0].right ?? 1700;
  return Math.max(180, rightEdge - ownMinLeft(state));
}
