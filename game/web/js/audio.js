/** 音效 + 结算播报语音：复用 client/assets 的 ogg / vo wav，浏览器需用户手势后才能播放。 */
import { SFX_URL } from './assets.js';
import { VOICE_GAP_MS } from './voice.js';

export class Sfx {
  constructor() {
    this.ctx = null;
    this.buffers = {};
    this.voices = {};        // cue id -> AudioBuffer | null（null = 拉取失败，不再重试）
    this.enabled = true;
    this._voiceSeq = 0;      // 语音序列令牌：新一轮 speak 使旧序列作废
    this._voiceSrc = null;   // 正在播的语音源（新序列启动时掐掉）
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

  /** 拉取并解码一条语音（assets/vo/<id>.wav，由 build_web.py 从 client/assets/vo 拷贝）。 */
  async loadCue(id) {
    if (this.voices[id] !== undefined) return this.voices[id];
    try {
      const res = await fetch(`./assets/vo/${id}.wav`);
      if (!res.ok) throw new Error(`vo ${id}: HTTP ${res.status}`);
      const buf = await this.ctx.decodeAudioData(await res.arrayBuffer());
      this.voices[id] = buf;
    } catch (_) {
      this.voices[id] = null;   // 缺句是合法降级：跳过不报错
    }
    return this.voices[id];
  }

  /**
   * 顺序播放一组语音 cue（voice.js 的 buildVoiceCues 产物）。
   * 逐条 await：上一条 onended 再停 VOICE_GAP_MS 起下一条；任何时刻只允许
   * 一个序列在播（新序列使旧序列作废，并掐掉正在响的那条）。
   */
  async speak(cues) {
    if (!this.enabled || !this.ctx || !Array.isArray(cues) || !cues.length) return;
    if (this.ctx.state === 'suspended') this.ctx.resume();
    const seq = ++this._voiceSeq;
    if (this._voiceSrc) {
      try { this._voiceSrc.stop(); } catch (_) { /* 已停 */ }
      this._voiceSrc = null;
    }
    for (const cue of cues) {
      if (seq !== this._voiceSeq) return;
      const buf = await this.loadCue(cue);
      if (seq !== this._voiceSeq) return;
      if (!buf) continue;
      await new Promise((resolve) => {
        const src = this.ctx.createBufferSource();
        src.buffer = buf;
        src.connect(this.ctx.destination);
        src.onended = resolve;
        this._voiceSrc = src;
        src.start();
      });
      if (seq !== this._voiceSeq) return;
      await new Promise((r) => setTimeout(r, VOICE_GAP_MS));
    }
  }
}
