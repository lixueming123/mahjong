/**
 * 副露视图：吃/碰/杠之后摊开亮给另外三家看的牌。
 *
 * 录像口径（这一版重做的原因，见 config.js 的 MELD 注释）：
 *  - 副露是**平铺在桌面上的牌**，不是缩小版的牌河牌：牌面尺寸跟这一家的
 *    手牌同一档，所以它一眼看去是「手牌旁边挪出来的一组大牌」；
 *  - 三家的牌沿桌边贴紧排成一条带子，透视给出那条「斜线」（renderSide）；
 *  - 被鸣那张横放（宽高互换），贴齐靠这一家本人的那条边。
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
  /** @param {import('./tablepass.js').TablePass} pass 透视通道（牌平躺在桌面上） */
  constructor(pass, scene) {
    this.pass = pass;
    this.group = new THREE.Group();
    pass.scene.add(this.group);
    // 本家副露牌大（94x150）又贴着画面下缘，透视会把它拉成明显的梯形，
    // 所以留在正交主桌里：1:1 不变形，厚度仍由斜切盒给出。
    this.ownGroup = new THREE.Group();
    scene.add(this.ownGroup);
  }

  render(state) {
    this.pass.forget(this.group);
    this.ownGroup.clear();
    const bySeat = new Map();
    for (const meld of state.melds || []) {
      if (!bySeat.has(meld.seat)) bySeat.set(meld.seat, []);
      bySeat.get(meld.seat).push(meld);
    }
    for (const [seat, melds] of bySeat) {
      const layout = MELD.seat[seat];
      if (!layout) continue;
      if (seat === 0) {
        this.renderOwn(state, layout, melds);
      } else {
        this.renderSide(layout, melds);
      }
    }
  }

  /** 本家：正交主桌里的斜切盒牌（透视会把贴着画面下缘的大牌拉成梯形） */
  renderOwn(state, L, melds) {
    const { flat, total, k } = flatten(L, melds, ownMaxLen(state));
    if (!flat.length) return;
    // 落位（含被鸣横放的 drop 位移）——与回归共用一份
    const layout = meldLayout(L, melds, ownNear(state, total));
    // 和手牌同一套画法（立牌图集 + 牌顶露出象牙白顶面与橙色牌背），
    // 否则摆在手牌旁边像两套牌。外轮廓不变：meldLayout 预留的右侧 side 并进牌宽，
    // 下缘 thick 挪到牌顶，整块往下移 thick 补回原位。
    const side = L.side * k;
    const thick = L.thick * k;
    layout.tiles.forEach((item, i) => {
      const tile = makeFlatTile({
        code: item.code,
        w: item.w + side,
        h: item.h,
        d: L.d * k,
        thick: -thick,
        // 被鸣那张横放：宽高互换（flatten 已做），图案再转 90°
        rot: item.claimed ? (L.rot + 1) % QUARTER : L.rot,
        atlas: 'hand',
      });
      tile.renderOrder = Z.RIVER + 2 + item.group * .1 + i * .01;
      const [wx, wy] = P(item.cx, item.cy);
      tile.position.set(wx + side / 2, wy - thick, 6 + item.group * 2 + i * .6);
      this.ownGroup.add(tile);
    });
  }

  /**
   * 三家：在透视通道里**沿桌边**平铺一条带子（录像：上家副露是一摞横牌，
   * 每张都不歪，只是顺着桌边的透视线往左下走）。
   *
   * 起点仍是设计坐标里的 near（手牌钉点解算依赖它），但之后完全在桌面世界里排：
   * 方向吸附到桌边轴向、所有牌同一个缩放 —— 于是牌与牌严丝合缝，
   * 近大远小和收敛交给真透视，不再叠加旧的屏幕 tilt。
   */
  renderSide(L, melds) {
    const { flat, total, k } = flatten(L, melds, L.maxLen || Infinity);
    if (!flat.length) return;
    const ax = Math.cos(L.angle);
    const ay = Math.sin(L.angle);
    const near = { x: L.far.x - ax * total, y: L.far.y - ay * total };

    // 设计方向吸附到最近的桌边轴，换到世界系（y 向上）
    const q = ((Math.round(L.angle / (Math.PI / 2)) % QUARTER) + QUARTER) % QUARTER;
    const u = { x: Math.round(Math.cos(q * Math.PI / 2)), y: -Math.round(Math.sin(q * Math.PI / 2)) };
    const toPlayer = { x: u.y, y: -u.x };
    const alongY = q % 2 === 1;
    const normalAcross = (alongY ? L.w : L.h) * k;

    // 带内坐标（世界尺寸，单位同 config）：along 沿带、across 朝这一家本人
    let along = 0;
    const slots = flat.map((item, i) => {
      const extent = alongY ? item.worldH : item.worldW;
      const across = alongY ? item.worldW : item.worldH;
      // 被鸣横放牌横跨更窄：贴齐靠本人的那条边（和本家副露同一规矩）
      const slot = { item, along: along + extent / 2, across: (normalAcross - across) / 2 };
      if (i < flat.length - 1) {
        along += extent + (flat[i + 1].group === item.group ? L.seam : L.gap) * k;
      }
      return slot;
    });
    const mid = { x: near.x + ax * total / 2, y: near.y + ay * total / 2 };
    const depth = L.d * k;
    // near 是带子在屏幕上靠手牌的那条边：带子往镜头方向（屏幕下方）长时，
    // 那条边是顶面棱；往远处长时是贴桌的底棱。锚在对应高度，牌才不会越过 near。
    const towardCamera = u.y < 0;

    slots.forEach(({ item, along: s, across: t }, i) => {
      const tile = makeFlatTile({
        code: item.code,
        w: item.worldW,
        h: item.worldH,
        d: depth,
        persp: true,
        rot: item.claimed ? (L.rot + 1) % QUARTER : L.rot,
        atlas: 'hand',
      });
      tile.renderOrder = Z.RIVER + 2 + item.group * .1 + i * .01;
      this.pass.placeWorld(tile, this.group, () => {
        // 整条带子共用带中点的世界比例（同牌河，各向同性）：牌按真实比例平躺，
        // 张与张之间没有比例差，纵向压缩交给透视
        const { sx } = this.pass.scaleAt(mid.x, mid.y);
        const base = this.pass.opponents.unprojectToTable(near.x, near.y,
          towardCamera ? depth * sx : 0);
        return {
          x: base.x + (u.x * s + toPlayer.x * t) * sx,
          y: base.y + (u.y * s + toPlayer.y * t) * sx,
          z: depth * sx / 2,
          sx,
          sy: sx,
        };
      });
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
