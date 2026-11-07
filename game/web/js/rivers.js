/**
 * 四家牌河。
 *
 * 录像规则（上一版做错的地方）：
 *  - 上下家：竖牌，6 张一行横向排，3 行向外延伸
 *  - 左右家：横牌，6 张一列纵向排，3 列向外延伸
 *  - 牌面按各家方向旋转，但橙金厚边永远朝屏幕下方（在 tiles.js 处理）
 *  - 牌河只占中央一小块，不是铺满整张桌子
 */
import * as THREE from 'three';
import { RIVER, BOARD, P, Z } from './config.js';
import { makeRiverTile } from './tiles.js';

/** 第 n 张舍牌在 seat 家牌河里的设计像素位置 */
export function riverSlot(seat, n) {
  const col = n % RIVER.cols;   // 沿「自己手牌方向」的第几张
  const row = Math.floor(n / RIVER.cols);
  const c = (RIVER.cols - 1) / 2;

  if (seat === 0) { // 本家（下）
    return {
      x: BOARD.x + (col - c) * RIVER.colGap,
      y: RIVER.firstRowBottom + row * RIVER.rowGap,
    };
  }
  if (seat === 2) { // 对家（上）
    return {
      x: BOARD.x + (col - c) * RIVER.colGap,
      y: RIVER.firstRowTop - row * RIVER.rowGap,
    };
  }
  // 左右家：6 张沿纵向、3 排沿横向
  const outward = seat === 3 ? -1 : 1;
  const baseX = seat === 3 ? RIVER.firstColLeft : RIVER.firstColRight;
  return {
    x: baseX + outward * row * (RIVER.hW + 2),
    y: RIVER.sideRowY0 + (col - 1) * (RIVER.hH + 2),
  };
}

export class RiverView {
  constructor(scene) {
    this.group = new THREE.Group();
    scene.add(this.group);
    this.meshes = [[], [], [], []];
  }

  clear() {
    this.group.clear();
    this.meshes = [[], [], [], []];
  }

  render(state) {
    this.clear();
    for (let seat = 0; seat < 4; seat++) {
      const cards = state.rivers[seat];
      for (let n = 0; n < cards.length; n++) {
        const p = riverSlot(seat, n);
        const tile = makeRiverTile(cards[n], seat);
        const [wx, wy] = P(p.x, p.y);
        // 真 3D 牌：略微离桌，shadowMap 才投得出落影；
        // 后打出的牌 z 更高，深度测试自然决定前后，不再靠贴图层叠。
        tile.position.set(wx, wy, 4 + n * .12);
        tile.renderOrder = Z.RIVER + n * .001;
        this.group.add(tile);
        this.meshes[seat].push(tile);
      }
    }
  }
}
