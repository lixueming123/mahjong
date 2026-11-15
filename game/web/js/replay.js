/**
 * 对局回放播放器（PROTOCOL v2.3 §5.18）。
 *
 * 设计上只做一件事：把服务端录下来的**原始帧**按顺序喂给正常的帧处理器。
 * 因此这里没有第二套渲染逻辑、也没有第二套状态归约 —— `client.handleFrame()` 就是
 * 唯一的归约入口（它本来就不认识 socket，帧从哪来对它无差别），牌桌、牌河、副立、
 * 结算页、音效全部照常工作。
 *
 * 三个刻意的决定：
 *   1. **不重放规则**。录制的是真实下发过的字节，所以规则改动不会让旧回放失效，
 *      回放也不需要重新算牌山、不需要 seed。
 *   2. **不消费 `action_request`**。回放里没有"窗口"这个概念（没人需要操作），
 *      该帧只用来定位时间轴（显示"某家思考中"）。
 *   3. **进度靠重放而不是快照**。`seekTo()` = 从头重新喂到目标步。状态机是纯函数，
 *      300 步的重放比维护每步的快照便宜得多，也永远不会和归约逻辑不一致。
 *      批量推进期间用 `app.suspendChanged()/resumeChanged()` 把渲染压成一次。
 *
 * 视角与透视：
 *   - `seat` 决定喂哪些帧（私有帧只发给对应座位）；
 *   - `steps[].v` 记的是每个座位**自己看到的私有视图**（麻将里就是门前手牌 + 副露）。
 *     没有 endpoint 的座位（机器人）服务端从不为它构建私有帧，所以这份轨道是线上帧
 *     推不出来的唯一来源；
 *   - 透视复用结算期的摊牌通道 `state.revealHands[view]`，`opponents.js` 已有的
 *     "按牌码换牌面"路径直接生效，渲染层零改动。
 *
 * 视图的内容对录制端与播放器都是不透明的（game.h 里说明理由）。这里之所以还认识
 * `hand`/`melds`，是因为下游渲染层就是麻将：换玩法要改的是 `_applySeatViews`，不是格式。
 */
import { FRAME_TYPE } from './protocol_enums.js';
import { MahjongClient } from './client.js';
import { toViewMeld } from './state.js';

/** 播放倍速档位（1 = 录制时的真实节奏） */
export const SPEEDS = [0.5, 1, 2, 4];

/** 只当作时间轴标记、不喂给状态机的帧。 */
const TIMELINE_ONLY = new Set([FRAME_TYPE.ACTION_REQUEST]);

export class ReplayPlayer {
  /**
   * @param {object} options
   * @param {object} options.app        createState() 的返回值
   * @param {object} options.ui         渲染层（可省略：逻辑测试不需要 DOM）
   * @param {object} options.sfx        音效（可省略）
   * @param {object} options.replay     §5.18 的文件对象（已 JSON.parse）
   * @param {number} [options.seat]     初始视角座位（协议座位，0..3）
   */
  constructor({ app, ui = null, sfx = null, replay, seat = 0 }) {
    if (!replay || replay.format !== 'mahjong-replay') {
      throw new Error('不是回放文件（缺少 format: "mahjong-replay"）');
    }
    if (Number(replay.version) > SUPPORTED_VERSION) {
      // 版本只增不改：未来格式可能有本播放器读不懂的字段，宁可明说也不猜。
      throw new Error(`回放格式版本 ${replay.version} 高于本播放器支持的 ${SUPPORTED_VERSION}`);
    }
    this.app = app;
    this.ui = ui;
    this.sfx = sfx;
    this.replay = replay;
    this.steps = Array.isArray(replay.steps) ? replay.steps : [];
    this.seats = Array.isArray(replay.seats) ? replay.seats : [];

    /** 当前视角的协议座位（不是屏幕座位 —— 屏幕座位由 state.mySeat 换算） */
    this.seat = Number.isInteger(seat) ? seat : 0;
    this.godMode = false;
    this.speed = 1;
    this.playing = false;
    /** 每消费完一拍就回调一次（控制条靠它同步进度，见 replaybar.js）。 */
    this.onTick = null;
    /**
     * 播到某个 `game_end` 时回调一次（局终）。播放器本身不画结算页 —— 那属于渲染层，
     * 由 main.js 接到 ui.showResult 上；这里的职责只是"告诉外面此刻是局终"。
     */
    this.onRoundEnd = null;
    /** 最近一次扫到的 `game_end` 帧，与它所在的步（`atRoundEnd` 的判据） */
    this._lastEndFrame = null;
    this._endStepIndex = -1;
    /** 已经喂到第几步（= 下一步的游标） */
    this.cursor = 0;
    /**
     * 每人的最新私有视图（`{hand, melds}`，来自 steps[].v）。服务端只说"这是该座位此刻看到
     * 的私有部分"，具体字段由玩法自己定；播放器只认麻将这一份（手牌 + 副露），因为渲染层是麻将的。
     * 这是透视与「切到没有连接（机器人）的座位」的唯一数据来源。
     */
    this._lastViews = [null, null, null, null];
    /** 时间轴上的毫秒位置（按倍速缩放前的原始时间） */
    this.clockMs = 0;
    this._timer = 0;
    this._lastFrameAt = 0;
    this._seeking = false;
    /** 每步的累计时间，用于二分定位（构造时算一次） */
    this._marks = this._buildMarks();
    /** 局边界（构造时扫一次）：每局从哪个 step 开始，以及它的局名 */
    this._rounds = this._buildRounds();

    /**
     * 帧处理完全复用联机会话那一条路径：`offline` 让 MahjongClient 不绑网络、不跑阶段计时器，
     * 但 `handleFrame` 与 state.js 的归约一模一样。出站帧一律丢弃（回放没有服务端）。
     */
    this.client = new MahjongClient({ app, ui, sfx, url: '', options: {}, offline: true });
    this.client.send = () => false;   // 回放里没有任何出站帧是合法的
    this.client.net.send = () => false;
    this.client.net.connected = false;
    /**
     * 回放里没有服务端可问：状态归约遇到"本地手牌对不上"时**不要**去置 `needsSync`。
     * 那个信号的意思是"丢帧了，拉一份权威状态回来"，而录制里唯一的权威就是这份文件本身。
     * 从 AI 视角重放时本家手牌在重放途中始终是空的（机器人没有私有 `state_sync`，手牌由
     * 视图轨道在整趟重放结束后补），照联机口径会刷几十条告警并留下一个永不消解的标记。
     */
    this.app.state.canResync = false;
    /**
     * 播报闸门（client.js 的 `narratable`）：状态归约照做，但结算语音与飘字只在
     * "正常速度自动播放"时出手。seek 是从第 0 步重放，路径上必然会重新喂到上一局的
     * `game_end` 与各条 `action_event` —— 那不是"现在发生的事"，不能重播一遍。
     */
    this.client.narrationGate = () => this.narratable;
  }

  // ---------------------------------------------------------------- 播放控制

  get durationMs() {
    return this._marks.length ? this._marks[this._marks.length - 1] : 0;
  }

  /** 是否该发声：只在「正常速度自动播放」时发声。跳转/单步/快进都不发声 ——
   *  一次 seek 会重放几百步，每步一声会把整局的音效糊成噪音。 */
  get audible() {
    return this.playing && !this._seeking && this.speed <= 1;
  }

  /**
   * 是否该**播报**（结算语音 + 飘字/横幅/轻提示）。
   *
   * 与 `audible` 只差倍速这一条：音效是"每一步一声"，快放时必须静音（否则糊成噪音）；
   * 播报一局只有一次（局终/鸣牌），快放照样是"现在发生的事"，所以仍要说出来。真正要禁的
   * 只有 seek —— 切视角、拖进度条、单步都是"从头重放已看过的部分"，路径上必然重新喂到
   * 上一局的 `game_end` 与各条 `action_event`：状态该按它们归约，播报却不能再触发一次，
   * 否则界面已跳到当前局，耳边还在念上一局的和牌，屏幕上还挂着上一局的局终飘字。
   */
  get narratable() {
    return this.playing && !this._seeking;
  }
  get finished() {
    return this.cursor >= this.steps.length;
  }

  /**
   * 光标是否正停在某个 `game_end` 上（即"现在应该显示结算页"）。
   *
   * 判据是**位置**而不是"刚收到帧"：暂停在这里、单步走到这里、拖动停在这里，三种情况
   * 都该看到结算；而继续播放、往前一步，就该收起。这样拖动进度条来回扫时不会闪。
   */
  get atRoundEnd() {
    if (!this._lastEndFrame) return false;
    // `_lastEndFrame` 是扫到 cursor 为止最后一个 game_end；只有当它恰好出现在**最后消费的
    // 那一步**时才算"停在局终"，否则是"这一局早就结束了，现在在局间"。
    return this._endStepIndex === this.cursor - 1;
  }

  /**
   * 当前播到第几局（`_rounds` 的下标）与该局标签。控制条用它显示"第 3/6 局 · 東2"。
   * 边界是「该局第一步的下标」，所以取最后一个 `index <= cursor` 的段。
   */
  get round() {
    const rounds = this._rounds;
    if (!rounds.length) return { index: 0, total: 0, label: '' };
    let at = 0;
    for (let i = 0; i < rounds.length; i += 1) {
      if (rounds[i].index <= this.cursor) at = i;
      else break;
    }
    return { index: at, total: rounds.length, label: rounds[at].label || '' };
  }

  play() {
    if (this.playing || this.finished) return false;
    this.playing = true;
    this._lastFrameAt = 0;
    this._timer = setInterval(() => this._advanceByClock(), TICK_MS);
    return true;
  }

  pause() {
    if (!this.playing) return false;
    this.playing = false;
    if (this._timer) clearInterval(this._timer);
    this._timer = 0;
    return true;
  }

  toggle() {
    return this.playing ? this.pause() : this.play();
  }

  setSpeed(speed) {
    const next = SPEEDS.includes(speed) ? speed : 1;
    this.speed = next;
    return next;
  }

  /** 单步前进：喂一步（不论时间），用于逐手复盘。 */
  stepForward() {
    if (this.finished) return false;
    // 单步是"用户主动换落点"，与 seek 同样：旧播报立刻收掉，这一步本身不发声。
    this._silence();
    this.app.suspendChanged();
    try {
      this._applyStep(this.steps[this.cursor]);
      this.cursor += 1;
      // 轨道改动必须发生在 resume 之前，否则这次渲染看不到补上的手牌/透视。
      this._applySeatViews();
    } finally {
      this.app.resumeChanged();
    }
    this.clockMs = this._clockAt(this.cursor);
    this.onTick?.();
    // 单步之后由落点决定结算页：走到局终就显示，再往前一步就收起。
    this._syncRoundEndOverlay();
    return true;
  }

  /**
   * 跳到某一步之前（0 = 开局前）。实现是「从头重放」：状态机没有回退能力，
   * 而重放几百步远比维护快照便宜，也保证永远不会与归约逻辑不一致。
   */
  seekTo(stepIndex) {
    const target = Math.max(0, Math.min(Math.floor(stepIndex) || 0, this.steps.length));
    this._seeking = true;
    // 落点在重放开始前定下：重放过程中 `narratable` 恒为 false（`_seeking`），所以下面
    // 喂进去的 `game_end` / `action_event` 只归约状态、不播报也不飘字。落点该显示的
    // 结算页由 `_syncRoundEndOverlay()` 按**位置**补上，与播报无关。
    this._silence();
    this.app.suspendChanged();
    try {
      this.app.reset();
      // 视角是回放自身的属性（不是对局历史），reset() 之后仍要是当前座位。
      this.app.state.mySeat = this.seat;
      // §6.2 的 seq 是**会话**状态，住在 client 里而不是 state 里：重放必须把它一起清掉，
      // 否则第二次 seek 的第一帧会因为 seq 比上次播到的地方小而被当成乱序丢掉。
      this.client.lastSeq = null;
      this.client.window = null;
      this.client.pending = false;
      this.client.frozen = false;
      this._lastViews = [null, null, null, null];
      this._feedRoomJoined();
      for (let index = 0; index < target; index += 1) {
        this._applyStep(this.steps[index], index);
      }
      this.cursor = target;
      // 同 stepForward()：轨道写入要在 resume 之前，才能随这次渲染一起生效。
      this._applySeatViews();
    } finally {
      this.app.resumeChanged();
      this._seeking = false;
    }
    this.clockMs = this._clockAt(target);
    this.onTick?.();
    // 跳转落点可能正好是局终（拖到某局末尾）—— 与单步同一套判据。
    this._syncRoundEndOverlay();
    return this.cursor;
  }

  /** 按时间轴比例跳转（进度条用）。 */
  seekToRatio(ratio) {
    const clamped = Math.max(0, Math.min(Number(ratio) || 0, 1));
    const target = clamped * this.durationMs;
    // 找到第一个时间大于 target 的步：marks 单调不减，停在那里即可（含等号表示该步已到期）。
    let index = 0;
    while (index < this._marks.length && this._marks[index] <= target) index += 1;
    return this.seekTo(index);
  }

  /**
   * 切视角。同一份文件对四个座位都成立（广播帧对谁都发，私有帧按座位过滤），
   * 所以切视角 = 用新座位把已看过的部分重放一遍，播放进度不变。
   */
  setSeat(seat) {
    const next = Math.max(0, Math.min(Math.floor(seat) || 0, 3));
    if (next === this.seat) return this.seat;
    this.seat = next;
    this.seekTo(this.cursor);
    return this.seat;
  }

  /**
   * 透视：把四家门前手牌写进 `state.revealHands`（结算页摊牌用的同一条通道），
   * 渲染层据牌码换牌面。关闭时清空，牌背恢复。
   */
  setGodMode(on) {
    this.godMode = !!on;
    this._applySeatViews();
    this.app.changed();
    return this.godMode;
  }

  /** 释放计时器（离开回放页时调用）。 */
  dispose() {
    this.pause();
    this._silence();
    // 牌桌要交回联机会话（大厅入口复用同一个 `app.state`）：恢复"可以拉权威状态"。
    this.app.state.canResync = true;
  }

  /**
   * 掐掉"已经在响/已经在显示"的播报：语音序列、大字横幅、轻提示。
   *
   * 与闸门（`narratable`）是一对：闸门防的是"重放时**新起**一条播报"，这里收的是
   * "切视角之前那条还没演完"。没有这一步，切视角后上一局的语音还会念完，飘字也会
   * 挂满它自己的 2.6 秒 —— 牌桌已经换了局次和视角，这两样都属于过去。
   */
  _silence() {
    this.sfx?.stopVoice?.();
    this.ui?.hideNarration?.();
  }

  /**
   * 按落点同步「结算页」的显示：停在局终就显示，离开就收起。
   *
   * 由 seekTo / stepForward 在状态落定之后调用（不能在 `_applyStep` 里调 —— 那里一局要
   * 走几十步，每次都画结算既慢又会闪）。播放推进时也在 `_advanceByClock` 之后调用。
   */
  _syncRoundEndOverlay() {
    if (!this.onRoundEnd) return;
    if (this.atRoundEnd) {
      this.onRoundEnd(this._lastEndFrame, { showing: true });
    } else {
      this.onRoundEnd(null, { showing: false });
    }
  }

  // ---------------------------------------------------------------- 内部

  /**
   * 每步的时间轴位置（毫秒，相对开局）：`marks[i]` 就是第 i 步的 `t`。
   *
   * 这里必须是「按步下标」的，不能在前面垫一个 0 ——`_advanceByClock` 拿 `marks[cursor]`
   * 判断第 cursor 步是否到期，垫一个 0 会让整体错开一步（第一步立刻播、之后每步都提前一拍）。
   */
  _buildMarks() {
    const marks = [];
    let last = 0;
    for (const step of this.steps) {
      const t = Number.isFinite(step?.t) ? step.t : 0;
      last = Math.max(last, t);   // 单调不减：录制的 t 有极小概率不严格递增
      marks.push(last);
    }
    return marks;
  }

  /**
   * 局边界：录制的 `game_start` 出现在哪一步，就是新一局的起点。
   *
   * `game_start` 是**按座位构造**的私有帧，所以同一局的四个座位各有一份，都在同一步里
   * （RoomActor 一步只交付一次 outbounds）。因此「这一步含有任一 game_start」= 新局起点，
   * 不需要按座位去重。
   *
   * 返回 `[{ index, label }]`，`index` 是该局第一步的下标。第一项恒为第 0 步（文件开头
   * 就是开局）；没有 game_start 的损坏文件退化成单局，进度条照样能用。
   */
  _buildRounds() {
    const rounds = [];
    for (let index = 0; index < this.steps.length; index += 1) {
      const frames = Array.isArray(this.steps[index]?.f) ? this.steps[index].f : [];
      for (const entry of frames) {
        const payload = entry?.[1];
        // 帧在文件里是直接拼接的对象；旧文件可能把帧包成字符串，两种都要能读。
        let frame = payload;
        if (typeof payload === 'string') {
          if (!payload) continue;
          try {
            frame = JSON.parse(payload);
          } catch (err) {
            continue;   // 坏帧在 _applyStep 里已经 warn 过，这里只是算边界
          }
        }
        if (frame?.type !== FRAME_TYPE.GAME_START) continue;
        rounds.push({ index, label: roundLabel(frame) });
        break;   // 一步里只算一次（四份同桌起点）
      }
    }
    // 文件开头没有 game_start（截断的录制）时，仍然给出一个从 0 开始的段，
    // 否则进度条会退化成"没有任何分段"的样子。
    if (!rounds.length || rounds[0].index !== 0) {
      rounds.unshift({ index: 0, label: rounds.length ? '开局' : '' });
    }
    return rounds;
  }

  /** 已消费 `cursor` 步之后，时间轴该停在哪（= 最后消费那一步的时刻）。 */
  _clockAt(cursor) {
    return cursor > 0 ? (this._marks[cursor - 1] || 0) : 0;
  }

  _advanceByClock() {
    const now = performance.now();
    const delta = this._lastFrameAt ? now - this._lastFrameAt : TICK_MS;
    this._lastFrameAt = now;
    // 倍速只影响"时间轴走多快"，帧本身是按时间轴顺序整步消费的，所以不会切出半帧。
    this.clockMs += delta * this.speed;
    // 1× 下每步约 1 秒而计时器 50ms 一拍，所以绝大多数拍是空拍：没有可消费的步就整段跳过，
    // 连 suspend 都不要（那会白重建一次场景，也更容易把配对写错）。
    if (this.finished || this._marks[this.cursor] > this.clockMs) {
      if (this.finished) this.pause();
      return;
    }
    this.app.suspendChanged();
    try {
      // 逐帧消费到时间轴当前位置。cursor 在这里才前进，_applyStep 与游标严格同步。
      while (!this.finished && this._marks[this.cursor] <= this.clockMs) {
        this._applyStep(this.steps[this.cursor]);
        this.cursor += 1;
      }
      // 手牌轨道（透视 / 补机器人手牌）必须落在批处理区间内：写在 resume 之后的话，
      // 这次渲染已经发生过了，改动要等下一次 changed 才可见。
      this._applySeatViews();
    } finally {
      // suspend 过就一定 resume —— 这是批处理的唯一不变量。条件式配对（只在"有消费"时
      // resume）会让 batchDepth 永久泄漏，之后所有 changed() 都被吞掉，画面再也不刷新。
      this.app.resumeChanged();
    }
    // 控制条的进度只有在"真的消费了一步"之后才有变化，空拍没必要通知。
    this.onTick?.();
    this._syncRoundEndOverlay();
    if (this.finished) this.pause();
  }

  /** 一步 = 该步所有发给我这个视角的帧，按录制顺序。`stepIndex` 只用于标记局终位置。 */
  _applyStep(step, stepIndex = this.cursor) {
    if (!step) return;
    // 视图轨道与帧无关：它记录的是"此刻每个座位自己的私有视图"（麻将 = 手牌 + 副露），
    // 按座位下标存，先更新再喂帧。
    if (Array.isArray(step.v)) {
      for (let seat = 0; seat < 4 && seat < step.v.length; seat += 1) {
        const view = step.v[seat];
        // 缺席（空串/空值）= 该座位这一步没变；对象 = 该座位最新的完整视图。
        if (view && typeof view === 'object') this._lastViews[seat] = view;
      }
    } else {
      // v1 文件：手牌与副露分两条轨道。保留读取，旧录制不能被一次格式收紧作废。
      if (Array.isArray(step.h)) {
        for (let seat = 0; seat < 4 && seat < step.h.length; seat += 1) {
          const text = step.h[seat];
          if (typeof text !== 'string' || !text) continue;
          this._lastViews[seat] = { ...(this._lastViews[seat] || {}), hand: text.split(',') };
        }
      }
      if (Array.isArray(step.m)) {
        for (let seat = 0; seat < 4 && seat < step.m.length; seat += 1) {
          const melds = step.m[seat];
          if (!Array.isArray(melds)) continue;
          this._lastViews[seat] = { ...(this._lastViews[seat] || {}), melds };
        }
      }
    }
    for (const entry of step.f || []) {
      if (!Array.isArray(entry) || entry.length < 2) continue;
      const [recipients, payload] = entry;
      // §5.18 的帧是**直接拼接进文件**的对象（不包成字符串，见 replay.cc），所以这里通常是
      // 现成的对象。字符串分支留着是为了读旧版本文件与手写夹具 —— 两种形状都接受。
      let frame = payload;
      if (typeof payload === 'string') {
        if (!payload) continue;
        try {
          frame = JSON.parse(payload);
        } catch (err) {
          console.warn('回放里的帧无法解析，已跳过', payload.slice(0, 120), err.message);
          continue;
        }
      }
      if (!frame || typeof frame !== 'object' || TIMELINE_ONLY.has(frame.type)) continue;
      // 新的一局（本局的第一帧）：视图轨道是**本局**的快照，跨局必须清空，否则
      // `_applySeatViews` 会把上一局结束时的门前牌当成这一局的（表现为换局后本家手牌
      // 张数停在旧值）。同理，上一局的局终位置不再代表"当前停在局终"，一起清掉 ——
      // 否则跨过边界后 `atRoundEnd` 仍会认为还停在上局的 game_end 上。
      if (frame.type === FRAME_TYPE.GAME_START) {
        this._lastViews = [null, null, null, null];
        this._lastEndFrame = null;
        this._endStepIndex = -1;
      }
      const addressed = Array.isArray(recipients) && recipients.includes(this.seat);
      if (!addressed) {
        // `game_start` 是按座位构造的（`for_seat` 决定手牌与 seat 字段），而机器人座位没有
        // endpoint，服务端从不为它构造这一帧 —— 录下来的那几份都发给了别的座位。可是重放
        // 需要它来「重置本局」：没有它，牌河与局次会跨局累加（切到 AI 视角就会看到四家各
        // 18 张牌河）。
        //
        // 其余帧不借用。`state_sync` 也从不发给机器人，所以 AI 视角下余牌数（`remaining`）
        // 会停在 game_start 的初值 —— 这份信息在录制里确实不存在，轨道里也没有，与其编一个
        // 数字不如让它缺失。
        if (frame.type !== FRAME_TYPE.GAME_START) continue;
        frame = { ...frame, seat: this.seat, hand: [] };
      }
      // §6.2 的 seq 单调性在回放里同样成立（录的就是真实顺序），所以不必绕过 acceptSeq。
      this.client.handleFrame(frame);
      // 局终：结算页归渲染层画，这里只记位置。**不在这里回调** —— 一局要消费几十步，
      // 每步都画结算既慢又会闪；显示与否统一由 `_syncRoundEndOverlay()` 按落点决定。
      if (frame.type === FRAME_TYPE.GAME_END) {
        this._lastEndFrame = frame;
        this._endStepIndex = stepIndex;
      }
    }
  }

  /** 合成 `room_joined`：座位名要靠它（正常会话里这一帧在开局前到过一次）。 */
  _feedRoomJoined() {
    this.client.handleFrame({
      type: FRAME_TYPE.ROOM_JOINED,
      room_id: this.replay.room_id || '',
      seat: this.seat,
      seats: this.seats.map((seat) => ({
        seat: seat.seat,
        name: seat.name || '',
        is_bot: !!seat.is_bot,
      })),
    });
  }

  /**
   * 用文件里的视图轨道补三件事，都只在这份录制**有**轨道时才做：
   *
   *   1. **本家手牌**：没有 endpoint 的座位（机器人）服务端从不给它发 `state_sync`，所以从
   *      它自己的视角重放会得到一副空手牌。轨道记的正是它当时看到的那一份，这里补回去 ——
   *      否则「切到任一 AI 视角」就是一块空牌桌，而这个功能最大的用处恰恰是复盘别人的选择。
   *   2. **另外三家的牌背张数**：同理，`hands[1..3]` 的 `'back'` 占位也来自各座位的私有
   *      `state_sync`；只有真人座位有 wire，所以从 AI 视角看另外三家会一张牌都不画。
   *      轨道里有张数，补齐牌背即可（牌面只在透视时才给）。
   *   3. **透视**：四家门前牌写进 `state.revealHands`（结算页摊牌用的同一条渲染通道）。
   *
   * `_lastViews` 是"扫到 cursor 为止每人的最新私有视图"（在 `_applyStep` 里滚动维护），
   * 所以这里是常数时间。整条轨道缺席（玩法没有提供视图）时什么都不做。
   *
   * 视图里的字段由玩法自己决定；这里只认麻将这一份（`hand` / `melds`），因为下游的渲染层
   * 是麻将的。换一个玩法，改的是这个函数，不是录制格式。
   */
  _applySeatViews() {
    const state = this.app.state;
    if (!this._lastViews.some(Boolean)) return;  // 没有轨道：保持线上帧的样子，不伪造
    const mySeat = state.mySeat ?? 0;
    const hands = this._lastViews.map((view) => (Array.isArray(view?.hand) ? view.hand : null));

    for (let seat = 0; seat < 4; seat += 1) {
      const tiles = hands[seat];
      if (!tiles) continue;
      const view = ((seat - mySeat) + 4) % 4;
      if (view === 0) {
        // 本家手牌只补空，不覆盖：`state_sync` 是这一步的权威手牌，而轨道是"滚到这一步
        // 为止最后一次变化的值"，两者可能差一步（delta 编码：缺席表示没变）。用轨道
        // 覆盖会让下一张 discard_event 找不到牌，进而判 needsSync。
        if (state.hands[0].length === 0) {
          state.hands[0] = tiles;
        }
      } else {
        // 另外三家：只补牌背占位。张数必须跟着轨道走（每摸一张、每打一张都在变），
        // 所以不能只在"当前为空"时填一次 —— 那样张数会永远停在切视角那一刻。
        state.hands[view] = Array(tiles.length).fill('back');
      }
    }
    // 张数与手牌必须同源：渲染层读 `hands` 摆牌，UI 读 `handCounts` 显示数字。
    // 轨道缺货的座位（`continue` 跳过的那几家）也要对齐 —— 否则 `state_sync.seats[]`
    // 留下的旧张数会和一个空手牌同时存在，牌桌上什么都不画、数字却是 13。
    state.handCounts = state.hands.map((hand) => hand.length);

    this._applyMeldTracks();

    // 透视
    if (!this.godMode) {
      if (state.revealHands) {
        state.revealHands = null;
        state.revealWinner = null;
      }
      return;
    }
    const revealed = [null, null, null, null];
    for (let seat = 0; seat < 4; seat += 1) {
      const tiles = hands[seat];
      if (!tiles) continue;
      // `revealHands` 按**屏幕**座位索引（本家恒 0），所以按协议座位旋转一次。
      revealed[((seat - mySeat) + 4) % 4] = tiles;
    }
    state.revealHands = revealed;
    state.revealWinner = null;  // 透视不是"和牌者高亮"，别抢结算页的金条
  }

  /**
   * 用视图轨道里的副露补 `state.melds`。
   *
   * 副露住在 `state_sync.seats[].melds` 里，而那是**按座位私有的帧** —— 没有 endpoint 的
   * 座位服务端从不为它构建，所以从 AI 视角重放时四家的副露全没了（牌桌上空空如也，
   * 但牌河是对的）。轨道是这些信息的唯一来源。
   *
   * 屏幕座位（本家恒 0）才是渲染层的索引，所以协议座位要旋转一次。
   * 横放那张用 `state.meldClaims` —— 它在 `action_event` 里逐步记下来，切视角重放时同样适用。
   */
  _applyMeldTracks() {
    // 没有副露轨道（旧文件，或玩法没有副露）时不做任何事：线上帧已经有权威副露，
    // 覆盖只会更糟。
    if (!this._lastViews.some((view) => Array.isArray(view?.melds) && view.melds.length)) return;
    const state = this.app.state;
    const mySeat = state.mySeat ?? 0;
    const rebuilt = [];
    for (let seat = 0; seat < 4; seat += 1) {
      const own = this._lastViews[seat]?.melds;
      if (!Array.isArray(own) || !own.length) continue;
      const view = ((seat - mySeat) + 4) % 4;
      const claims = state.meldClaims?.[view] || [];
      own.forEach((meld, index) => {
        if (!meld || !Number.isInteger(meld.type)) return;
        rebuilt.push(toViewMeld(view, meld, claims[index] ?? null));
      });
    }
    state.melds = rebuilt;
  }
}

/** 播放器支持的格式版本（§5.18）。v2 = `steps[].v`；v1 的 `h`/`m` 轨道仍然能读。 */
const SUPPORTED_VERSION = 2;

/** 计时器间隔：比 60 fps 粗，比一次渲染贵得多的重放循环省得多。 */
const TICK_MS = 50;

/** 局名用的风牌：`round_index / 4` 就是第几圈（東→南→西→北）。 */
const ROUND_WINDS = ['東', '南', '西', '北'];

/**
 * `game_start` → 进度条上的一小段标签（"東1"、"東2"…）。
 *
 * 只用 `round_index`：PROTOCOL §5.4 定义它是**整场里的局序号、0 基**，且**连庄不增加**
 * （连庄仍是同一局号）。所以圈风 = round_index / 4，局号 = round_index % 4 + 1：
 * 0→東1、1→東2、3→東4、4→南1。`dealer` 只是庄家座位号，不含"第几局"的信息 ——
 * 早先用它当局号，把東2 标成了南2。
 *
 * 连庄时两段标签相同（東1、東1），这是对的：那确实是同一局在重打。
 */
function roundLabel(frame) {
  const roundIndex = Number.isInteger(frame?.round_index) ? frame.round_index : null;
  if (roundIndex === null || roundIndex < 0) return '';
  const wind = ROUND_WINDS[Math.floor(roundIndex / 4)];
  if (!wind) return '';   // 超过一圈东风+南场（>8 局）就不标，免得写出不存在的风
  return `${wind}${(roundIndex % 4) + 1}`;
}
