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
  // 录像里牌背是一整块橙色，紧挨的牌之间只有一道很细的暗缝；
  // 缝主要靠橙色倒棱的明暗给出，这里不再画粗边框（会像嵌在白框里的按钮）。
  g.strokeStyle = 'rgba(120,58,6,.45)'; g.lineWidth = 3;
  g.strokeRect(1.5, 1.5, c.width - 3, c.height - 3);
  backTex = new THREE.CanvasTexture(c);
  backTex.colorSpace = THREE.SRGBColorSpace;
  return backTex;
}

const layerTex = new Map();
/**
 * 侧面 + 倒棱的厚度分层：真实麻将牌是「橙色牌背层 + 象牙白牌面层」粘起来的，
 * 侧面应当一半橙一半白。v 沿牌厚（v=1 在 +z），backAtTop = 牌背朝 +z。
 */
function layerTexture(backAtTop) {
  if (layerTex.has(backAtTop)) return layerTex.get(backAtTop);
  const c = document.createElement('canvas');
  c.width = 4; c.height = 64;
  const g = c.getContext('2d');
  const BACK = .42;                      // 牌背层占厚度的比例
  const grad = g.createLinearGradient(0, 0, 0, 64);
  const stops = [
    [0, TILE_3D.backMid], [BACK - .02, TILE_3D.backMid],
    [BACK, '#f6f3ea'], [1, '#e4e2d8'],
  ];
  for (const [t, color] of stops) {
    grad.addColorStop(backAtTop ? t : 1 - t, color);
  }
  g.fillStyle = grad; g.fillRect(0, 0, 4, 64);
  const tex = new THREE.CanvasTexture(c);
  tex.colorSpace = THREE.SRGBColorSpace;
  layerTex.set(backAtTop, tex);
  return tex;
}

const cache = new Map();
function materials(faceUp, code) {
  // 牌面朝上（明牌）时 +z 面要采样图集，按 code 缓存；牌背共用一套
  const key = faceUp ? `f:${code}` : 'b';
  if (cache.has(key)) return cache.get(key);

  const std = (opts) => new THREE.MeshStandardMaterial({ roughness: .78, metalness: .02, ...opts });
  const layers = layerTexture(!faceUp);
  const side = std({ map: layers, roughness: .5, emissive: 0xffffff, emissiveMap: layers, emissiveIntensity: .22 });   // 侧面 + 倒棱：橙/白分层
  // 朝内的一面：牌背朝外时是象牙白牌面一侧；明牌摊开时则是橙色牌背
  const nz = faceUp
    ? std({ color: TILE_3D.backMid, emissive: TILE_3D.backMid, emissiveIntensity: .15 })
    : std({ color: 0xd9d5c8, emissive: 0xffffff, emissiveIntensity: .15 });

  let pz;
  if (faceUp) {
    pz = std({ map: getAtlas(), emissive: 0xffffff, emissiveMap: getAtlas(), emissiveIntensity: .3 });
  } else {
    pz = std({ map: backTexture(), emissive: 0xffffff, emissiveMap: backTexture(), emissiveIntensity: .55 });
  }
  const mats = [side, nz, pz];                   // bevelTileGeometry 分组：侧面、背面(-z)、正面(+z)
  cache.set(key, mats);
  return mats;
}

/**
 * 圆角倒棱的牌身：比直角 BoxGeometry 多出一圈会反光的斜面，
 * 一整排牌之间天然有凹缝，不再像一根长方体。
 * 材质分组与 BoxGeometry 兼容的三组：[侧面+斜面, 背面(-z), 正面(+z)]，
 * 对应 materials() 里 [side, nz, pz]。
 */
function bevelTileGeometry(w, h, d, code, faceUp) {
  const bs = Math.min(w, h) * .06;     // 斜面水平宽度
  const bt = Math.min(d * .2, bs);     // 斜面厚度
  const iw = w - 2 * bs, ih = h - 2 * bs;
  const r = Math.min(iw, ih) * .12;
  const x0 = -iw / 2, y0 = -ih / 2;
  const shape = new THREE.Shape();
  shape.moveTo(x0 + r, y0);
  shape.lineTo(x0 + iw - r, y0);
  shape.quadraticCurveTo(x0 + iw, y0, x0 + iw, y0 + r);
  shape.lineTo(x0 + iw, y0 + ih - r);
  shape.quadraticCurveTo(x0 + iw, y0 + ih, x0 + iw - r, y0 + ih);
  shape.lineTo(x0 + r, y0 + ih);
  shape.quadraticCurveTo(x0, y0 + ih, x0, y0 + ih - r);
  shape.lineTo(x0, y0 + r);
  shape.quadraticCurveTo(x0, y0, x0 + r, y0);

  const ext = new THREE.ExtrudeGeometry(shape, {
    depth: d - 2 * bt,
    bevelEnabled: true,
    bevelThickness: bt,
    bevelSize: bs,
    bevelOffset: 0,
    bevelSegments: 2,
    curveSegments: 4,
  });
  ext.translate(0, 0, -(d - 2 * bt) / 2);
  const src = ext.toNonIndexed();
  ext.dispose();

  // 非索引几何：每 3 个顶点一个三角形；按面法线把三角形分到 侧面 / 背面 / 正面
  const pos = src.attributes.position;
  const nor = src.attributes.normal;
  const uvA = src.attributes.uv;
  const buckets = [[], [], []];
  for (let t = 0; t < pos.count / 3; t++) {
    const nz = (nor.getZ(t * 3) + nor.getZ(t * 3 + 1) + nor.getZ(t * 3 + 2)) / 3;
    const onCap = Math.abs(nz) > .98;
    buckets[onCap ? (nz > 0 ? 2 : 1) : 0].push(t);
  }
  const total = pos.count;
  const P3 = new Float32Array(total * 3);
  const N3 = new Float32Array(total * 3);
  const U2 = new Float32Array(total * 2);
  const geo = new THREE.BufferGeometry();
  let cursor = 0;
  buckets.forEach((tris, gi) => {
    const start = cursor;
    for (const t of tris) {
      for (let k = 0; k < 3; k++) {
        const s = t * 3 + k;
        P3.set([pos.getX(s), pos.getY(s), pos.getZ(s)], cursor * 3);
        N3.set([nor.getX(s), nor.getY(s), nor.getZ(s)], cursor * 3);
        let u = uvA.getX(s), v = uvA.getY(s);
        if (gi === 0) {
          u = .5;
          v = Math.min(1, Math.max(0, (pos.getZ(s) + d / 2) / d));
        }
        if (gi === 2) {
          // 正面：按位置映射成 0..1，再落到图集格 / 牌背纹理
          u = Math.min(1, Math.max(0, (pos.getX(s) + w / 2) / w));
          v = Math.min(1, Math.max(0, (pos.getY(s) + h / 2) / h));
          if (faceUp) {
            const idx = INDEX.has(code) ? INDEX.get(code) : INDEX.get('back');
            const col = idx % ATLAS_COLS, row = Math.floor(idx / ATLAS_COLS);
            u = (col + u) / ATLAS_COLS;
            v = 1 - (row + 1 - v) / ATLAS_ROWS;
          }
        }
        U2.set([u, v], cursor * 2);
        cursor++;
      }
    }
    geo.addGroup(start, cursor - start, gi);
  });
  src.dispose();
  geo.setAttribute('position', new THREE.BufferAttribute(P3, 3));
  geo.setAttribute('normal', new THREE.BufferAttribute(N3, 3));
  geo.setAttribute('uv', new THREE.BufferAttribute(U2, 2));
  return geo;
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
  const geo = bevelTileGeometry(w, h, d, code, faceUp);
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
  for (const tex of layerTex.values()) tex.dispose();
  layerTex.clear();
}
