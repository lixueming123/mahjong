// 回放载荷还原（web/js/replay_payload.js）的验收。
//
// 这条代码是 §5.18 v2.6 的客户端一半：服务端把整份回放用 gzip 压好、base64 之后切开发，
// 客户端拼回来、解压、parse。它出错的表现是"回放打不开"，而原因可能在任何一层
// （base64 少一个字符 / gzip 魔数不对 / 分片顺序拼错），所以这里把三层都单独钉一遍。
//
// 夹具用 Node 的 zlib 现造，**不**读仓库里录好的回放：这里要验的是编解码本身，
// 而不是某一份历史文件长什么样。反过来，replay_e2e_test.py 验的是真服务端产物，
// 两者合起来才说明"服务端压的 = 浏览器能解的"。
//
// 运行：node game/tests/web/replay_payload.mjs

import { gzipSync } from 'node:zlib';
import { Buffer } from 'node:buffer';
import { joinReplayPayload } from '../../web/js/replay_payload.js';

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

/** 服务端那条路径：JSON 文本 → gzip → base64。 */
const encodeLikeServer = (text) => gzipSync(Buffer.from(text, 'utf8'), { level: 6 }).toString('base64');

const SAMPLE = JSON.stringify({
  format: 'mahjong-replay',
  version: 2,
  room_id: 'r-2o5endvwb8zcx',
  seats: [
    { seat: 0, player_id: 'u-42', name: '小明', is_bot: false, account_id: 42 },
    { seat: 1, player_id: 'bot-1', name: 'AI-1', is_bot: true, account_id: 0 },
  ],
  steps: [{ t: 0, f: [[[0, 1], { type: 20, remaining: 83 }]] }],
});

async function main() {
  console.log('replay_payload: 明文（旧服务端 / 旧文件）');
  check('空 encoding 原样返回', (await joinReplayPayload(SAMPLE, '')) === SAMPLE);
  check('undefined encoding 原样返回', (await joinReplayPayload(SAMPLE, undefined)) === SAMPLE);

  console.log('replay_payload: gzip + base64');
  const encoded = encodeLikeServer(SAMPLE);
  check('base64 只含 ASCII', /^[A-Za-z0-9+/]*={0,2}$/.test(encoded), encoded.slice(0, 40));
  const decoded = await joinReplayPayload(encoded, 'gzip');
  check('还原出原始 JSON 文本', decoded === SAMPLE);
  check('还原出的文本可 JSON.parse 且房间号对得上', JSON.parse(decoded).room_id === 'r-2o5endvwb8zcx');

  // 大一点的载荷：真实的回放是 1.2 MB 级别的，且分片是拼起来的整体。这里造一份 200 KB 的
  // 近似（重复的帧结构），确认解压这条路径不受长度影响。
  const big = JSON.stringify({
    format: 'mahjong-replay',
    version: 2,
    steps: Array.from({ length: 2000 }, (item, index) => ({
      t: index * 100,
      f: [[[0, 1, 2, 3], { type: 20, seq: index, hand: ['1m', '3m', '1p', '2p'] }]],
    })),
  });
  const bigEncoded = encodeLikeServer(big);
  check('大载荷确实被压小了', bigEncoded.length < big.length / 4,
    `${big.length} -> ${bigEncoded.length}`);
  check('大载荷完整还原', (await joinReplayPayload(bigEncoded, 'gzip')) === big);

  console.log('replay_payload: 失败路径');
  let unknownRejected = false;
  try {
    await joinReplayPayload(encoded, 'brotli');
  } catch (err) {
    unknownRejected = /不支持的回放编码/.test(err.message);
  }
  check('未知编码明确报错，而不是返回乱码', unknownRejected);

  let corruptRejected = false;
  try {
    // 合法 base64、合法 gzip 头，但内容被截断：必须 reject 而不是返回半截文本。
    await joinReplayPayload(encoded.slice(0, Math.floor(encoded.length / 2)), 'gzip');
  } catch (err) {
    corruptRejected = true;
  }
  check('截断的 gzip 流报错', corruptRejected);

  console.log(`\nreplay_payload: ${pass} passed, ${fail} failed`);
  if (fail > 0) process.exitCode = 1;
}

await main();
