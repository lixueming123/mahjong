/**
 * 桌面平躺牌（牌河 / 副露）的透视通道。
 *
 * 为什么不继续用正交相机：正交正俯视下，平躺的牌只能靠「斜切盒」假装露出一条
 * 下缘，看起来永远是「纸片 + 一根黄条」。录像是带俯角的透视：牌平放在桌面上，
 * 镜头从前上方看，能自然看到牌的前侧面（上半象牙白牌身、下半橙色牌背），
 * 远处的牌略小、纵向被压扁。
 *
 * 做法：与三家手牌共用同一台透视相机（opponents.camera）。调用方给出每张牌的
 * **桌面世界**落位（通常从某个设计坐标锚点反投影出来，再在世界里贴紧排开），
 * 所有牌同一个世界尺寸 —— 近大远小、纵向压缩、前后遮挡都交给真透视。
 */
import * as THREE from 'three';
import { DESIGN } from './config.js';

export class TablePass {
  /** @param {import('./opponents.js').OpponentHandsView} opponents 共用其透视相机与反投影 */
  constructor(opponents) {
    this.opponents = opponents;
    this.scene = new THREE.Scene();
    this.scene.add(new THREE.AmbientLight(0xffffff, .95));
    // 光从前上方来，才能照亮朝向镜头的前侧面
    const key = new THREE.DirectionalLight(0xfff2dc, .9);
    key.position.set(-420, -900, 1200);
    this.scene.add(key);
    this.placements = [];   // { wrapper, solve }
  }

  /** 设计坐标 (dx,dy) 处，每个设计像素对应的桌面世界长度（x 向、y 向） */
  scaleAt(dx, dy) {
    const u = (x, y) => this.opponents.unprojectToTable(x, y);
    const a = u(dx - 10, dy), b = u(dx + 10, dy);
    const c = u(dx, dy - 10), d = u(dx, dy + 10);
    return { sx: a.distanceTo(b) / 20, sy: c.distanceTo(d) / 20 };
  }

  /** 世界点投影回设计坐标（x 右、y 下），用当前可见设计区域换算 */
  toDesign(x, y, z) {
    const p = new THREE.Vector3(x, y, z).project(this.opponents.camera);
    return {
      x: DESIGN.CX + p.x * this.opponents.viewW / 2,
      y: DESIGN.CY - p.y * this.opponents.viewH / 2,
    };
  }

  /**
   * 按桌面世界坐标摆放 tile（局部单位 = 世界尺寸 / sx，z 为厚度方向）。
   * solve() 返回 { x, y, z, sx, sy }（世界位置与缩放），视口变化后会重新调用。
   * @param {THREE.Object3D} parent  装牌的 Group（已在 this.scene 里）
   */
  placeWorld(tile, parent, solve) {
    const wrapper = new THREE.Group();
    wrapper.add(tile);
    parent.add(wrapper);
    const p = { wrapper, solve };
    this.placements.push(p);
    this.apply(p);
    return wrapper;
  }

  apply({ wrapper, solve }) {
    const { x, y, z, sx, sy } = solve();
    wrapper.scale.set(sx, sy, sx);
    wrapper.position.set(x, y, z);
  }

  /** 视口/相机变化后重算全部摆位 */
  relayout() {
    for (const p of this.placements) {
      this.apply(p);
    }
  }

  /** 清掉某个 Group 里的牌时同步丢弃摆位记录 */
  forget(parent) {
    this.placements = this.placements.filter((p) => p.wrapper.parent !== parent);
    parent.clear();
  }

  /** 主桌（正交）之后、立牌之前叠加；clearDepth 让它压在台布之上 */
  renderOverlay(renderer) {
    renderer.autoClear = false;
    renderer.clearDepth();
    renderer.render(this.scene, this.opponents.camera);
    renderer.autoClear = true;
  }
}
