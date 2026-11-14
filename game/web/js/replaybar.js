/**
 * 回放控制条：只做 DOM ↔ ReplayPlayer 的接线。
 *
 * 状态全部在 ReplayPlayer 里（play/pause/speed/cursor/seat/godMode），这里不复制一份 ——
 * 控制条每次变化都从播放器读回真值，所以拖动进度条、按步前进、播放到底之后按钮的显示
 * 永远和实际播放位置一致，不会有第二份"UI 状态"漂移。
 */
import { SPEEDS } from './replay.js';

/**
 * 局边界 → DOM 片段（纯函数，便于单测）。
 *
 * 「442 步」在时间轴上其实是一局一局拼起来的，一条光溜溜的滑轨看不出这个结构。这里按
 * `steps[]` 里 `game_start` 的位置画竖线 + 局号，拖动时能直接看到"这一局还有多长"。
 *
 * 两件事要注意：
 *   1. **竖线画在段的起点**（第 0 段除外 —— 那条线只会压住滑轨左端）；
 *   2. **标签要抽稀**：滑轨只有 320 设计像素，6 局挤在一起会糊成一片。按"每段宽度 >=
 *      minLabelPercent"决定画不画。被跳过的段仍然有竖线 —— 分段结构完整，只是不写字，
 *      这样不会让人误以为"跳局了"。首段与末段必画（标出两端）。
 *
 * @param {{index:number,label:string}[]} rounds
 * @param {number} totalSteps
 * @param {number} [minLabelPercent] 一段至少占多宽才配写字（% 于整条滑轨）
 */
export function roundMarksHtml(rounds, totalSteps, minLabelPercent = 12) {
  if (!Array.isArray(rounds) || !rounds.length || !totalSteps) return '';
  const parts = [];
  rounds.forEach((round, index) => {
    const percent = (round.index / totalSteps) * 100;
    const next = index + 1 < rounds.length
      ? (rounds[index + 1].index / totalSteps) * 100
      : 100;
    const width = next - percent;
    if (round.index > 0) {
      parts.push(`<i style="left:${percent.toFixed(3)}%"></i>`);
    }
    const first = index === 0;
    const last = index === rounds.length - 1;
    const roomy = width >= minLabelPercent || first || last;
    if (!round.label || !roomy) return;
    const cls = first ? ' class="first"' : (last ? ' class="last"' : '');
    parts.push(`<span${cls} style="left:${percent.toFixed(3)}%">${round.label}</span>`);
  });
  return parts.join('');
}

export class ReplayBar {
  constructor(player) {
    this.player = player;
    this.el = {
      root: document.getElementById('replay-bar'),
      play: document.getElementById('rb-play'),
      prev: document.getElementById('rb-prev'),
      next: document.getElementById('rb-next'),
      speed: document.getElementById('rb-speed'),
      progress: document.getElementById('rb-progress'),
      marks: document.getElementById('rb-marks'),
      pos: document.getElementById('rb-pos'),
      seat: document.getElementById('rb-seat'),
      god: document.getElementById('rb-god'),
      exit: document.getElementById('rb-exit'),
    };
    this.onExit = null;
    // 拖动进度条期间不覆盖滑块：播放器每 50 ms 回写一次会让拖动卡住。
    this._dragging = false;
    // 播放过程由播放器逐拍回调，否则进度数字会停在按下播放那一刻
    // （按钮只在 click 后 refresh，自动推进没有任何 DOM 更新）。
    player.onTick = () => this.refresh();
    this.bind();
    this.renderRoundMarks();
  }

  show() {
    if (this.el.root) this.el.root.hidden = false;
    this.refresh();
  }

  hide() {
    if (this.el.root) this.el.root.hidden = true;
  }

  /** 进度条上的局边界（一次性，构造时调用）。算法见 `roundMarksHtml`。 */
  renderRoundMarks() {
    const host = this.el.marks;
    if (!host) return;
    host.innerHTML = roundMarksHtml(this.player._rounds || [], this.player.steps.length);
  }

  /** 从播放器读回全部显示状态（唯一真值来源）。 */
  refresh() {
    const player = this.player;
    const total = player.steps.length;
    if (this.el.play) {
      // 播完显示重播符号：同一个按钮，一下就能从头再看
      this.el.play.textContent = player.playing ? '❚❚' : (player.finished ? '⟲' : '▶');
      this.el.play.classList.toggle('on', player.playing);
    }
    if (this.el.speed) this.el.speed.textContent = `${player.speed}×`;
    if (this.el.pos) {
      // 局内位置比全局步数有用得多：「第 3/6 局 · 東2 · 85/442」比光秃秃的 85/442 更能说明
      // 现在看到哪儿了。
      const round = player.round;
      const scope = round.total > 1
        ? `第 ${round.index + 1}/${round.total} 局${round.label ? ` ${round.label}` : ''} · `
        : '';
      this.el.pos.textContent = `${scope}${player.cursor} / ${total}`;
    }
    if (this.el.progress && !this._dragging) {
      const ratio = total === 0 ? 0 : player.cursor / total;
      this.el.progress.value = String(Math.round(ratio * 1000));
    }
    if (this.el.seat) this.el.seat.textContent = `视角 ${player.seat}`;
    if (this.el.god) this.el.god.classList.toggle('on', player.godMode);
    if (this.el.prev) this.el.prev.disabled = player.cursor <= 0;
    if (this.el.next) this.el.next.disabled = player.finished;
  }

  bind() {
    const click = (element, handler) => element?.addEventListener('click', (event) => {
      event.preventDefault();
      handler();
      this.refresh();
    });
    click(this.el.play, () => {
      // 播完之后再点 = 从头重播（不是"无反应"）
      if (this.player.finished) this.player.seekTo(0);
      this.player.toggle();
    });
    click(this.el.prev, () => this.player.seekTo(this.player.cursor - 1));
    click(this.el.next, () => this.player.stepForward());
    click(this.el.speed, () => {
      const at = SPEEDS.indexOf(this.player.speed);
      this.player.setSpeed(SPEEDS[(at + 1) % SPEEDS.length]);
    });
    click(this.el.seat, () => this.player.setSeat((this.player.seat + 1) % 4));
    click(this.el.god, () => this.player.setGodMode(!this.player.godMode));
    click(this.el.exit, () => this.onExit?.());

    this.el.progress?.addEventListener('pointerdown', () => { this._dragging = true; });
    this.el.progress?.addEventListener('pointerup', () => { this._dragging = false; this.refresh(); });
    this.el.progress?.addEventListener('input', () => {
      const ratio = Number(this.el.progress.value) / 1000;
      this.player.seekToRatio(ratio);
    });
    // 键盘：空格播放/暂停，左右单步 —— 与操作说明里的既有习惯一致（空格在牌桌是「过」，
    // 但回放里没有「过」这个概念，所以复用它做播放更自然）。
    this._onKey = (event) => {
      if (this.el.root?.hidden) return;
      const tag = event.target?.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA') return;
      if (event.code === 'Space') {
        event.preventDefault();
        this.el.play?.click();
      } else if (event.code === 'ArrowLeft') {
        event.preventDefault();
        this.player.seekTo(this.player.cursor - 1);
        this.refresh();
      } else if (event.code === 'ArrowRight') {
        event.preventDefault();
        this.player.stepForward();
        this.refresh();
      }
    };
    window.addEventListener('keydown', this._onKey);
  }

  dispose() {
    if (this._onKey) window.removeEventListener('keydown', this._onKey);
    this.hide();
  }
}
