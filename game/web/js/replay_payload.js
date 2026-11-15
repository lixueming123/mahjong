/**
 * 把 §5.18 拼好的回放载荷还原成 JSON 文本（PROTOCOL v2.6）。
 *
 * v2.3 的 `data` 就是回放文本本身，所以 `chunks.join('')` 之后直接 `JSON.parse` 就行。
 * v2.6 起服务端下发 **gzip + base64**：一份 1.5 MB 的回放压到 ~60 KB，切片数从 94 片降到
 * 6 片，代价是客户端要先 base64 解码再解压。`encoding` 字段说明用的是哪种：
 *   - 缺省/空串：明文切片（旧服务端，或一份 v2.3 时期写的旧文件）；
 *   - `"gzip"`：上面那套。
 *
 * 为什么整份解压而不是逐片：gzip 的字典跨帧，按 16 KB 分片各压一次会差 1.8 倍（实测
 * 63 KB → 115 KB），所以服务端是整份压完再切，这里也必须整份拼完再解。
 *
 * 解压走浏览器原生的 `DecompressionStream('gzip')`（Chrome 80+ / Firefox 113+ / Safari 16.4+），
 * 不引第三方库。它不可用时降级为报错而不是静默返回乱码——回放是只读历史，读不出来就该说
 * 读不出来。
 */

/** base64 → Uint8Array。`atob` 给的是 latin1 字符串，逐字节搬一次。 */
function decodeBase64(base64) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes;
}

/**
 * @param {string} payload  已按 chunk 顺序拼好的载荷
 * @param {string} encoding  §5.18 的 `encoding`（空串 = 明文）
 * @returns {Promise<string>} 回放 JSON 文本
 */
export async function joinReplayPayload(payload, encoding) {
  if (!encoding) return payload;
  if (encoding !== 'gzip') {
    // 未知编码明确拒绝：猜错会得到一个"看起来是 JSON"的乱码，比直接失败更难查。
    throw new Error(`不支持的回放编码：${encoding}`);
  }
  if (typeof DecompressionStream !== 'function') {
    throw new Error('当前浏览器不支持解压回放（需要 DecompressionStream）');
  }
  const compressed = decodeBase64(payload);
  // Blob → stream 是拿到一个可 pipe 的字节流的最短路径；Response 负责把流收集成文本。
  const stream = new Blob([compressed]).stream().pipeThrough(new DecompressionStream('gzip'));
  return new Response(stream).text();
}
