/** 交互：3D 拾取 + 键盘 + 音效开关。点选 -> 再点/Enter 打出的口径沿用第一版。 */
import * as THREE from 'three';
import { ACTION, FRAME_TYPE } from './protocol_enums.js';

export class Input {
  constructor({ canvas, camera, app, hand, sfx, ui, client }) {
    this.canvas = canvas;
    this.camera = camera;
    this.app = app;
    this.hand = hand;
    this.sfx = sfx;
    this.ui = ui;
    this.client = client;
    this.ray = new THREE.Raycaster();
    this.ndc = new THREE.Vector2();
    this.bind();
  }

  bind() {
    this.canvas.addEventListener('pointerdown', (e) => this.onPointer(e));
    window.addEventListener('keydown', (e) => this.onKey(e));
    // 音效开关统一在 main.js 里接（顶部 ♪ 与底部小按钮共用一套状态）
  }

  /** 只有服务端给了窗口、且没有挂起提交/冻结时才允许摸牌面（同参考端 _can_select_hand） */
  canSelectHand() {
    const client = this.client;
    if (!client.window) return false;
    if (client.pending || client.frozen) return false;
    if (client.remainingMs() <= 0) return false;
    return true;
  }

  onPointer(e) {
    const r = this.canvas.getBoundingClientRect();
    this.ndc.set(
      ((e.clientX - r.left) / r.width) * 2 - 1,
      -((e.clientY - r.top) / r.height) * 2 + 1,
    );
    this.ray.setFromCamera(this.ndc, this.camera);
    const hits = this.ray.intersectObjects(this.hand.pickables, false);
    if (!hits.length) return;
    const idx = hits[0].object.userData.pick?.index ?? -1;
    if (idx < 0) return;

    this.sfx.play('click');
    // 立直宣言模式：点一张候选牌 = 直接宣言并打出（§12.4 / v1.21）
    if (this.client.riichiArming) {
      this.client.commitRiichiAt(idx);
      return;
    }
    if (!this.canSelectHand()) return;
    if (this.app.state.selected === idx) this.confirm();
    else this.app.select(idx);
  }

  onKey(e) {
    const k = e.key;
    if (!this.client) return;
    if (k === 'Escape') {
      this.app.select(-1);
      this.client.cancelRiichiArming();
      return;
    }
    if (k === ' ' || k === 'Spacebar') {
      // 参考端口径：空格 = 过（鸣牌窗口内）
      e.preventDefault();
      this.client.action(ACTION.PASS, []);
      return;
    }
    if (k >= '1' && k <= '9') {
      if (!this.canSelectHand()) return;
      this.app.select(parseInt(k, 10) - 1);
      this.sfx.play('click');
    } else if (k === '0') {
      if (!this.canSelectHand()) return;
      this.app.select(9);
      this.sfx.play('click');
    } else if (k === 'Enter') {
      e.preventDefault();
      this.confirm();
    } else if (k === 's' || k === 'S') {
      // 与服务端对齐（PROTOCOL §4.7 的 sync），沿用旧「摸牌」按钮的手感
      this.client.send({ type: FRAME_TYPE.SYNC });
    }
  }

  /** 打牌必须由服务端确认：本地只发帧，等 discard_event 落地 */
  confirm() {
    this.client.discardSelected();
  }
}
