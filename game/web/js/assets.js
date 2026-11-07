/** 素材加载：牌面图集 UV 索引、音效路径。素材由 game/build_web.py 生成。 */
import * as THREE from 'three';
import { ALL_CODES } from './config.js';

const ATLAS_COLS = 8;
const ATLAS_ROWS = 5;
const ATLAS_URL = './assets/tile-atlas.png';       // 旧版金框牌面（保留兜底）
const FLAT_ATLAS_URL = './assets/tile-atlas-flat.png';  // 平躺牌面（白牌身，厚度交给几何）
const HAND_ATLAS_URL = './assets/tile-atlas-hand.png';  // 本家立牌（程序化立体牌身）
const INK_ATLAS_URL = './assets/tile-atlas-ink.png';    // 舍牌墨迹（透明底，只有图案）

export const SFX_URL = {
  discard: './assets/sfx/discard.ogg',
  draw: './assets/sfx/draw.ogg',
  click: './assets/sfx/click.ogg',
};

const INDEX = new Map(ALL_CODES.map((c, i) => [c, i]));

const textures = { river: null, flat: null, hand: null, ink: null };

function load(url, key) {
  if (textures[key]) return Promise.resolve(textures[key]);
  return new Promise((resolve, reject) => {
    new THREE.TextureLoader().load(
      url,
      (tex) => {
        tex.colorSpace = THREE.SRGBColorSpace;
        tex.magFilter = THREE.LinearFilter;
        tex.minFilter = THREE.LinearMipmapLinearFilter;
        tex.generateMipmaps = true;
        tex.anisotropy = 4;
        tex.needsUpdate = true;
        textures[key] = tex;
        resolve(tex);
      },
      undefined,
      reject,
    );
  });
}

/** 三套图集共用 8x5 的 UV 排布，faceGeometry 无需区分 */
export function loadAtlas() {
  return Promise.all([
    load(ATLAS_URL, 'river'),
    load(FLAT_ATLAS_URL, 'flat'),
    load(HAND_ATLAS_URL, 'hand'),
    load(INK_ATLAS_URL, 'ink'),
  ]);
}

export function getAtlas() {
  return textures.river;
}

export function getFlatAtlas() {
  return textures.flat || textures.river;
}

export function getHandAtlas() {
  return textures.hand;
}

export function getInkAtlas() {
  return textures.ink;
}

/**
 * 生成一个只采样图集中某一格的 PlaneGeometry。
 * 用自定义 UV 而不是 clone texture，38 张牌共用一张 GPU 纹理。
 */
export function faceGeometry(code, w, h) {
  const idx = INDEX.has(code) ? INDEX.get(code) : INDEX.get('back');
  const col = idx % ATLAS_COLS;
  const row = Math.floor(idx / ATLAS_COLS);
  const u0 = col / ATLAS_COLS;
  const u1 = (col + 1) / ATLAS_COLS;
  const v1 = 1 - row / ATLAS_ROWS;
  const v0 = 1 - (row + 1) / ATLAS_ROWS;

  const geo = new THREE.PlaneGeometry(w, h);
  const uv = geo.attributes.uv;
  // PlaneGeometry UV 顶点顺序: 左上, 右上, 左下, 右下
  uv.setXY(0, u0, v1);
  uv.setXY(1, u1, v1);
  uv.setXY(2, u0, v0);
  uv.setXY(3, u1, v0);
  uv.needsUpdate = true;
  return geo;
}

export const FONT = '"PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", sans-serif';
