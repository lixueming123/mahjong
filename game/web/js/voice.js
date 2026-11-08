/**
 * 结算播报语音：id 选取口径移植自 client/broadcast.py（日麻侧）。
 *
 * 六段结构里日麻实际可念的只有三段：和牌方式 → 番种明细 → 合计番数。
 * classic 的加成/封顶/倍率/收付整句在日麻语义不成立（庄家加番、6 番封顶、
 * 2^番 倍率都不存在），与 pygame 同口径一律不念。
 *
 * 资产复用规则（「宁可语音有损、不能念错番值」）：
 *  - 役名与番值**都**与 classic 资产一致才复用（如断幺九一番、对对和两番）；
 *  - 日麻专属役用 tools/gen_riichi_vo.py 生成的带番值后缀资产；
 *  - 宝牌类（dora/ura_dora/aka_dora）不是役，不念。
 */

/** 实测时长表（ms）：classic 57 条移植 vo_durations.py，日麻 18 条来自 gen_riichi_vo.py。 */
export const VOICE_MS = {
  // ---- 开场：和牌方式（3）
  vo_mode_tsumo: 488,
  vo_mode_ron: 562,
  vo_mode_draw: 573,
  // ---- 段 1：番种整句（classic 16 + 日麻 18）
  vo_yaku_menzen: 1328,
  vo_yaku_tanyao: 1165,
  vo_yaku_pinfu: 1037,
  vo_yaku_iipeikou: 1162,
  vo_yaku_rinshan: 1531,
  vo_yaku_yakuhai_1: 966,
  vo_yaku_yakuhai_2: 1038,
  vo_yaku_yakuhai_3: 1038,
  vo_yaku_sanshoku_doujun: 1638,
  vo_yaku_ittsuu: 1520,
  vo_yaku_toitoi: 1290,
  vo_yaku_sanankou: 1360,
  vo_yaku_honitsu: 1255,
  vo_yaku_shousangen: 1388,
  vo_yaku_seven_pairs: 1306,
  vo_yaku_pure_one_suit: 1236,
  vo_yaku_riichi_1: 945,
  vo_yaku_ippatsu_1: 909,
  vo_yaku_tsumo_1: 1769,
  vo_yaku_chankan_1: 1050,
  vo_yaku_haitei_1: 1331,
  vo_yaku_houtei_1: 1389,
  vo_yaku_sanshoku_doujun_1: 1558,
  vo_yaku_ittsuu_1: 1455,
  vo_yaku_chanta_1: 1787,
  vo_yaku_chanta_2: 1880,
  vo_yaku_junchan_2: 1912,
  vo_yaku_junchan_3: 1935,
  vo_yaku_honitsu_3: 1265,
  vo_yaku_chinitsu_6: 1259,
  vo_yaku_ryanpeikou_3: 1340,
  vo_yaku_honroutou_2: 1288,
  vo_yaku_sankantsu_2: 1337,
  vo_yakuman: 456,
  // ---- 段 2：合计番数（0..6；>6（役满 13 番）不念，避免把 13 念成 6）
  vo_han_0: 515,
  vo_han_1: 950,
  vo_han_2: 1037,
  vo_han_3: 1043,
  vo_han_4: 991,
  vo_han_5: 968,
  vo_han_6: 988,
  // ---- 段 6：荒庄（classic 口径，日麻流局只念 vo_mode_draw）
  vo_draw_no_score: 1149,
  vo_draw_renchan: 1055,
};

/** 与 vo_han_N 资产对齐的合计番数上限 */
const HAN_CUE_MAX = 6;

/** 句间停顿（ms）：TTS 资产本身已裁静音，留一口气 */
export const VOICE_GAP_MS = 170;

/**
 * 日麻役种 → 语音 cue：
 *  - 值为 [cue, han] 且 han 与服务端下发的番值相等才念（classic 复用资产）；
 *  - 值为函数时按番值查日麻专属资产（gen_riichi_vo.py 生成的 _N 后缀）；
 *  - 宝牌类（dora/ura_dora/aka_dora）与役满种一律不在此表 —— 役满整手统一
 *    念「役满」一句（见 yakuman 处理），不逐条念错番值的 classic 资产。
 */
const RIICHI_YAKU_CUES = {
  riichi: ['vo_yaku_riichi_1', 1],
  ippatsu: ['vo_yaku_ippatsu_1', 1],
  tsumo: ['vo_yaku_tsumo_1', 1],              // 门前清自摸和（≠ vo_mode_tsumo「自摸」）
  rinshan: ['vo_yaku_rinshan', 1],            // 复用 classic「杠上开花一番」
  chankan: ['vo_yaku_chankan_1', 1],
  haitei: ['vo_yaku_haitei_1', 1],
  houtei: ['vo_yaku_houtei_1', 1],
  tanyao: ['vo_yaku_tanyao', 1],
  pinfu: ['vo_yaku_pinfu', 1],
  iipeikou: ['vo_yaku_iipeikou', 1],
  chiitoitsu: ['vo_yaku_seven_pairs', 2],     // id 不同、番值与文案一致，复用
  toitoi: ['vo_yaku_toitoi', 2],
  sanankou: ['vo_yaku_sanankou', 2],
  sankantsu: ['vo_yaku_sankantsu_2', 2],
  shousangen: ['vo_yaku_shousangen', 2],
  honroutou: ['vo_yaku_honroutou_2', 2],
  sanshoku_doujun: (han) => han === 2 ? 'vo_yaku_sanshoku_doujun'
    : han === 1 ? 'vo_yaku_sanshoku_doujun_1' : null,
  ittsuu: (han) => han === 2 ? 'vo_yaku_ittsuu' : han === 1 ? 'vo_yaku_ittsuu_1' : null,
  chanta: (han) => han === 2 ? 'vo_yaku_chanta_2' : han === 1 ? 'vo_yaku_chanta_1' : null,
  junchan: (han) => han === 3 ? 'vo_yaku_junchan_3' : han === 2 ? 'vo_yaku_junchan_2' : null,
  ryanpeikou: ['vo_yaku_ryanpeikou_3', 3],
  honitsu: (han) => han === 3 ? 'vo_yaku_honitsu_3' : han === 2 ? 'vo_yaku_honitsu' : null,
  chinitsu: (han) => han === 6 ? 'vo_yaku_chinitsu_6' : han === 5 ? 'vo_yaku_pure_one_suit' : null,
};

/** 役满 id 集合（riichi_rules.cc is_yakuman 同款）——整手只念一句「役满」 */
const YAKUMAN_IDS = new Set([
  'tenhou', 'chiihou', 'kokushi', 'kokushi_13', 'suuankou', 'suuankou_tanki',
  'daisangen', 'tsuuiisou', 'ryuuiisou', 'chinroutou', 'chuuren', 'chuuren_junsei',
  'suukantsu', 'shousuushii', 'daisuushii',
]);

/**
 * game_end → 语音 cue 序列。
 * @returns {{cues: string[], yakuman: boolean}} cues 顺序播放；yakuman 表示该手是否役满
 */
export function buildVoiceCues(frame) {
  const win = frame && typeof frame === 'object' ? frame.win : null;
  if (!win || typeof win !== 'object') {
    return { cues: ['vo_mode_draw'], yakuman: false };   // 荒庄：只念「荒庄」
  }
  const cues = [win.tsumo ? 'vo_mode_tsumo' : 'vo_mode_ron'];
  let yakuman = false;
  const yaku = Array.isArray(win.yaku) ? win.yaku : [];
  for (const row of yaku) {
    if (!row || typeof row.id !== 'string') continue;
    const han = Number.isInteger(row.han) ? row.han : null;
    if (YAKUMAN_IDS.has(row.id)) { yakuman = true; continue; }
    if (row.id === 'yakuhai') {
      // 箭刻/风牌按组数分三条（>3 归 3）
      cues.push(`vo_yaku_yakuhai_${Math.min(3, Math.max(1, han ?? 1))}`);
      continue;
    }
    const entry = RIICHI_YAKU_CUES[row.id];
    if (!entry || han == null) continue;
    const cue = typeof entry === 'function' ? entry(han) : (entry[1] === han ? entry[0] : null);
    if (cue && VOICE_MS[cue]) cues.push(cue);
  }
  if (yakuman) cues.push('vo_yakuman');
  // 合计番数：0..6 才有资产；役满（13 番）跳过，由 vo_yakuman 兜底
  const han = Number.isInteger(win.han) ? win.han : null;
  if (!yakuman && han != null && han >= 0 && han <= HAN_CUE_MAX) {
    cues.push(`vo_han_${han}`);
  }
  return { cues, yakuman };
}

/** 序列总时长（ms），供播报横幅决定展示窗口；未知 id 按 600ms 兜底 */
export function voiceDuration(cues) {
  return (cues || []).reduce((sum, cue) => sum + (VOICE_MS[cue] ?? 600) + VOICE_GAP_MS, 0);
}
