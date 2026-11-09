/**
 * 牌河在桌面世界里的落位 —— 渲染（rivers.js）、出牌飞行落点（main.js）、
 * 几何回归（tests/web/geo.mjs）共用这一份。
 *
 * 为什么不再按设计坐标逐张反投影：那样每张牌都被「拉回」设计尺寸，
 * 透视的纵向压缩被抵消，牌河的牌看起来又瘦又高（录像里上下家的牌几乎是方的），
 * 张与张的比例也各不相同。现在整桌牌用**同一个世界尺寸**、按真实比例
 * （宽 58 : 长 80 : 厚 30）贴紧排成网格，近大远小、纵向压缩、前侧面
 * 被下一排遮住，全部由透视自然给出 —— 和录像一致。
 *
 * 四家牌河的「内沿」贴着情报板四条边：先把内沿中点（顶面高度）反投影到桌面，
 * 再在世界里往外排。
 */
import { RIVER, BOARD } from './config.js';

/**
 * seat 家第 n 张舍牌的世界落位：{ x, y, z, sx, sy, horizontal }。
 * z 是牌厚中心；lift 为整体抬高（设计像素，鸣牌被指的那张）。
 * @param {import('./tablepass.js').TablePass} pass
 */
export function riverPlacement(pass, seat, n, lift = 0) {
  const s = pass.scaleAt(BOARD.x, BOARD.y).sx;
  const horizontal = seat === 1 || seat === 3;
  // 世界 x / y 向的牌长：上下家竖放，左右家横放
  const tw = (horizontal ? RIVER.h : RIVER.w) * s;
  const th = (horizontal ? RIVER.w : RIVER.h) * s;
  const seam = RIVER.seam * s;
  const col = n % RIVER.cols;   // 沿「自己手牌方向」的第几张
  const row = Math.floor(n / RIVER.cols);
  const zTop = RIVER.d * s;
  // 锚点取「这条内沿在屏幕上最靠板子的那个高度」：下家/左右家是顶面棱，
  // 上家朝镜头的前侧面会往下盖，要用它贴桌的底棱，否则前侧面压到板上。
  const unproject = (dx, dy, z = zTop) => pass.opponents.unprojectToTable(dx, dy, z);

  const top = BOARD.y - BOARD.h / 2;
  const bottom = BOARD.y + BOARD.h / 2;
  const left = BOARD.x - BOARD.w / 2;
  const right = BOARD.x + BOARD.w / 2;
  const c = (RIVER.cols - 1) / 2;

  let x;
  let y;
  if (seat === 0 || seat === 2) {
    // 上下家：6 张一行横排，行往外（远离情报板）延伸
    const a = seat === 0
      ? unproject(BOARD.x, bottom + RIVER.gap)
      : unproject(BOARD.x, top - RIVER.gap, 0);
    const outward = seat === 0 ? -1 : 1;   // 世界 y 向上：本家往近处（-y）
    x = a.x + (col - c) * (tw + seam);
    y = a.y + outward * (th / 2 + row * (th + seam));
  } else {
    // 左右家：第一张顶边对齐板顶，6 张往下码，列往外延伸
    const outward = seat === 3 ? -1 : 1;
    const a = unproject(seat === 3 ? left - RIVER.sideGap : right + RIVER.sideGap, top - RIVER.sideLead);
    x = a.x + outward * (tw / 2 + row * (tw + seam));
    y = a.y - th / 2 - col * (th + seam);
  }
  return { x, y, z: zTop - RIVER.d * s / 2 + lift * s, sx: s, sy: s, horizontal };
}

/** 第 n 张舍牌顶面中心在设计坐标里的位置（出牌飞行的落点） */
export function riverSlot(pass, seat, n) {
  const p = riverPlacement(pass, seat, n);
  return pass.toDesign(p.x, p.y, p.z + RIVER.d * p.sx / 2);
}
