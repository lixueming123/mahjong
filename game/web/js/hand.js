/** 本家手牌渲染 + 出牌飞行动画。 */
import * as THREE from 'three';
import { HAND, RIVER, P, Z } from './config.js';
import { handLayout, drawnIndex, normTile } from './state.js';
import { makeHandTile, flat, rectMesh } from './tiles.js';

export class HandView {
  constructor(scene) {
    this.group = new THREE.Group();
    this.flightGroup = new THREE.Group();
    scene.add(this.group, this.flightGroup);
    this.pickables = [];
    this.flights = [];
  }

  render(state) {
    this.group.clear();
    this.pickables = [];
    // 左端固定的槽位；刚摸到那张自带 33px 空档 + 47px 上抬
    const layout = handLayout(state.hand.length, drawnIndex(state));
    // 立直宣言模式：候选宣言牌高亮（点它即宣言并打出，§12.4）
    const candidates = new Set(
      (state.riichiArming ? state.riichiTiles || [] : []).map(normTile),
    );
    state.hand.forEach((code, i) => {
      const tile = makeHandTile(code, i);
      const [x, y] = P(layout[i].x, layout[i].y);
      const lifted = i === state.selected;
      tile.position.set(x, y + (lifted ? HAND.lift : 0), 0);
      if (lifted) {
        // 抽出的牌更大、带金色轮廓和轻微倾角；落影随组一起上移，悬浮感更明显
        tile.scale.setScalar(1.045);
        tile.rotation.z = -.018;
        const glow = rectMesh(HAND.w + 12, HAND.h + 12, flat(0xffd479, .55), Z.HAND_SHADOW);
        glow.position.z = -.01;
        tile.add(glow);
      } else if (candidates.has(normTile(code))) {
        // 候选宣言牌：牌面之上画一圈金框 + 底部金条（画在牌面之下会被不透明牌面盖住）
        const wash = rectMesh(HAND.w - 6, HAND.h - 10, flat(0xffd166, .16), Z.HAND + 2);
        tile.add(wash);
        const t = 5;
        const halfW = HAND.w / 2 - 4;
        const halfH = HAND.h / 2 - 4;
        [[HAND.w - 8, t, 0, halfH], [HAND.w - 8, t, 0, -halfH],
          [t, HAND.h - 8, -halfW, 0], [t, HAND.h - 8, halfW, 0]]
          .forEach(([w, h, x, y]) => {
            const edge = rectMesh(w, h, flat(0xffd166, .92), Z.HAND + 3);
            edge.position.set(x, y, 0);
            tile.add(edge);
          });
        const bar = rectMesh(HAND.w - 22, 8, flat(0xffc63d, .98), Z.HAND + 5);
        bar.position.set(0, -HAND.h / 2 + 15, 0);
        tile.add(bar);
      }
      // 只有牌面可拾取，阴影/高亮不参与 raycast
      const face = tile.children.find((c) => c.userData && c.userData.face);
      if (face) {
        face.userData.pick = { kind: 'hand', index: i };
        this.pickables.push(face);
      }
      this.group.add(tile);
    });
  }

  /** 从手牌位置抛物线飞到牌河；状态在 main 里已即时落地，动画只做视觉 */
  fly(code, from, to) {
    const tile = makeHandTile(code, -1);
    const s = RIVER.vH / HAND.h;
    tile.scale.setScalar(1);
    const [fx, fy] = P(from.x, from.y);
    const [tx, ty] = P(to.x, to.y);
    tile.position.set(fx, fy, 0);
    this.flightGroup.add(tile);
    this.flights.push({
      tile, start: performance.now(), dur: 380,
      from: new THREE.Vector2(fx, fy), to: new THREE.Vector2(tx, ty),
      s0: 1, s1: s,
    });
  }

  update(now) {
    if (!this.flights.length) return;
    for (const f of this.flights) {
      const k = Math.min(1, (now - f.start) / f.dur);
      const e = 1 - Math.pow(1 - k, 3);
      f.tile.position.x = f.from.x + (f.to.x - f.from.x) * e;
      f.tile.position.y = f.from.y + (f.to.y - f.from.y) * e
        + Math.sin(Math.PI * k) * 55;
      f.tile.scale.setScalar(f.s0 + (f.s1 - f.s0) * e);
      f.tile.rotation.z = (1 - e) * .12;
    }
    this.flights = this.flights.filter((f) => {
      if (now - f.start < f.dur) return true;
      this.flightGroup.remove(f.tile);
      return false;
    });
  }
}
