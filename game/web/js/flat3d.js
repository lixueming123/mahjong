/**
 * 平躺麻将牌的真 3D 几何。
 *
 * 之前 tiles.js 用「白色 Plane + 一张画出来的橙金矩形」伪造牌的厚度，
 * 那就是纸片感的来源；副露里横躺那张整体 rotation.z=90° 后，
 * 假厚边跟着转到左边，就变成一条突兀的竖直大黄条。
 *
 * 这里改成真实的斜切盒（sheared box）：
 *   - 顶面 = 牌面，直接采样 tile-atlas.png 的对应格（UV 在几何上重映射）；
 *   - 底面相对顶面往屏幕下方偏移 thick，于是 1:1 正交俯视也能看到
 *     牌的下缘一条**有法线、吃光照、有棱边**的橙金牌背侧面；
 *   - 其余四面是象牙白牌身，六个面由同一 Mesh 深度测试，彼此真实遮挡；
 *   - 落影交给 shadowMap 投到台布，不再画半透明深色矩形。
 *
 * 「1:1」只锁设计坐标，不锁渲染方式：牌的占屏包围盒仍是 w × (h+thick)，
 * 与旧版完全一致，所以牌河/副露的位置一个像素都不用挪。
 */
import * as THREE from 'three';
import { ALL_CODES, Z } from './config.js';
import { getFlatAtlas } from './assets.js';

const ATLAS_COLS = 8, ATLAS_ROWS = 5;
const INDEX = new Map(ALL_CODES.map((c, i) => [c, i]));

/** 图集四角 UV：左上 / 右上 / 左下 / 右下 */
const cornerUV = (code) => {
  const idx = INDEX.has(code) ? INDEX.get(code) : INDEX.get('back');
  const col = idx % ATLAS_COLS, row = Math.floor(idx / ATLAS_COLS);
  const u0 = col / ATLAS_COLS, u1 = (col + 1) / ATLAS_COLS;
  const v1 = 1 - row / ATLAS_ROWS, v0 = 1 - (row + 1) / ATLAS_ROWS;
  return [[u0, v1], [u1, v1], [u0, v0], [u1, v0]];
};

/**
 * 顶面 4 个顶点（BoxGeometry 的 16/17/18/19 = 左上/右上/左下/右下）
 * 在图案顺时针转 k×90° 后应取样原图的哪个角。
 * 牌背不旋转。
 */
const ROT = [
  [0, 1, 2, 3],
  [2, 0, 3, 1],
  [3, 2, 1, 0],
  [1, 3, 0, 2],
];

let MATS = null;
let MATS_SIDE = null;

let goldTex = null;
/**
 * 牌背侧面的竖向色阶（录像里的样子）：顶部一线象牙白倒棱 -> 亮橙 -> 深橙 ->
 * 底缘一线深褐收边。比单纯两点顶点色渐变更像「有厚度的牌身」。
 */
function goldTexture() {
  if (goldTex) return goldTex;
  const c = document.createElement('canvas');
  c.width = 4; c.height = 64;
  const g = c.getContext('2d');
  const grad = g.createLinearGradient(0, 0, 0, 64);
  // 真实牌侧：上 ~70% 是象牙白牌身，下 ~30% 是极细的橙色牌背边，中间是硬分界
  grad.addColorStop(0, '#e9e6da');
  grad.addColorStop(.7, '#c4bfae');
  grad.addColorStop(.72, '#f2a02a');
  grad.addColorStop(1, '#c8700c');
  g.fillStyle = grad; g.fillRect(0, 0, 4, 64);
  goldTex = new THREE.CanvasTexture(c);
  goldTex.colorSpace = THREE.SRGBColorSpace;
  return goldTex;
}
/**
 * 六面材质：正俯视下只有「被斜切拉开」的那两个面能露出投影，
 * 所以真正决定牌长什么样的就是 +z 牌面、-y 下缘、+x 右缘这三张。
 * 副露牌额外要金色右缘（side>0）—— 录像里立着的牌从斜前方看，
 * 右侧是一条和牌背同色的橙金窄边，和手牌一致。
 */
function materials(withSide) {
  if (MATS) return withSide ? MATS_SIDE : MATS;
  const std = (o) => new THREE.MeshStandardMaterial({
    roughness: .72, metalness: .03, ...o,
  });
  const ivory = std({ color: 0xe8ebe6 });
  const ivoryDim = std({ color: 0xc9cfc9 });
  // 下缘橙金牌背：色阶来自几何顶点色（上缘棱线亮黄 -> 下缘深棕），
  // 再乘真实光照；不是一张画好渐变的贴图。
  const gold = std({
    color: 0xffffff, map: goldTexture(), roughness: .5, metalness: .1,
    emissive: 0xffffff, emissiveIntensity: .55, emissiveMap: goldTexture(),
  });
  const flatTex = getFlatAtlas();
  const face = std({
    map: flatTex,
    roughness: .68,
    // 牌面四角是透明的（圆角），alphaTest 让透明片元不写深度，
    // 免得圆角那块小方框挡住后面的牌
    transparent: true,
    alphaTest: .35,
    // 本家手牌走 2D 图集（完全不受光），躺牌如果只靠灯光会整体压暗发灰，
    // 两种牌摆在一起就像两套素材。这里让贴图自己发一点光补回亮度，
    // 侧面/棱线仍然吃光照，「厚度」不会被抹平。
    emissive: 0xffffff,
    emissiveIntensity: .30,
    emissiveMap: flatTex,
  });
  // BoxGeometry 面序：+x, -x, +y, -y, +z, -z
  MATS = [ivoryDim, ivoryDim, ivory, gold, face, ivoryDim];
  MATS_SIDE = [gold, ivoryDim, ivory, gold, face, ivoryDim];
  return withSide ? MATS_SIDE : MATS;
}

const geoCache = new Map();
function geometry({ code, w, h, d, thick, rot, angle = 0, side = 0 }) {
  const key = `${w}:${h}:${d}:${thick}:${code}:${rot}:${angle}:${side}`;
  if (geoCache.has(key)) return geoCache.get(key);

  const geo = new THREE.BoxGeometry(w, h, d);
  const pos = geo.attributes.position;
  // 只把「底层」(z<0) 的顶点推开，顶面保持不动 → 斜棱柱。
  // 剪切恒在**牌自身的局部 -y**：整张牌绕 z 倾斜时，厚边跟着牌一起斜，
  // 才和录像里侧家斜牌一致（橙金棱沿牌的斜下缘，不是一条水平带）。
  // 侧家 angle 只有约 18°，不会像早期那样转 90° 竖成大黄条。
  //
  // side 是右侧那条金边：底层顶点再往 +x 推一点，于是 +x 面被拉斜，
  // 正俯视也能看到一条 side 宽的牌背侧棱（牌是立着的，右侧本来就该露）。
  const sx = side;
  const sy = -thick;
  for (let i = 0; i < pos.count; i++) {
    if (pos.getZ(i) < 0) {
      pos.setX(i, pos.getX(i) + sx);
      pos.setY(i, pos.getY(i) + sy);
    }
  }
  pos.needsUpdate = true;
  geo.computeVertexNormals();

  // 下缘面（顶点 12..15）的法线人为朝上翘一点：
  // 斜切后它的几何法线带 -z 分量会完全背光，这里按「相机有俯角」的真实观感
  // 给上下两条边不同法线，明暗渐变由光照自然产生，而不是画进贴图。
  const nrm = geo.attributes.normal;
  // BoxGeometry -y 面顶点顺序：12 左上(z=+d/2), 13 右上, 14 左下(z=-d/2), 15 右下
  const up = new THREE.Vector3(0, -.16, .99).normalize();
  const down = new THREE.Vector3(0, -.62, .78).normalize();
  nrm.setXYZ(12, up.x, up.y, up.z); nrm.setXYZ(13, up.x, up.y, up.z);
  nrm.setXYZ(14, down.x, down.y, down.z); nrm.setXYZ(15, down.x, down.y, down.z);
  // +x 面（顶点 0..3）：0/1 上缘、2/3 下缘，同样按「有俯角」给法线
  const rUp = new THREE.Vector3(.55, -.12, .82).normalize();
  const rDown = new THREE.Vector3(.52, -.52, .68).normalize();
  nrm.setXYZ(0, rUp.x, rUp.y, rUp.z); nrm.setXYZ(1, rUp.x, rUp.y, rUp.z);
  nrm.setXYZ(2, rDown.x, rDown.y, rDown.z); nrm.setXYZ(3, rDown.x, rDown.y, rDown.z);
  nrm.needsUpdate = true;

  // 色阶：上边一条亮黄棱线 -> 下缘深棕。
  // 只有 gold 材质开 vertexColors，其余面读到全白，不受影响。
  const col = new Float32Array(pos.count * 3).fill(1);
  const put = (i, hex) => {
    const c = new THREE.Color(hex);
    col[i * 3] = c.r; col[i * 3 + 1] = c.g; col[i * 3 + 2] = c.b;
  };
  put(12, 0xffdc86); put(13, 0xffdc86);
  put(14, 0xa1580b); put(15, 0xa1580b);
  put(0, 0xffd67a); put(1, 0xffd67a);
  put(2, 0x9c5309); put(3, 0x9c5309);
  geo.setAttribute('color', new THREE.BufferAttribute(col, 3));
  {
    // 下缘面 12..15 与右缘面 0..3：v 上=1 下=0，对应 goldTexture 的竖向色阶
    const guv = geo.attributes.uv;
    [[12, 1], [13, 1], [14, 0], [15, 0], [0, 1], [1, 1], [2, 0], [3, 0]]
      .forEach(([vi, v]) => guv.setXY(vi, .5, v));
  }

  // 顶面 UV -> 图集里 code 那一格，并按座位/横躺旋转
  const c = cornerUV(code);
  const k = code === 'back' ? 0 : (rot | 0) % 4;
  const pick = ROT[k];
  const uv = geo.attributes.uv;
  [16, 17, 18, 19].forEach((v, i) => {
    const [u, vv] = c[pick[i]];
    uv.setXY(v, u, vv);
  });
  uv.needsUpdate = true;

  geoCache.set(key, geo);
  return geo;
}

/* ------------------------------------------------------- 牌身底衬 */

const bodyCache = new Map();
/**
 * 一整块「牌身侧影」：圆角矩形，上段象牙白（牌身），下段橙色（牌背层），
 * 垫在牌面正下方。牌面图集四角是透明的，没有这块底衬时，牌面与橙边之间会
 * 露出台布的蓝色缝，橙边就像漂在牌下面的一根黄条。
 * 底衬与牌面同宽、圆角连续，牌面+侧面才读成同一块实心的牌。
 */
function bodyMesh(w, h, thick, d) {
  const key = `${w}:${h}:${thick}`;
  let entry = bodyCache.get(key);
  if (!entry) {
    const K = 4;                                   // 每设计像素 4 个贴图像素
    const cw = Math.round(w * K), ch = Math.round((h + thick) * K);
    const c = document.createElement('canvas');
    c.width = cw; c.height = ch;
    const g = c.getContext('2d');
    const r = Math.min(9 * K, cw / 6);
    g.beginPath();
    g.moveTo(r, 0); g.arcTo(cw, 0, cw, ch, r); g.arcTo(cw, ch, 0, ch, r * .7);
    g.arcTo(0, ch, 0, 0, r * .7); g.arcTo(0, 0, cw, 0, r); g.closePath();
    g.clip();
    const orangeH = Math.max(3, thick * .62) * K;
    const split = ch - orangeH;
    const ivory = g.createLinearGradient(0, 0, 0, split);
    ivory.addColorStop(0, '#f4f1e8');
    ivory.addColorStop(.7, '#e6e2d4');
    ivory.addColorStop(1, '#c9c4b2');
    g.fillStyle = ivory; g.fillRect(0, 0, cw, split);
    const orange = g.createLinearGradient(0, split, 0, ch);
    orange.addColorStop(0, '#ffc050');
    orange.addColorStop(.35, '#f39a1c');
    orange.addColorStop(1, '#c46c08');
    g.fillStyle = orange; g.fillRect(0, split, cw, orangeH);
    // 象牙/橙色交界一条细暗线（两种材料的接缝）
    g.fillStyle = 'rgba(110,60,0,.55)';
    g.fillRect(0, split - K * .4, cw, K * .8);
    const tex = new THREE.CanvasTexture(c);
    tex.colorSpace = THREE.SRGBColorSpace;
    tex.anisotropy = 4;
    entry = {
      tex,
      geo: new THREE.PlaneGeometry(w, h + thick),
      mat: new THREE.MeshBasicMaterial({ map: tex, transparent: true, alphaTest: .3 }),
    };
    bodyCache.set(key, entry);
  }
  const m = new THREE.Mesh(entry.geo, entry.mat);
  m.position.set(0, -thick / 2, d / 2 - .6);
  m.raycast = () => {};
  return m;
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

/* ------------------------------------------------------- 牌面倒棱高光 */

let bevelTex = null;
/**
 * 盖在牌面上的透明层：左上缘一圈亮边、右下缘一圈暗边，
 * 外加一层很淡的明暗渐变，让平躺的牌面读成「微凸的圆角面」而不是贴纸。
 */
function bevelTexture() {
  if (bevelTex) return bevelTex;
  const W = 128, H = 160, R = 14;
  const c = document.createElement('canvas');
  c.width = W; c.height = H;
  const g = c.getContext('2d');
  const path = (inset) => {
    const x = inset, y = inset, w = W - inset * 2, h = H - inset * 2, r = R - inset * .5;
    g.beginPath();
    g.moveTo(x + r, y); g.arcTo(x + w, y, x + w, y + h, r);
    g.arcTo(x + w, y + h, x, y + h, r); g.arcTo(x, y + h, x, y, r);
    g.arcTo(x, y, x + w, y, r); g.closePath();
  };
  g.save();
  path(1); g.clip();
  const lit = g.createLinearGradient(0, 0, W, H);
  lit.addColorStop(0, 'rgba(255,255,255,.22)');
  lit.addColorStop(.5, 'rgba(255,255,255,0)');
  lit.addColorStop(1, 'rgba(40,50,70,.26)');
  g.fillStyle = lit; g.fillRect(0, 0, W, H);
  // 亮边：整圈描边后用渐变裁成只剩左上
  g.lineWidth = 6;
  const hi = g.createLinearGradient(0, 0, W, H);
  hi.addColorStop(0, 'rgba(255,255,255,.85)');
  hi.addColorStop(.55, 'rgba(255,255,255,0)');
  hi.addColorStop(.6, 'rgba(30,40,60,0)');
  hi.addColorStop(1, 'rgba(30,40,60,.75)');
  g.strokeStyle = hi;
  path(3); g.stroke();
  // 最外圈细暗线：把相邻两张牌明确分开（淡一点，免得牌面与金边之间出一道黑缝）
  g.lineWidth = 2;
  g.strokeStyle = 'rgba(20,28,44,.2)';
  path(1); g.stroke();
  g.restore();
  bevelTex = new THREE.CanvasTexture(c);
  bevelTex.colorSpace = THREE.SRGBColorSpace;
  return bevelTex;
}

const bevelMat = () => new THREE.MeshBasicMaterial({
  map: bevelTexture(), transparent: true, depthTest: true, depthWrite: false,
});
let sharedBevelMat = null;

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
 * @param {number} [o.thick=13] 下缘露出的橙金厚边宽度
 * @param {number} [o.rot=0]    牌面图案顺时针旋转的 90° 步数
 * @param {number} [o.angle=0]  整张牌绕 z 倾斜的弧度（侧家副露用），厚边会自动补正
 * @param {number} [o.side=0]   右侧露出的橙金牌背窄边宽度（副露牌用，0 = 纯象牙白侧棱）
 * @param {boolean} [o.shadow=true] 是否加软接触阴影（紧贴成排的小牌要关掉，否则出深缝）
 */
export function makeFlatTile({
  code, w, h, d = 18, thick = 13, rot = 0, angle = 0, side = 0, shadow = true,
}) {
  const mesh = new THREE.Mesh(
    geometry({ code, w, h, d, thick, rot, angle, side }),
    materials(side > 0),
  );
  mesh.rotation.z = angle;
  mesh.castShadow = true;
  mesh.receiveShadow = false;
  // 阴影作为子对象：跟随牌移动，但 renderOrder 独立，先于牌绘制。
  // 用 clone 而非共享同一 Mesh，避免 Three 的「一个对象只能有一个父节点」。
  if (shadow) mesh.add(shadowPlane(w + side, h, thick, d).clone());
  // 与主桌其它层同处透明队列，用 renderOrder 保持 1:1 的前后关系；
  // depthTest/depthWrite 仍开启，六面之间才有真实遮挡。
  for (const m of mesh.material) {
    m.transparent = true;
    m.depthTest = true;
    m.depthWrite = true;
  }
  // 牌面倒棱高光层：和牌共用同一个 renderOrder（略高一点），随牌移动/倾斜
  sharedBevelMat = sharedBevelMat || bevelMat();
  const bevel = new THREE.Mesh(new THREE.PlaneGeometry(w, h), sharedBevelMat);
  bevel.position.z = d / 2 + .4;
  bevel.raycast = () => {};
  mesh.add(bevel);
  const body = bodyMesh(w, h, thick, d);
  if (side === 0) { mesh.add(body); }
  let order = mesh.renderOrder;
  Object.defineProperty(mesh, 'renderOrder', {
    get: () => order,
    set: (v) => { order = v; body.renderOrder = v - .0004; bevel.renderOrder = v + .0004; },
  });
  mesh.renderOrder = order;
  mesh.userData = { kind: 'flatTile', code, w, h, d, thick, rot, side };
  return mesh;
}

export function clearFlatTileCache() {
  for (const g of geoCache.values()) g.dispose();
  geoCache.clear();
  for (const { tex, geo, mat } of bodyCache.values()) { tex.dispose(); geo.dispose(); mat.dispose(); }
  bodyCache.clear();
  for (const m of shadowGeoCache.values()) { m.geometry.dispose(); m.material.dispose(); }
  shadowGeoCache.clear();
  if (shadowTex) { shadowTex.dispose(); shadowTex = null; }
  if (sharedBevelMat) { sharedBevelMat.dispose(); sharedBevelMat = null; }
  if (bevelTex) { bevelTex.dispose(); bevelTex = null; }
  // 两套材质数组共用同一批材质对象，去重后再释放
  for (const m of new Set([...(MATS || []), ...(MATS_SIDE || [])])) m.dispose();
  MATS = null; MATS_SIDE = null;
}
