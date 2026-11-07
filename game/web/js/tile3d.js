/**
 * 真 3D 麻将牌：有厚度的 BoxGeometry + 吃光的 MeshStandardMaterial。
 *
 * 与 tiles.js（2D 贴图合成）的本质区别：
 *  - 白色侧棱是牌身真实的 +y 侧面，不是 Canvas 里画的一条渐变；
 *  - 明暗来自 DirectionalLight 与法线夹角，不是预烘的线性渐变；
 *  - 落影由 shadowMap 投到台布 / 邻牌上，不是贴图里的一块半透明矩形；
 *  - 牌与牌之间有真实深度、自遮挡，depthTest/depthWrite 全开。
 *
 * 牌由 opponents.js 的透视相机渲染并真正竖立，所以牌背、顶面、
 * 端面会随视角自然浮现，没有任何需要「画出来」的立体部分。
 */
import * as THREE from 'three';
import { TILE_3D, ALL_CODES } from './config.js';
import { getAtlas } from './assets.js';

const INDEX = new Map(ALL_CODES.map((c, i) => [c, i]));
const ATLAS_COLS = 8, ATLAS_ROWS = 5;

let backTex = null;
/** 纯橙金牌背：只有颜色，没有任何假立体（无白棱、无落影、无高光条） */
function backTexture() {
  if (backTex) return backTex;
  const c = document.createElement('canvas');
  c.width = 128; c.height = 192;
  const g = c.getContext('2d');
  // 平涂底色 + 印刷纹样。明暗交给灯光，不在这里画渐变/高光/落影去伪造立体。
  g.fillStyle = TILE_3D.backMid; g.fillRect(0, 0, c.width, c.height);
  g.save();
  g.beginPath(); g.rect(0, 0, c.width, c.height); g.clip();
  g.strokeStyle = 'rgba(110,54,4,.11)'; g.lineWidth = 3;
  for (let d = -c.height; d < c.width; d += 13) {
    g.beginPath(); g.moveTo(d, 0); g.lineTo(d + c.height, c.height); g.stroke();
  }
  g.restore();
  // 每节之间的深灰分隔线：录像里靠这条线把紧挨的 13 张牌分开，
  // 牌本身不能靠偏角留缝（那样牌会散开，反而更不像录像）。
  g.strokeStyle = 'rgba(48,32,14,.88)'; g.lineWidth = 9;
  g.strokeRect(4, 4, c.width - 8, c.height - 8);
  g.strokeStyle = 'rgba(255,232,168,.42)'; g.lineWidth = 2;
  g.strokeRect(9, 9, c.width - 18, c.height - 18);
  g.fillStyle = 'rgba(255,232,168,.28)';
  g.fillRect(c.width / 2 - 13, c.height / 2 - 2, 26, 4);
  backTex = new THREE.CanvasTexture(c);
  backTex.colorSpace = THREE.SRGBColorSpace;
  return backTex;
}

const cache = new Map();
function materials(faceUp, code) {
  // 牌面朝上（明牌）时 +z 面要采样图集，按 code 缓存；牌背共用一套
  const key = faceUp ? `f:${code}` : 'b';
  if (cache.has(key)) return cache.get(key);

  const std = (opts) => new THREE.MeshStandardMaterial({ roughness: .78, metalness: .02, ...opts });
  const px = std({ color: TILE_3D.sideDark });
  const nx = std({ color: TILE_3D.sideDark });
  const py = std({ color: TILE_3D.side });        // 后仰后露出的「白侧棱」
  const ny = std({ color: TILE_3D.side });   // 另一侧的端面，同样亮
  const nz = std({ color: 0xd9d5c8 });            // 朝内的一面：象牙白牌身，不要黑块

  let pz;
  if (faceUp) {
    pz = std({ map: getAtlas() });
  } else {
    pz = std({ map: backTexture() });
  }
  const mats = [px, nx, py, ny, pz, nz];         // BoxGeometry: +x,-x,+y,-y,+z,-z
  cache.set(key, mats);
  return mats;
}

/** 把 BoxGeometry 的 +z 面 UV 重映射到图集里 code 那一格 */
function mapAtlasUV(geo, code) {
  const idx = INDEX.has(code) ? INDEX.get(code) : INDEX.get('back');
  const col = idx % ATLAS_COLS, row = Math.floor(idx / ATLAS_COLS);
  const u0 = col / ATLAS_COLS, u1 = (col + 1) / ATLAS_COLS;
  const v1 = 1 - row / ATLAS_ROWS, v0 = 1 - (row + 1) / ATLAS_ROWS;
  const uv = geo.attributes.uv;
  // +z 面是第 5 个面，顶点 16..19，顺序：左上/右上/左下/右下
  uv.setXY(16, u0, v1); uv.setXY(17, u1, v1);
  uv.setXY(18, u0, v0); uv.setXY(19, u1, v0);
  uv.needsUpdate = true;
}

/**
 * @param {object} o
 * @param {string} o.code   牌面编码（faceUp 时有效）
 * @param {boolean} o.faceUp true=明牌，false=牌背
 * @param {number} o.w 牌宽（沿排布方向）
 * @param {number} o.h 牌高（沿法向，后仰前）
 * @param {number} o.d 牌厚
 */
export function makeTile3D({ code = 'back', faceUp = false, w, h, d }) {
  const geo = new THREE.BoxGeometry(w, h, d);
  if (faceUp) mapAtlasUV(geo, code);
  const mesh = new THREE.Mesh(geo, materials(faceUp, code));
  mesh.castShadow = true;
  // 不接收自阴影：否则光斜着打过来时，左邻牌会把自己那半截白侧棱染暗，
  // 整排白棱呈楔形由宽变窄，看起来像贴图没画好。投影落在台布上就够了。
  mesh.receiveShadow = false;
  mesh.userData = { kind: 'tile3d', code, faceUp, w, h, d };
  return mesh;
}

/** 清空材质缓存（切换图集 / 热重载时用） */
export function clearTile3DCache() {
  cache.clear();
  if (backTex) { backTex.dispose(); backTex = null; }
}
