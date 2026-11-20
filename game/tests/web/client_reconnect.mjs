// Web 客户端断线自动重连的行为测试（无浏览器、无 socket）。
//
// 覆盖「web 端挂机好久回来发现和服务器断开连接」这条链路的客户端一半：
//   1. 断线且凭据还在 ⇒ 自动调度重连，而不是弹个提示就停住；
//   2. 重连成功必须走 login{resume}，服务端才可能把原座还回来；
//   3. 切回前台时：socket 已死 ⇒ 立刻补连；socket 看似活着但入站已静默 ⇒ 主动换连接；
//   4. 对局中长时间没有实质帧 ⇒ 发 sync 探活，探针没回音就判定座位已丢并重连
//      （服务端的 20s 空闲摘掉的是座位，ping 的 pong 不经过房间，所以必须用 sync 探）；
//   5. 重连回来没拿到 in_room ⇒ 落回大厅，而不是卡在冻结的牌桌上，也不按 URL 凭据重建房；
//   6. 主动退出/返回大厅不触发自动重连。

globalThis.document = {
  hidden: false,
  addEventListener: () => {},
  querySelector: () => null,
  querySelectorAll: () => [],
  getElementById: () => null,
};
globalThis.window = { addEventListener: () => {} };

const { createState } = await import('../../web/js/state.js');
const { MahjongClient } = await import('../../web/js/client.js');
const { FRAME_TYPE } = await import('../../web/js/protocol_enums.js');
const { Lobby } = await import('../../web/js/lobby.js');

let pass = 0;
let fail = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a === e) {
    pass += 1;
    console.log(`  ok   ${name}`);
  } else {
    fail += 1;
    console.log(`  FAIL ${name}\n       got ${a}\n       exp ${e}`);
  }
};
const ok = (name, condition) => check(name, !!condition, true);

/**
 * 一个把 Net 换成假货的 client：记录 connect/close 调用与发出的帧。
 * 假 net.connect 成功时补发一帧 authFrame()，这与 bindNet 里 open 处理器的行为一致
 * （真实流程里 login 就是连上之后才发的）。
 */
function makeClient() {
  const app = createState();
  const client = new MahjongClient({ app, ui: null, sfx: null, url: 'ws://test', options: {} });
  const sent = [];
  let connects = 0;
  let closes = 0;
  client.net = {
    url: 'ws://test',
    connected: true,
    lastInbound: performance.now(),
    connect: async () => {
      connects += 1;
      client.net.connected = true;
      sent.push(client.authFrame());
    },
    close: () => { closes += 1; client.net.connected = false; },
    send: (frame) => { sent.push(frame); return true; },
    startHeartbeat: () => {},
    stopHeartbeat: () => {},
    on: () => {},
  };
  client.hint = () => {};
  client.applyButtons = () => {};
  return { app, client, sent, stats: () => ({ connects, closes }) };
}

const seats = [
  { seat: 0, name: 'Alice', is_bot: false },
  { seat: 1, name: 'AI-1', is_bot: true },
  { seat: 2, name: 'AI-2', is_bot: true },
  { seat: 3, name: 'AI-3', is_bot: true },
];

// ---------------------------------------------------------------- 1. 断线即自动重连
{
  const { client } = makeClient();
  client.credential = { user: 'alice', pass: 'pw' };
  client.room = { roomId: 'r-test', seat: 0, seats };
  client._hadRoom = true;
  ok('有凭据时允许自动重连', client.canReconnect());
  client.scheduleReconnect();
  ok('断线后进入重连态（不是停在 closed）', client.reconnecting);
  ok('排了一次重连定时器', client._reconnectTimer !== 0);

  const guest = makeClient().client;
  guest.credential = { name: '游客', user: '' };
  ok('游客也能自动重连（重建连接后回大厅）', guest.canReconnect());
  const none = makeClient().client;
  ok('没有凭据就不重连', !none.canReconnect());
}

// ---------------------------------------------------------------- 2. 重连走 login{resume}
{
  const { client, sent } = makeClient();
  client.credential = { user: 'alice', pass: 'pw' };
  // 上一次「返回大厅」把 resume 关掉了：断线重连必须把它改回来，否则服务端不还座位。
  client.resumeNext = false;
  await client.attemptReconnect();
  const login = sent.find((frame) => frame.type === FRAME_TYPE.LOGIN);
  ok('重连发出 login', !!login);
  check('重连的 login 不复用「不回旧房」的 resume:false', login.resume, undefined);
  check('重连后 resumeNext 复位', client.resumeNext, true);
  ok('重连标记为「这一次是断线恢复」', client._resumeAttempt);
}

// ---------------------------------------------------------------- 3. 回到前台
{
  // 3a. socket 已死：立刻重连（不等退避那一档）
  const { client } = makeClient();
  client.credential = { user: 'alice', pass: 'pw' };
  client.net.connected = false;
  const before = client.reconnecting;
  client.resumeAfterForeground();
  check('前台发现 socket 已死 → 转入重连', [before, client.reconnecting], [false, true]);

  // 3b. socket 活着但入站静默很久：主动换连接（服务端那边座位早被摘了）
  const silent = makeClient();
  silent.client.credential = { user: 'alice', pass: 'pw' };
  silent.client.net.lastInbound = performance.now() - 30000;
  silent.client.resumeAfterForeground();
  ok('前台发现入站静默 → 主动重连', silent.client.reconnecting);

  // 3c. 刚回过帧：什么都不做，别把好连接换掉
  const fresh = makeClient();
  fresh.client.credential = { user: 'alice', pass: 'pw' };
  fresh.client.net.lastInbound = performance.now();
  fresh.client.resumeAfterForeground();
  ok('刚有过入站帧 → 不折腾连接', !fresh.client.reconnecting);

  // 3d. 后台期间被节流的重连：回前台立刻补一次，不再等退避
  const throttled = makeClient();
  throttled.client.credential = { user: 'alice', pass: 'pw' };
  throttled.client.reconnecting = true;
  throttled.client._reconnectTimer = setTimeout(() => {}, 100000);
  const oldTimer = throttled.client._reconnectTimer;
  throttled.client.resumeAfterForeground();
  ok('补连时清掉被节流的退避定时器', throttled.client._reconnectTimer !== oldTimer);
  clearTimeout(oldTimer);
}

// ---------------------------------------------------------------- 4. 对局探活
{
  // 4a. 只有 pong：不算实质帧，静默够了就发 sync
  // 用真实帧把牌桌推到 playing（探活只在「已开局」时才有意义）。
  const startFrame = {
    type: FRAME_TYPE.GAME_START, seat: 0, dealer: 0, round_index: 1, round_count: 4,
    hand: ['1m', '2m', '3m', '4m', '5m', '6p', '7p', '8p', '9p', '1s', '1s', '2z', '2z'],
  };
  const { client, sent } = makeClient();
  client.room = { roomId: 'r-test', seat: 0, seats };
  client.handleFrame(startFrame);
  check('牌桌进入对局', client.state.phase, 'playing');
  client._lastGameFrameAt = performance.now() - 20000;
  client.handleFrame({ type: FRAME_TYPE.PONG });
  ok('pong 不刷新实质帧时间', performance.now() - client._lastGameFrameAt >= 19000);
  client.probeGameSeat(performance.now());
  ok('对局静默超阈值 → 发 sync 探活', sent.some((frame) => frame.type === FRAME_TYPE.SYNC));
  ok('发出探针后进入等待回音', client._probeSentAt !== 0);

  // 4b. 探针没有回音：判定座位已丢，主动断开走重连
  const lost = makeClient();
  lost.client.room = { roomId: 'r-test', seat: 0, seats };
  lost.client.handleFrame(startFrame);
  lost.client._lastGameFrameAt = performance.now() - 40000;
  lost.client._probeSentAt = performance.now() - 6000;
  lost.client.probeGameSeat(performance.now());
  ok('探针超时 → 断开（交给自动重连）', lost.stats().closes >= 1);

  // 4c. 有实质帧回来：探针状态清零，不再判定丢座
  const alive = makeClient();
  alive.client.room = { roomId: 'r-test', seat: 0, seats };
  alive.client.handleFrame(startFrame);
  alive.client._probeSentAt = performance.now() - 1000;
  alive.client._lastGameFrameAt = performance.now() - 40000;
  alive.client.handleFrame({ type: FRAME_TYPE.ROOM_UPDATE, room_id: 'r-test', seats });
  check('实质帧回来清掉探针', alive.client._probeSentAt, 0);
  alive.client.probeGameSeat(performance.now());
  check('刚收到实质帧就不该再判丢座', alive.stats().closes, 0);

  // 4d. 等待页/未开局不发探针（房间会回 INTERNAL，白送玩家一个错误弹窗）
  const waiting = makeClient();
  waiting.client.room = { roomId: 'r-test', seat: 0, seats };
  waiting.client._lastGameFrameAt = performance.now() - 40000;
  waiting.client.probeGameSeat(performance.now());
  ok('等待页不发探针', !waiting.sent.some((frame) => frame.type === FRAME_TYPE.SYNC));

  // 4e. 大厅/场终不探活（那里没有座位，也换不来游戏帧）
  const lobby = makeClient();
  lobby.client._lastGameFrameAt = performance.now() - 40000;
  lobby.client.probeGameSeat(performance.now());
  ok('不在房间时不发探针', !lobby.sent.some((frame) => frame.type === FRAME_TYPE.SYNC));
}

// ---------------------------------------------------------------- 5. 拿不回原座
{
  const { client } = makeClient();
  client.credential = { user: 'alice', pass: 'pw' };
  client.lobbyDriven = false;
  client.autoReady = true;
  client.room = { roomId: 'r-test', seat: 0, seats };
  client._resumeAttempt = true;
  client._hadRoom = true;
  client._resumeWasPlaying = true;
  let created = 0;
  client.createRoom = () => { created += 1; return true; };
  client.listRooms = () => true;

  client.handleFrame({ type: FRAME_TYPE.LOGIN_OK, player_id: 'u-1', name: 'Alice', in_room: false });
  check('拿不回原座 → 回大厅', client.stage, 'lobby');
  check('拿不回原座 → 不按 URL 凭据重建房', created, 0);
  check('拿不回原座 → 牌桌状态清空', client.app.state.phase, 'idle');
  ok('拿不回原座 → 允许进大厅选玩法', client.lobbyDriven);

  // 玩家主动登录（不是断线恢复）仍然保持 URL 一步到位
  const direct = makeClient().client;
  direct.credential = { user: 'alice', pass: 'pw' };
  direct.lobbyDriven = false;
  let directCreated = 0;
  direct.createRoom = () => { directCreated += 1; return true; };
  direct.handleFrame({ type: FRAME_TYPE.LOGIN_OK, player_id: 'u-1', name: 'Alice', in_room: false });
  check('主动登录仍走 URL 一步到位', directCreated, 1);
}

// ---------------------------------------------------------------- 6. 主动退出不重连
{
  const { client } = makeClient();
  client.credential = { user: 'alice', pass: 'pw' };
  client.logout();
  ok('退出登录后不自动重连', !client.canReconnect());
  client.scheduleReconnect();
  ok('退出登录后也没有排上重连', !client.reconnecting);

  const { client: back } = makeClient();
  back.credential = { user: 'alice', pass: 'pw' };
  back.listRooms = () => true;
  back.net.connect = async () => { back.net.connected = true; };
  back.returnToLobby();
  check('返回大厅清掉重连退避', [back.reconnecting, back._reconnectAttempt], [false, 0]);
}

// ---------------------------------------------------------------- 7. 大厅页不卡在「发牌中…」
{
  const { app, client } = makeClient();
  const lobby = new Lobby({ app, client, auto: false });
  let tip = '';
  lobby.el.roomTip = { textContent: '' };
  lobby.visible = true;
  lobby.roomReady = true;      // 模拟「已点开始对局，正在发牌」
  lobby.busy = true;
  app.emit('close', { reason: 'idle timeout in lobby' });
  tip = lobby.el.roomTip?.textContent || '';
  check('断线后「发牌中…」被替换掉', tip, '连接已断开，正在重连…');
  check('断线后开始按钮的 busy 锁释放', lobby.busy, false);
  check('断线后 roomReady 复位', lobby.roomReady, false);
}

client_cleanup();
function client_cleanup() {
  // 让进程能退出：makeClient 里没有真 socket，只有上面可能残留的退避定时器
  setTimeout(() => {
    console.log(`\nWeb 自动重连：${fail ? 'FAILED' : 'ALL PASS'}  ${pass} passed, ${fail} failed`);
    process.exit(fail ? 1 : 0);
  }, 10);
}
