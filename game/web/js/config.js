/**
 * 1:1 复刻参考录像的布局常量。
 *
 * 设计坐标系固定 1920x1080（与录像一致），原点左上、y 向下。
 * Three 世界坐标用 P() 换算（y 翻转、原点居中），相机是正交正俯视，
 * 所以「设计像素」可以直接当「屏幕像素」用，不存在透视畸变。
 */

export const DESIGN = { W: 1920, H: 1080, CX: 960, CY: 540 };

/** 设计像素 -> Three 世界坐标 */
export function P(x, y) {
  return [x - DESIGN.CX, DESIGN.CY - y];
}
/** 设计长度 -> 世界长度（1:1） */
export const U = (v) => v;

/** 层级：全部 depthTest:false，靠 renderOrder 排前后 */
export const Z = {
  CLOTH: 0,
  FRAME: 2,
  WALL_SHADOW: 3,
  WALL: 4,
  BOARD_SHADOW: 5,
  BOARD: 6,
  RIVER_SHADOW: 7,
  RIVER: 8,
  HAND_SHADOW: 9,
  HAND: 10,
  FLY: 12,
};

export const COLOR = {
  clothDark: '#102a49',
  clothMid: '#224c7d',
  clothLight: '#2b5c92',
  clothLine: 'rgba(9,26,48,.55)',
  woodDark: '#2a0d0b',
  woodMid: '#5a2118',
  woodLight: '#7b3222',
  gold: '#e6a63c',
  goldDim: '#a87a2e',
  ivory: '#f6f1e4',
  ivoryLit: '#fcf9f0',
  wallOrange: '#f0a52a',
  wallTop: '#eef0ec',
  wallSide: '#f7f7f2',
  board: '#3a3b42',
  boardInner: '#565861',
  ink: '#12161d',
};

/** 中央八角情报板（录像实测 320x243，四家牌河紧贴它的四条边） */
export const BOARD = { x: 960, y: 418, w: 320, h: 240 };

/** 舍牌：竖牌（上下家）/ 横牌（左右家），橙金厚边永远朝屏幕下方 */
export const RIVER = {
  cols: 6,
  rows: 3,
  cap: 18,
  vW: 54, vH: 70,          // 上下家：竖立
  hW: 70, hH: 54,          // 左右家：横躺
  thick: 13,               // 牌身外侧下方的橙金厚边（录像实测约 13，不遮牌面）
  colGap: 56,              // 列距（含 2px 缝）
  rowGap: 64,              // 行距
  firstRowTop: 238,        // 上家第一行中心 y（含金边的牌底离板顶约 6px）
  firstRowBottom: 580,     // 下家第一行中心 y
  firstColLeft: 728,       // 左家第一列中心 x
  firstColRight: 1192,     // 右家第一列中心 x
  // 左右家牌河的第一张牌**顶边对齐板顶**（录像实测：舍牌从板顶一路往下码），
  // 不是围着板中心居中 —— 那样只打三四张时会浮在板子上方一大截。
  sideRowY0: 380,          // 左右家第 2 张（col=1）的中心 y
};

/**
 * 四家手牌统一布局（第一版口径）。
 *
 * 关键：一手牌 = n 张「同尺寸」的牌，从中心向两侧展开：
 *     t = (i - (n-1)/2) * step
 *     x = cx + cos(angle) * t,  y = cy + sin(angle) * t
 * step 是固定值，牌宽 w 也是固定值。13 张变 14 张时整排只变长 step，
 * 每张牌绝不会被拉伸 —— 这才是「一手牌」，不是一条被缩放的装饰带。
 *
 * 这里没有 x1/y1/x2/y2 线段端点：那会把整排宽度锁死，张数一变就缩放牌宽，
 * 正是 pygame 客户端 _draw_back_row / _blit_flat_orange 的做法。
 *
 * angle 是录像里量出的排布方向（侧家约 19°，不是 45°）。
 * h 是牌竖起来的高度、d 是牌厚；透视相机俯角约 47°，
 * 屏幕可见高度 ≈ h*cos47° + d*sin47°。
 *
 * 长宽比不能和本家共用一套，三家里对家和侧家也彼此不同：
 *  - 本家：正对镜头的完整牌面 90x150，0.60 竖长；
 *  - 对家：牌背小块，约 36x62，同样竖长，但只有本家 1/2.4 大，
 *    顶面露出一条窄象牙白（约 18%）；
 *  - 左右家：从侧面斜看，「橙牌背 + 白色侧面并排」的六边形斜块，
 *    偏方甚至横宽，白面约占 30%。
 * 屏幕可见高 = h*cos47° + d*sin47°，白面宽度由 d 决定。
 * 所以对家要「高而白条窄」，侧家要「矮而白侧面宽」。
 * w 保持上一轮加宽后的值不再缩回，避免又变窄。
 */
const DEG = Math.PI / 180;
export const SEAT_LAYOUT = {
  0: { cx: 888,  cy: 997, angle: 0,          step: 95,  w: 94, h: 150, d: 22 },
  // 右家：录像里整列占 y221~814（中心 524、12 步长约 612）。
  // 立牌会被透视抬高约 118px，所以 cy 要往下补到 642 才落在这个位置。
  1: { cx: 1690, cy: 642, angle: 71.3 * DEG, step: 51,  w: 54, h: 54,  d: 20 },
  2: { cx: 1105, cy: 43,  angle: 0,          step: 44,  w: 43, h: 44,  d: 20 },
  3: { cx: 260,  cy: 355, angle: 105.4 * DEG, step: 48, w: 47,  h: 52,  d: 18 },
};

/** 一手牌的张数：庄家 14、闲家 13；摸打之间始终维持这个值 */
export const HAND_SIZE = 13;

/** 牌身侧面（象牙白）/ 牌背橙金的基色，由真实光照产生明暗，不画渐变贴图 */
export const TILE_3D = {
  side: 0xe9ece7,
  sideDark: 0xb9c0ba,
  edge: 0x8b6a1c,
  backTop: '#ffd987',
  backMid: '#efa327',
  backBottom: '#b5680d',
};

/**
 * 本家立牌（按录像实测）：单牌 92x146、列距 95、13 张占 x224~1453，
 * 牌顶 y926 牌底 y1072。
 *
 * 关键：手牌是**左端固定**的（不是居中）。摸牌后多出来的那张贴在右边、还要
 * 上抬一截（录像实测：与上一张隔 33px、整体抬高 47px），打掉后 13 张回到原位，
 * 整排不会左右跳。手牌变短（副露）时同理，右端往左收，左端钉死。
 */
export const HAND = {
  left: 271,             // 第一张牌中心 x（左沿 224 + 半张牌宽）
  cy: 997,
  w: 94,                 // 录像里牌宽约 94、几乎紧贴
  h: 150,
  gap: 1,                // 布局列距 = w+gap = 95，1:1 坐标不动
  seam: 1,               // 牌面宽 = 列距 95，相邻牌边缘正好相接：零重叠、零缝
  lift: 26,                // 选中抬起
  count: 14,
  drawGap: 33,             // 摸到的那张与手牌之间的空档（录像实测）
  drawLift: 47,            // 摸到的那张上抬的高度（录像实测）
  side: 4,                 // 右侧露出的牌身厚度
  foot: 5,                 // 触底暗边高
  shadowDx: 6,
  shadowDy: -9,
};

/**
 * 副露（吃/碰/杠之后亮给另外三家看的牌）。
 *
 * 逐帧量录像（f35~f48 上家两组：2s3s4s + 8m7m6m，合计 6 张）得到：
 *   - 一条从手牌末端往桌角延伸的**连续斜带**，方向与上家手牌同一条线
 *     （约 107°，往下略偏左），不是垂直列、也不是水平行；
 *   - 每组三张里**只有被鸣的那张横放**（宽高互换、在带子里更长），
 *     另外两张照常立着 —— 高倍放大看得很清楚：
 *       2s3s4s 组：最靠手牌端 2s 图案**正读**（被鸣），3s、4s **侧读**；
 *       8m7m6m 组：最靠手牌端 8m 图案**正读**（被鸣），7m、6m **侧读**。
 *     图案侧读 = 牌面朝向这一家本人（上家坐左，本来就要转 90°），
 *     图案正读 = 在此基础上又转了 90°，也就是「横放」。
 *   - 组内相邻牌**轻微互压**（pitch 62 略小于牌身 66），连成一条斜带；
 *   - 整条带子略微倾斜（约 6°）：录像里牌的上缘不是水平的，
 *     那是透视收敛，正交相机画不出来，用一点 tilt 近似。
 *
 * f44 的 2s3s4s 组**叠在它上方**（更靠手牌那一侧）—— 副露区是**远端固定**：
 * 靠桌角那一头钉死（上家 y≈957），新的一组往手牌方向长。
 *
 * 牌的**长边永远指向「远离自己」**：本家/对家是竖牌（94x150、48x62），
 * 左右家是横牌（86x64）。
 *
 * 坐标语义：far 是这一家副露区的**远端**（离手牌最远的那条边中点），angle 是
 * 「由近到远」的方向；本家没有 far —— 它贴着会伸缩的手牌右端，从手牌往外排。
 *
 * swap：被鸣牌宽高互换 = 麻将规则里的「横放」（占位更长、图案再转 90°）。
 * 四家一律 true —— 这是唯一正确的规则表达，不分座位。
 */
export const MELD = {
  seat: {
    // 本家：手牌右端外侧，沿 +x 排开（起点按手牌末端实时算）
    0: {
      far: null, right: 1760, y: 997, angle: 0, w: 94, h: 150, rot: 0, d: 20, thick: 13, side: 7,
      tilt: 0, gap: 11, seam: 1, swap: true,
    },
    // 右家：手牌上端往右上角摊开（沿自己牌列的同一条斜线，和上家镜像）
    1: {
      far: { x: 1600, y: 250 }, angle: 251.3 * DEG, w: 84, h: 52, rot: 1, d: 18, thick: 7, side: 5,
      tilt: -18.7 * DEG, gap: 16, seam: -3, swap: true, pinGap: 52, maxLen: 470,
    },
    // 对家：牌河**右侧**沿同一条水平带摊开（他们的左手边）。
    // 旧位置 far{812,128} 在牌河上方往右长，3 行舍牌（y≈68~215）一多
    // 就整段压进副露带（y≈99~157）—— 用户截图顶部那团乱牌就是它。
    // 现在挪到河外右翼：x 从牌河右沿（≈1155）之外起步，y 与牌河各行错开。
    2: {
      far: { x: 1720, y: 150 }, angle: 0, w: 46, h: 58, rot: 2, d: 16, thick: 7, side: 5,
      tilt: 0, gap: 12, seam: -2, swap: true, maxLen: 500,
    },
    // 上家：手牌下端往左下角摊开（录像实测坐标）
    3: {
      far: { x: 100, y: 962 }, angle: 105.4 * DEG, w: 84, h: 52, rot: 3, d: 18, thick: 7, side: 5,
      tilt: 15.4 * DEG, gap: 16, seam: -3, swap: true, pinGap: 26, maxLen: 470,
    },
  },
};

export const SEATS = [
  { id: 0, name: '自', wind: '東', label: '本家' },
  { id: 1, name: '右', wind: '南', label: '下家' },
  { id: 2, name: '对', wind: '西', label: '对家' },
  { id: 3, name: '左', wind: '北', label: '上家' },
];

/** 34 面编码顺序，与 client/tiles.py 一致；赤牌单独归一 */
export const SUITS = 'mps';
export const TILE_CODES = [
  ...Array.from({ length: 3 }, (_, si) =>
    Array.from({ length: 9 }, (__, r) => `${r + 1}${SUITS[si]}`)).flat(),
  ...Array.from({ length: 7 }, (_, i) => `${i + 1}z`),
];
export const REDS = ['0m', '0p', '0s'];
export const ALL_CODES = [...TILE_CODES, ...REDS, 'back'];
