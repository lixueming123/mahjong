/**
 * 平躺麻将牌的真 3D 几何。
 *
 * 之前 tiles.js 用「白色 Plane + 一张画出来的橙金矩形」伪造牌的厚度，
 * 那就是纸片感的来源；副露里横躺那张整体 rotation.z=90° 后，
 * 假厚边跟着转到左边，就变成一条突兀的竖直大黄条。
 *
 * 这里是真实的圆角倒棱牌身（圆角矩形挤出 + 上下棱斜面）：
 *   - 顶面 = 牌面，直接采样平躺图集的对应格（UV 在几何上重映射）；
 *   - 侧面按厚度分层：上段象牙白牌面层、下段橙色牌背层，吃光照、有棱边；
 *   - 正交通道（本家副露）额外把底层往屏幕下方 / 右方斜切 thick / side，
 *     1:1 正俯视也能看到下缘与右缘的厚度；透视通道不斜切，真透视自己会露出侧面；
 *   - 落影是一块软边阴影平面，不再画半透明深色矩形。
 *
 * 「1:1」只锁设计坐标，不锁渲染方式：牌的占屏包围盒仍是 w × (h+thick)，
 * 与旧版完全一致，所以牌河/副露的位置一个像素都不用挪。
 */
import * as THREE from 'three';
import { ALL_CODES, Z } from './config.js';
import { getFlatAtlas, getHandAtlas } from './assets.js';

const ATLAS_COLS = 8, ATLAS_ROWS = 5;
const INDEX = new Map(ALL_CODES.map((c, i) => [c, i]));
/** 立牌图集单格 120x200 的宽高比（build_web.py 的 H_W / H_H） */
const HAND_CELL_ASPECT = 120 / 200;

/** 图集格的 UV 范围 */
const cellUV = (code) => {
  const idx = INDEX.has(code) ? INDEX.get(code) : INDEX.get('back');
  const col = idx % ATLAS_COLS, row = Math.floor(idx / ATLAS_COLS);
  return {
    u0: col / ATLAS_COLS, u1: (col + 1) / ATLAS_COLS,
    v0: 1 - (row + 1) / ATLAS_ROWS, v1: 1 - row / ATLAS_ROWS,
  };
};

/** 每套图集一组材质：flat = 躺牌顶面，hand = 本家立牌正面 */
const MATS = new Map();

let layerTex = null;
/**
 * 侧面的竖向色阶（v=1 在牌面一侧）：真实牌侧上 ~70% 是象牙白牌面层，
 * 下 ~30% 是橙色牌背层，中间是硬分界。
 */
function layerTexture() {
  if (layerTex) return layerTex;
  const c = document.createElement('canvas');
  c.width = 4; c.height = 64;
  const g = c.getContext('2d');
  const grad = g.createLinearGradient(0, 0, 0, 64);
  grad.addColorStop(0, '#e9e6da');
  grad.addColorStop(.7, '#c4bfae');
  grad.addColorStop(.72, '#f2a02a');
  grad.addColorStop(1, '#c8700c');
  g.fillStyle = grad; g.fillRect(0, 0, 4, 64);
  layerTex = new THREE.CanvasTexture(c);
  layerTex.colorSpace = THREE.SRGBColorSpace;
  return layerTex;
}

/** 材质分组与 roundedGeometry 一致：[侧面+斜面, 底面(-z), 牌面(+z)] */
function materials(atlas = 'flat') {
  if (MATS.has(atlas)) return MATS.get(atlas);
  const std = (o) => new THREE.MeshStandardMaterial({
    roughness: .72, metalness: .03, ...o,
  });
  const ivoryDim = std({ color: 0xc9cfc9 });
  const side = std({
    color: 0xffffff, map: layerTexture(), roughness: .5, metalness: .1,
    emissive: 0xffffff, emissiveIntensity: .55, emissiveMap: layerTexture(),
  });
  const faceTex = atlas === 'hand' ? getHandAtlas() : getFlatAtlas();
  const face = std({
    map: faceTex,
    roughness: .68,
    // 牌面四角是透明的（圆角），alphaTest 让透明片元不写深度，
    // 免得圆角那块小方框挡住后面的牌
    transparent: true,
    alphaTest: .35,
    // 只靠灯光牌面会整体压暗发灰：让贴图自己发一点光补回亮度，
    // 侧面/棱线仍然吃光照，「厚度」不会被抹平。立牌正面正对镜头、
    // 受光角最差，补得更多才和录像里的亮白牌面一致。
    emissive: 0xffffff,
    emissiveIntensity: atlas === 'hand' ? .42 : .30,
    emissiveMap: faceTex,
  });
  const mats = [side, ivoryDim, face];
  MATS.set(atlas, mats);
  return mats;
}

const geoCache = new Map();
/**
 * 圆角倒棱的牌身：俯视轮廓是圆角矩形，上下棱有一圈斜面。
 * 直角盒的四个角是尖的，和牌面图集的圆角对不上。
 * 材质分组：[侧面+斜面, 底面(-z), 牌面(+z)]，对应 materials()。
 */
function roundedGeometry({ code, w, h, d, rot, thick = 0, side = 0, atlas = 'flat' }) {
  const key = `${w}:${h}:${d}:${code}:${rot}:${thick}:${side}:${atlas}`;
  if (geoCache.has(key)) return geoCache.get(key);

  const m = Math.min(w, h);
  const bs = m * .05;                   // 斜面水平宽度
  const bt = Math.min(d * .16, bs * 1.4);
  const iw = w - 2 * bs, ih = h - 2 * bs;
  // 顶面圆角不小于图集牌面圆角（约 11.5% 宽），否则透明角会在牌面上抠出小洞
  const r = m * .17 - bs;
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
    bevelSegments: 3,
    curveSegments: 6,
  });
  ext.translate(0, 0, -(d - 2 * bt) / 2);
  const src = ext.index ? ext.toNonIndexed() : ext;
  if (src !== ext) {
    ext.dispose();
  }

  const pos = src.attributes.position;
  if (thick !== 0 || side !== 0) {
    // 正交通道（本家副露）：越往底层越往 -y / +x 推，正俯视才露得出下缘与右缘的厚度。
    // thick < 0 反过来往 +y 推：本家立牌从斜上方看，露出的是牌顶面和橙色牌背。
    // 剪切恒在牌自身的局部系：整张牌绕 z 倾斜时，厚边跟着牌一起斜。
    for (let i = 0; i < pos.count; i++) {
      const depth = (d / 2 - pos.getZ(i)) / d;
      pos.setX(i, pos.getX(i) + side * depth);
      pos.setY(i, pos.getY(i) - thick * depth);
    }
    src.computeVertexNormals();
  }
  const nor = src.attributes.normal;
  const buckets = [[], [], []];
  for (let t = 0; t < pos.count / 3; t++) {
    const nz = (nor.getZ(t * 3) + nor.getZ(t * 3 + 1) + nor.getZ(t * 3 + 2)) / 3;
    const onCap = Math.abs(nz) > .98;
    buckets[onCap ? (nz > 0 ? 2 : 1) : 0].push(t);
  }

  const { u0, u1, v0, v1 } = cellUV(code);
  const k = code === 'back' ? 0 : (rot | 0) % 4;
  // 牌面上一点 (s 左→右, t 上→下) 顺时针转 k×90° 后在原图里的位置 (a, b)
  const turn = (s, t) => [
    [s, t], [t, 1 - s], [1 - s, 1 - t], [1 - t, s],
  ][k];
  // 立牌图集按「裁切铺满」采样：牌面（图案朝向下的）宽高比和格子不同时，
  // 裁掉格子多出来的一边而不是拉伸，同一张图集竖放、横放图案都不变形
  let fu = 1, fv = 1;
  if (atlas === 'hand') {
    const faceAspect = k % 2 ? ih / iw : iw / ih;
    if (faceAspect > HAND_CELL_ASPECT) {
      fv = HAND_CELL_ASPECT / faceAspect;
    } else {
      fu = faceAspect / HAND_CELL_ASPECT;
    }
  }

  const P3 = new Float32Array(pos.count * 3);
  const N3 = new Float32Array(pos.count * 3);
  const U2 = new Float32Array(pos.count * 2);
  const geo = new THREE.BufferGeometry();
  let cursor = 0;
  buckets.forEach((tris, gi) => {
    const start = cursor;
    for (const t of tris) {
      for (let j = 0; j < 3; j++) {
        const i = t * 3 + j;
        const x = pos.getX(i), y = pos.getY(i), z = pos.getZ(i);
        P3.set([x, y, z], cursor * 3);
        N3.set([nor.getX(i), nor.getY(i), nor.getZ(i)], cursor * 3);
        let u = .5, v = Math.min(1, Math.max(0, (z + d / 2) / d));
        if (gi === 2) {
          const s = Math.min(1, Math.max(0, (x + iw / 2) / iw));
          const tt = Math.min(1, Math.max(0, (ih / 2 - y) / ih));
          const [a, b] = turn(s, tt);
          u = u0 + (u1 - u0) * (.5 + (a - .5) * fu);
          v = v1 - (v1 - v0) * (.5 + (b - .5) * fv);
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

  geoCache.set(key, geo);
  return geo;
}

/* ------------------------------------------------------- 软接触阴影 */

let shadowTex = null;
/** 多层圆角矩形叠出高斯般的软边，不再是一块硬邦邦的深色矩形 */
function softShadowTexture() {
  if (shadowTex) return shadowTex;
  const c = document.createElement('canvas');
  c.width = 128; c.height = 160;
  const g = c.getContext('2d');
  const rr = (x, y, w, h, r) => {
    g.beginPath();
    g.moveTo(x + r, y); g.lineTo(x + w - r, y);
    g.quadraticCurveTo(x + w, y, x + w, y + r); g.lineTo(x + w, y + h - r);
    g.quadraticCurveTo(x + w, y + h, x + w - r, y + h); g.lineTo(x + r, y + h);
    g.quadraticCurveTo(x, y + h, x, y + h - r); g.lineTo(x, y + r);
    g.quadraticCurveTo(x, y, x + r, y); g.closePath();
  };
  for (let i = 9; i >= 0; i--) {
    g.fillStyle = `rgba(2,8,20,${.1})`;
    rr(20 - i * 1.7, 18 - i * 1.7, 88 + i * 3.4, 124 + i * 3.4, 9 + i * 1.7);
    g.fill();
  }
  shadowTex = new THREE.CanvasTexture(c);
  return shadowTex;
}

const shadowGeoCache = new Map();
function shadowPlane(w, h, thick, d) {
  const key = `${w}:${h}:${thick}:${d}`;
  if (shadowGeoCache.has(key)) return shadowGeoCache.get(key);
  const m = new THREE.Mesh(
    new THREE.PlaneGeometry(w + 30, h + thick + 30),
    new THREE.MeshBasicMaterial({
      map: softShadowTexture(), transparent: true, opacity: .5,
      depthTest: false, depthWrite: false,
    }),
  );
  m.position.set(7, -6 - thick / 2, -d / 2 - 2);
  m.renderOrder = Z.RIVER_SHADOW;
  shadowGeoCache.set(key, m);
  return m;
}

/**
 * @param {object} o
 * @param {string} o.code  牌面编码，'back' = 牌背
 * @param {number} o.w     牌身宽
 * @param {number} o.h     牌身高（不含厚边）
 * @param {number} [o.d=18]     牌厚（真实 3D 深度）
 * @param {number} [o.thick=13] 下缘露出的厚边宽度（正交通道斜切量）
 * @param {number} [o.rot=0]    牌面图案顺时针旋转的 90° 步数
 * @param {number} [o.angle=0]  整张牌绕 z 倾斜的弧度（侧家副露用），厚边会自动补正
 * @param {number} [o.side=0]   右侧露出的窄边宽度（正交通道斜切量，0 = 不露）
 * @param {boolean} [o.persp=false] 透视相机渲染：几何不斜切
 * @param {boolean} [o.shadow=true] 是否加软接触阴影（紧贴成排的小牌要关掉，否则出深缝）
 * @param {'flat'|'hand'} [o.atlas='flat'] 牌面图集：躺牌顶面 / 本家立牌正面
 */
export function makeFlatTile({
  code, w, h, d = 18, thick = 13, rot = 0, angle = 0, side = 0, shadow = true,
  persp = false, atlas = 'flat',
}) {
  // persp：由透视相机（tablepass.js）渲染。不再斜切——真透视自己就会露出
  // 牌的前侧面（象牙白牌身 + 橙色牌背），斜切反而会画出一条多余的黄条。
  if (persp) {
    thick = 0;
    side = 0;
  }
  const mesh = new THREE.Mesh(
    roundedGeometry({ code, w, h, d, rot, thick, side, atlas }), materials(atlas),
  );
  mesh.rotation.z = angle;
  mesh.castShadow = true;
  mesh.receiveShadow = false;
  // 阴影作为子对象：跟随牌移动，但 renderOrder 独立，先于牌绘制。
  // 用 clone 而非共享同一 Mesh，避免 Three 的「一个对象只能有一个父节点」。
  if (shadow) mesh.add(shadowPlane(w + side, h, thick, d).clone());
  // 与主桌其它层同处透明队列，用 renderOrder 保持 1:1 的前后关系；
  // depthTest/depthWrite 仍开启，各面之间才有真实遮挡。
  for (const m of mesh.material) {
    m.transparent = true;
    m.depthTest = true;
    m.depthWrite = true;
  }
  mesh.userData = { kind: 'flatTile', code, w, h, d, thick, rot, side };
  return mesh;
}

export function clearFlatTileCache() {
  for (const g of geoCache.values()) g.dispose();
  geoCache.clear();
  for (const m of shadowGeoCache.values()) { m.geometry.dispose(); m.material.dispose(); }
  shadowGeoCache.clear();
  if (shadowTex) { shadowTex.dispose(); shadowTex = null; }
  if (layerTex) { layerTex.dispose(); layerTex = null; }
  for (const mats of MATS.values()) {
    for (const m of mats) m.dispose();
  }
  MATS.clear();
}
