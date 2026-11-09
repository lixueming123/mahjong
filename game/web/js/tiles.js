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
import { RIVER, HAND, Z } from './config.js';
import { makeFlatTile } from './flat3d.js';

/** 舍牌角度：下家正立 / 右家顺时针 / 对家 180° / 左家逆时针（90° 步数） */
export const RIVER_ROT = { 0: 0, 1: 1, 2: 2, 3: 3 };

/** 2D 覆盖层（落影 / 高亮）用的无光照材质 */
export const flat = (color, opacity = 1) => new THREE.MeshBasicMaterial({
  color, transparent: true, opacity, depthTest: false, depthWrite: false,
});

export function rectMesh(w, h, material, order) {
  const m = new THREE.Mesh(new THREE.PlaneGeometry(w, h), material);
  m.renderOrder = order;
  return m;
}

/**
 * 一张平躺舍牌 = 单个真 3D Mesh（透视通道渲染，局部单位 = 桌面世界尺寸）。
 */
export function makeRiverTile(code, seat) {
  const horizontal = seat === 1 || seat === 3;
  const tile = makeFlatTile({
    code,
    w: horizontal ? RIVER.h : RIVER.w,
    h: horizontal ? RIVER.w : RIVER.h,
    d: RIVER.d,
    persp: true,
    rot: RIVER_ROT[seat] || 0,
  });
  tile.renderOrder = Z.RIVER;
  return tile;
}

/**
 * 本家立牌：和躺牌同一套真 3D 圆角倒棱牌身，正面朝镜头。
 * 牌背一侧的顶点往 +y 斜切 HAND.top：1:1 正交相机下也能看到牌的上表面，
 * 侧面色阶给出「象牙白顶面 + 一线橙色牌背」，吃光照、有棱边。
 * 以前是一张画好的平面贴图（橙条 + 白面），那就是纸片感的来源。
 * 占屏包围盒仍是 HAND.w × HAND.h、中心不变，handLayout 坐标不用动。
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

  const body = makeFlatTile({
    code,
    w: HAND.body,
    h: HAND.h - HAND.top,
    d: HAND.d,
    thick: -HAND.top,
    atlas: 'hand',
    shadow: false,
  });
  body.position.y = -HAND.top / 2;
  body.renderOrder = Z.HAND;
  body.userData.face = true;
  g.add(body);

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
