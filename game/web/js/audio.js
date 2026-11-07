/** 音效：复用 client/assets/sfx 的三个 ogg，浏览器需用户手势后才能播放。 */
import { SFX_URL } from './assets.js';

export class Sfx {
  constructor() {
    this.ctx = null;
    this.buffers = {};
    this.enabled = true;
  }

  async load() {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return;
    this.ctx = new Ctx();
    await Promise.all(Object.entries(SFX_URL).map(async ([name, url]) => {
      try {
        const res = await fetch(url);
        if (!res.ok) return;
        const buf = await res.arrayBuffer();
        this.buffers[name] = await this.ctx.decodeAudioData(buf);
      } catch (_) { /* 缺音效不影响牌局 */ }
    }));
  }

  play(name) {
    if (!this.enabled || !this.ctx || !this.buffers[name]) return;
    if (this.ctx.state === 'suspended') this.ctx.resume();
    const src = this.ctx.createBufferSource();
    src.buffer = this.buffers[name];
    src.connect(this.ctx.destination);
    src.start();
  }
}
