/**
 * 麻将牌工厂。
 *
 * 平躺牌（牌河 / 副露 / 宝牌指示）统一由 flat3d.js 生成真 3D 斜切盒：
 * 牌面、象牙白侧面、橙金下缘厚边都是同一个 Mesh 的真实面，吃光照、有棱边、
 * 会投影，不再用「Plane + 画出来的渐变矩形」拼贴。
 *
 * 录像规则：舍牌牌面按各家方向旋转，但橙金厚边永远朝屏幕下方。
 * 现在靠顶面 UV 旋转实现 —— 旋转的只有图案，牌身几何不转，
 * 所以横躺那张不会再出现「厚边跑到左边变成大黄条」。
 */
import * as THREE from 'three';
import { faceGeometry, getHandAtlas } from './assets.js';
import { RIVER, HAND, Z } from './config.js';
import { makeFlatTile } from './flat3d.js';

/** 舍牌角度：下家正立 / 右家顺时针 / 对家 180° / 左家逆时针（90° 步数） */
export const RIVER_ROT = { 0: 0, 1: 1, 2: 2, 3: 3 };

/** 2D 覆盖层（本家立牌 / 落影）用的无光照材质 */
const atlasMaterial = (texture = getHandAtlas()) => new THREE.MeshBasicMaterial({
  map: texture,
  transparent: true,
  depthTest: false,
  depthWrite: false,
  side: THREE.DoubleSide,
});

export const flat = (color, opacity = 1) => new THREE.MeshBasicMaterial({
  color, transparent: true, opacity, depthTest: false, depthWrite: false,
});

export function rectMesh(w, h, material, order) {
  const m = new THREE.Mesh(new THREE.PlaneGeometry(w, h), material);
  m.renderOrder = order;
  return m;
}

/**
 * 一张平躺舍牌 = 单个真 3D Mesh。
 * 占屏包围盒仍是 fw × (fh + thick)，与旧版一致，牌河坐标无需改动。
 */
export function makeRiverTile(code, seat) {
  const horizontal = seat === 1 || seat === 3;
  const tile = makeFlatTile({
    code,
    w: horizontal ? RIVER.hW : RIVER.vW,
    h: horizontal ? RIVER.hH : RIVER.vH,
    d: 18,
    thick: RIVER.thick,
    rot: RIVER_ROT[seat] || 0,
  });
  tile.renderOrder = Z.RIVER;
  return tile;
}

/**
 * 本家立牌：牌面正对屏幕、牌背在上缘露出一条橙金厚边。
 * 立体层次（橙金圆角牌顶 / 灰紫阴影带 / 白色圆角牌面 / 左右暗缝）
 * 已经预烘在 tile-atlas-hand.png 里；这里只补落影和触底暗边。
 */
export function makeHandTile(code, index) {
  const g = new THREE.Group();

  // 立牌落影：往右下偏，两层叠出软边，牌才不像贴在桌布上
  const sh1 = rectMesh(HAND.w - 4, HAND.h - 8, flat(0x040a14, .30), Z.HAND_SHADOW);
  sh1.position.set(HAND.shadowDx, HAND.shadowDy, 0);
  g.add(sh1);
  const sh2 = rectMesh(HAND.w - 14, HAND.h - 20, flat(0x040a14, .22), Z.HAND_SHADOW);
  sh2.position.set(HAND.shadowDx + 3, HAND.shadowDy - 3, 0);
  g.add(sh2);

  // 牌面比布局宽 seam：相邻牌各溢出 2px 轻微叠压，录像里手牌几乎无缝。
  // 只改渲染宽度、不改 handLayout 的坐标，整排 1:1 位置保持不动。
  const face = new THREE.Mesh(
    faceGeometry(code, HAND.w + HAND.seam, HAND.h), atlasMaterial(),
  );
  face.renderOrder = Z.HAND;
  face.userData.face = true;
  g.add(face);

  // 触底暗边：牌立在桌布上的接触阴影
  const foot = rectMesh(HAND.w - 16, HAND.foot, flat(0x16202e, .5), Z.HAND + 1);
  foot.position.set(0, -HAND.h / 2 + HAND.foot / 2 + 3, 0);
  g.add(foot);

  g.userData = { kind: 'hand', code, index, w: HAND.w, h: HAND.h };
  return g;
}

/**
 * 顶部那一排小号真 3D 牌。
 * 录像 f05/f07 实测：白牌身约 40 高、下缘橙金厚边约 15（占 1/3），
 * 宽约 39、间距约 40，整排贴着画面上沿。全部是翻开的明牌。
 */
export function makeMiniTile(code, w, h, order = Z.RIVER) {
  const tile = makeFlatTile({
    code, w, h,
    d: Math.max(9, w * .34),
    thick: Math.max(5, Math.round(h * .30)),
    rot: 0,
    shadow: false,          // 录像里这一排牌紧贴，加落影反而会切出深蓝缝
  });
  tile.renderOrder = order;
  return tile;
}
