/**
 * 四家牌河。
 *
 * 录像规则：
 *  - 上下家：竖牌，6 张一行横向排，3 行向外延伸
 *  - 左右家：横牌，6 张一列纵向排，3 列向外延伸
 *  - 牌面按各家方向旋转，但橙金厚边永远朝屏幕下方（在 tiles.js 处理）
 *  - 牌河只占中央一小块，不是铺满整张桌子
 *
 * 落位在 riverLayout.js（桌面世界坐标、整桌同一尺寸，由透视给出近大远小）。
 */
import * as THREE from 'three';
import { RIVER, Z } from './config.js';
import { makeRiverTile, rectMesh, flat } from './tiles.js';
import { riverPlacement, riverSlot } from './riverLayout.js';

export class RiverView {
  /** @param {import('./tablepass.js').TablePass} pass 透视通道（牌平躺在桌面上） */
  constructor(pass) {
    this.pass = pass;
    this.group = new THREE.Group();
    pass.scene.add(this.group);
    this.meshes = [[], [], [], []];
  }

  /** 第 n 张舍牌在设计坐标里的位置（出牌飞行落点） */
  slot(seat, n) {
    return riverSlot(this.pass, seat, n);
  }

  clear() {
    this.pass.forget(this.group);
    this.meshes = [[], [], [], []];
  }

  render(state) {
    this.clear();
    const target = state.claimTarget;   // 鸣牌窗口：牌河里被指的那张（打出者随局轮转）
    for (let seat = 0; seat < 4; seat++) {
      const cards = state.rivers[seat];
      for (let n = 0; n < cards.length; n++) {
        const tile = makeRiverTile(cards[n], seat);
        const isTarget = !!target && target.seat === seat && n === target.index;
        // 被指的那张抬高一些，和金光一起把「就是这张」说清楚。
        // 真透视 + 深度测试：前后遮挡由几何决定，不再靠 z 微调。
        tile.renderOrder = Z.RIVER + n * .001;
        const lift = isTarget ? 8 : 0;
        this.pass.placeWorld(tile, this.group, () => riverPlacement(this.pass, seat, n, lift));
        this.meshes[seat].push(tile);
        if (isTarget) {
          // 鸣牌标记：牌脚下垫一块金光板。左右家的牌是横躺的，光板跟着换向。
          const horizontal = seat === 1 || seat === 3;
          const gw = (horizontal ? RIVER.h : RIVER.w) + 18;
          const gh = (horizontal ? RIVER.w : RIVER.h) + 12;
          const glow = rectMesh(gw, gh, flat(0xffc94d, .60), Z.RIVER - 1);
          this.pass.placeWorld(glow, this.group, () => {
            const p = riverPlacement(this.pass, seat, n);
            return { ...p, z: p.sx };
          });
        }
      }
    }
  }
}
