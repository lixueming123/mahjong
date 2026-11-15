// §5.18 回放帧 → Promise 的拼装验收（web/js/client.js 的 requestReplay/handleReplayFrame）。
//
// replay_payload.mjs 验的是"载荷 → 文本"这一个函数，这里验的是它外面那一圈：
// 分片乱序到达、`encoding` 只在第一片出现、失败帧、以及 v2.6 之前那种明文应答。
// 这一层最容易出的错是"改了异步之后忘了返回"—— 载荷解压是异步的，Promise 必须等到它结束
// 才 resolve，否则调用方拿到的是一份还没解压的 base64。
//
// 运行：node game/tests/web/client_replay.mjs

import { gzipSync } from 'node:zlib';
import { Buffer } from 'node:buffer';

// ---- 最小 DOM（client.js 的构造要它）----
const mk = () => ({
  textContent: '', disabled: false, hidden: false, dataset: {}, style: {},
  classList: { toggle() {}, add() {}, remove() {} },
  addEventListener() {},
});
globalThis.document = {
  getElementById: () => mk(),
  querySelector: () => mk(),
  querySelectorAll: () => [],
};
globalThis.window = globalThis.window || { addEventListener() {}, removeEventListener() {} };
globalThis.performance = globalThis.performance || { now: () => Date.now() };

const { MahjongClient } = await import('../../web/js/client.js');
const { FRAME_TYPE } = await import('../../web/js/protocol_enums.js');

let pass = 0;
let fail = 0;
const check = (name, ok, detail = '') => {
  if (ok) {
    pass++;
    console.log(`  ok   ${name}`);
  } else {
    fail++;
    console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
  }
};

const REPLAY = {
  format: 'mahjong-replay',
  version: 2,
  room_id: 'r-2o5endvwb8zcx',
  seats: [],
  steps: [{ t: 0, f: [] }],
};

/** 造一个只带 `handleReplayFrame` 所需的客户端：不建 socket、不连网。 */
function makeClient() {
  const client = Object.create(MahjongClient.prototype);
  client._replayPending = null;
  client.net = { connected: true };
  return client;
}

/** 发一次请求并返回 Promise（绕过 send，直接建 pending 状态，与 requestReplay 末段等价）。 */
function beginRequest(client, roomId) {
  return new Promise((resolve, reject) => {
    client._replayPending = { roomId, chunks: null, received: 0, resolve, reject };
  });
}

/** 服务端 v2.6 的一条应答：gzip+base64、按 size 切开的若干片，`encoding` 只在第一片带。 */
function gzipFrames(roomId, payload, size) {
  const encoded = gzipSync(Buffer.from(payload, 'utf8'), { level: 6 }).toString('base64');
  const frames = [];
  for (let offset = 0; offset < encoded.length; offset += size) {
    frames.push(encoded.slice(offset, offset + size));
  }
  if (!frames.length) frames.push('');
  return frames.map((data, index) => ({
    type: FRAME_TYPE.REPLAY,
    room_id: roomId,
    chunk: index,
    chunks: frames.length,
    ...(index === 0 ? { encoding: 'gzip' } : {}),
    data,
  }));
}

async function main() {
  console.log('client_replay: gzip 应答');

  // 1. 顺序到达。
  {
    const client = makeClient();
    const promise = beginRequest(client, REPLAY.room_id);
    for (const frame of gzipFrames(REPLAY.room_id, JSON.stringify(REPLAY), 64)) {
      client.handleReplayFrame(frame);
    }
    const got = await promise;
    check('顺序到达能还原', got?.room_id === REPLAY.room_id && got?.steps?.length === 1);
  }

  // 2. 乱序到达。服务端按序发，但 Net 之上没有任何顺序保证值得依赖；拼装按 chunk 下标。
  {
    const client = makeClient();
    const promise = beginRequest(client, REPLAY.room_id);
    const frames = gzipFrames(REPLAY.room_id, JSON.stringify(REPLAY), 64);
    for (const frame of [...frames].reverse()) {
      client.handleReplayFrame(frame);
    }
    const got = await promise;
    check('乱序到达能还原', got?.room_id === REPLAY.room_id);
  }

  // 3. 多片：确认真的等到最后一片才 resolve（少了这一条，"忘了返回"也能过）。
  {
    const client = makeClient();
    const promise = beginRequest(client, REPLAY.room_id);
    const frames = gzipFrames(REPLAY.room_id, JSON.stringify(REPLAY), 24);
    check('夹具确实是多片', frames.length > 1, `${frames.length} 片`);
    let settled = false;
    promise.then(() => { settled = true; }, () => { settled = true; });
    for (const frame of frames.slice(0, -1)) {
      client.handleReplayFrame(frame);
    }
    await new Promise((resolve) => setTimeout(resolve, 20));
    check('最后一片之前不 resolve', !settled);
    client.handleReplayFrame(frames[frames.length - 1]);
    const got = await promise;
    check('补齐后 resolve', got?.room_id === REPLAY.room_id);
  }

  console.log('client_replay: 明文应答（v2.6 之前）');
  {
    const client = makeClient();
    const promise = beginRequest(client, REPLAY.room_id);
    const text = JSON.stringify(REPLAY);
    client.handleReplayFrame({
      type: FRAME_TYPE.REPLAY, room_id: REPLAY.room_id, chunk: 0, chunks: 1, data: text,
    });
    const got = await promise;
    check('没有 encoding 的帧仍按明文解析', got?.room_id === REPLAY.room_id);
  }

  console.log('client_replay: 失败路径');
  {
    const client = makeClient();
    const promise = beginRequest(client, REPLAY.room_id);
    client.handleReplayFrame({ type: FRAME_TYPE.REPLAY, room_id: REPLAY.room_id, error: 3 });
    let message = '';
    try {
      await promise;
    } catch (err) {
      message = err.message;
    }
    check('error 帧 reject 成"没有回放"', /没有回放/.test(message), message);
  }

  {
    const client = makeClient();
    const promise = beginRequest(client, REPLAY.room_id);
    // 第一片声明 gzip，第二片是垃圾：解压必须 reject，而不是 resolve 一份坏文本。
    client.handleReplayFrame({
      type: FRAME_TYPE.REPLAY, room_id: REPLAY.room_id, chunk: 0, chunks: 2, encoding: 'gzip', data: 'H4sIAAAA',
    });
    client.handleReplayFrame({
      type: FRAME_TYPE.REPLAY, room_id: REPLAY.room_id, chunk: 1, chunks: 2, data: 'not-gzip',
    });
    let rejected = false;
    try {
      await promise;
    } catch (err) {
      rejected = true;
    }
    check('解压失败会 reject', rejected);
  }

  {
    const client = makeClient();
    const promise = beginRequest(client, REPLAY.room_id);
    client.handleReplayFrame({
      type: FRAME_TYPE.REPLAY, room_id: 'r-2o5endvwb8zcy', chunk: 0, chunks: 1, data: '{}',
    });
    // 不属于本次请求的 room_id 被忽略：既不 resolve 也不 reject。
    let settled = false;
    promise.then(() => { settled = true; }, () => { settled = true; });
    await new Promise((resolve) => setTimeout(resolve, 20));
    check('别的房间的帧被忽略', !settled);
  }

  console.log(`\nclient_replay: ${pass} passed, ${fail} failed`);
  if (fail > 0) process.exitCode = 1;
}

await main();
