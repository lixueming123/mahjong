/**
 * 另外三家的手牌 —— 透视通道里的真·立牌。
 *
 * 为什么必须单独一个透视相机（这是「和 pygame 撞脸」的根因）：
 *   主桌为了 1:1 像素对齐用正交正俯视相机。正交投影下，一张竖立的牌
 *   其牌背法线垂直于视线，投影面积 ≈ 0，只剩顶面一条窄线；想同时看到
 *   牌背和侧面就只能把牌放倒，放倒后又退化成两条平行矩形 —— 就是色带。
 *   视频本身是带俯角的透视 3D，牌是竖着的，远端小近端大、彼此遮挡。
 *   所以正交主桌保留 1:1，这三家牌改由透视相机渲染后叠加，
 *   两者兼得：布局坐标不动，牌真正立起来。
 *
 * 逻辑仍是第一版口径：四家各持一手牌，step 固定、牌宽固定，
 * t = (i-(n-1)/2)*step 从中心展开，13→14 张只变长不拉伸。
 */
import * as THREE from 'three';
import { SEAT_LAYOUT, MELD, DESIGN } from './config.js';
import { makeTile3D } from './tile3d.js';
import { meldNearPoint, seatMeldLayout } from './meldLayout.js';

export class OpponentHandsView {
  constructor() {
    this.scene = new THREE.Scene();
    // 玩家席后上方俯视：远端（对家）小、近端大，侧家牌列自然缩短
    this.camera = new THREE.PerspectiveCamera(32, 16 / 9, 1, 6000);
    this.camera.position.set(0, -1150, 1250);
    this.camera.lookAt(0, 0, 60);
    this.camera.updateMatrixWorld(true);
    this.viewW = DESIGN.W;
    this.viewH = DESIGN.H;

    this.scene.add(new THREE.AmbientLight(0xffffff, .92));
    const key = new THREE.DirectionalLight(0xfff2dc, 1.05);
    key.position.set(-420, -900, 1200);
    key.target.position.set(0, 0, 0);
    key.castShadow = true;
    key.shadow.mapSize.set(2048, 2048);
    key.shadow.camera.left = -1200; key.shadow.camera.right = 1200;
    key.shadow.camera.top = 800; key.shadow.camera.bottom = -800;
    key.shadow.camera.near = 100; key.shadow.camera.far = 3200;
    key.shadow.normalBias = 2;
    key.shadow.radius = 3;
    this.scene.add(key, key.target);
    const fill = new THREE.DirectionalLight(0xdfe9ff, .4);
    fill.position.set(300, 700, 800);
    this.scene.add(fill);

    // 只接影、不遮住主桌的透明地面
    const ground = new THREE.Mesh(
      new THREE.PlaneGeometry(3000, 2000),
      new THREE.ShadowMaterial({ opacity: .3 }),
    );
    ground.receiveShadow = true;
    ground.position.z = -.5;
    this.scene.add(ground);

    this.seats = new Map();   // seat -> { group, tiles: [{holder, mesh}] }
  }

  /** 设计坐标 -> 透视相机下 z=0 桌面上的世界点（射线反投影） */
  unprojectToTable(dx, dy) {
    const nx = 2 * (dx - DESIGN.CX) / this.viewW;
    const ny = 2 * (DESIGN.CY - dy) / this.viewH;
    const p = new THREE.Vector3(nx, ny, .5).unproject(this.camera);
    const dir = p.sub(this.camera.position).normalize();
    const t = -this.camera.position.z / dir.z;
    return this.camera.position.clone().addScaledVector(dir, t);
  }

  /**
   * 把一家牌列校准回录像的设计坐标：反投影牌列两端，
   * 得到 3D 中的中心、方向和缩放。这样透视只负责「牌立起来 + 近大远小」，
   * 不再把整排牌拖离 1:1 的位置。
   */
  placeGroup(group, L) {
    const half = 12 * L.step / 2;
    const ax = Math.cos(L.angle), ay = Math.sin(L.angle);
    // 牌立起来后顶端会在屏幕上偏上，按牌高补偿一小段设计 y；
    // 牌改矮后补偿也要同比例缩小，否则整排会往下掉。
    const drop = Math.round(L.h * .31);
    const p1 = this.unprojectToTable(L.cx - ax * half, L.cy + drop - ay * half);
    const p2 = this.unprojectToTable(L.cx + ax * half, L.cy + drop + ay * half);
    p1.z = 0; p2.z = 0;
    group.position.copy(p1).add(p2).multiplyScalar(.5);

    const u = p2.clone().sub(p1);
    const len3d = u.length();
    u.normalize();
    const up = new THREE.Vector3(0, 0, 1);
    const n = new THREE.Vector3().crossVectors(u, up).normalize();
    // 牌背要朝「这一家到相机」的方向，不能统一朝 -y：
    // 左右家在画面两侧，视线有巨大 x 分量，统一朝 -y 会让牌背侧对镜头，
    // 只剩白顶面 + 黑侧面，看起来像一条棋盘格色带。
    const toCam = this.camera.position.clone().sub(group.position);
    toCam.z = 0; toCam.normalize();
    if (n.dot(toCam) < 0) {
      u.negate();
      n.crossVectors(u, up).normalize();
    }
    group.quaternion.setFromRotationMatrix(new THREE.Matrix4().makeBasis(u, up, n));
    group.scale.setScalar(len3d / (12 * L.step));
  }

  render(state) {
    for (const seat of [1, 2, 3]) {
      const L = SEAT_LAYOUT[seat];
      const hand = state.hands?.[seat] ?? [];
      let entry = this.seats.get(seat);

      if (!entry) {
        const group = new THREE.Group();
        this.scene.add(group);
        entry = { group, tiles: [], k: 1 };
        this.seats.set(seat, entry);
      }
      this.placeGroup(entry.group, L);

      const n = hand.length;
      // 手牌**靠近副露那一端固定**：张数变化只动远端，
      // 一手牌 + 副露在画面上连成一条带子（录像：上家 7 张手牌末端 y591，
      // 副露正好从 y591 开始；13 张时同一端也在同一位置）。
      const meldAngle = MELD.seat[seat]?.angle ?? L.angle;
      const growing = Math.cos(L.angle) * Math.cos(meldAngle)
        + Math.sin(L.angle) * Math.sin(meldAngle);
      const towardMeld = growing >= 0 ? 1 : -1;
      const pin = meldNearPoint(seat, state);
      let tNear = null;
      let signAway = 1;
      let k = 1;   // 远端出屏时整列的等比压缩（牌身 + 间距一起缩）
      if (pin) {
        entry.group.updateMatrixWorld(true);
        // 牌列在屏幕上的走向要从**实际投影**里取：这个 Group 有缩放、方向也可能
        // 被翻转过，拿设计坐标硬推符号会反。取底座上两个点投影回设计坐标，
        // 得到「局部槽位 → 屏幕位移」的方向，再把副露起点反解成槽位。
        const a = this.basePoint(entry.group, 0);
        const b = this.basePoint(entry.group, 100);
        const dx = (b.x - a.x) / 100;
        const dy = (b.y - a.y) / 100;
        const len = Math.hypot(dx, dy) || 1;
        // 远离副露的方向（设计坐标）与「手牌 -> 副露」的间距一起折算成目标点：
        // 立牌要倾斜，右家的副露恰好在它上方，间距得够那一截牌身。
        const awayX = -Math.cos(L.angle) * towardMeld;
        const awayY = -Math.sin(L.angle) * towardMeld;
        signAway = dx * awayX + dy * awayY >= 0 ? 1 : -1;
        const baseGap = MELD.seat[seat].pinGap ?? 8;
        // 透视近大远小：钉点定的是牌底**中心**，而牌身沿牌列占一整张牌宽；
        // 屏幕左下/右下角的立牌还会被放大一倍以上，固定 pinGap 会被
        // 半张放大的牌宽整个吃掉，直接压上副露带（实测最多 ~24px）。
        // 所以从 pinGap 起解，再按多边形间距缺口补足（见下方 clearance）。
        const solve = (gap) => {
          const tx = pin.x + awayX * gap;
          const ty = pin.y + awayY * gap;
          const residual = (tt) => {
            const p = this.basePoint(entry.group, tt);
            return ((p.x - tx) * dx + (p.y - ty) * dy) / len;
          };
          let t0 = 0;
          let f0 = residual(t0);
          let tn = 240;
          let f1 = residual(tn);
          for (let it = 0; it < 8 && Math.abs(f1) > .5; it++) {
            const t2 = tn - f1 * (tn - t0) / ((f1 - f0) || 1);
            t0 = tn;
            f0 = f1;
            tn = t2;
            f1 = residual(tn);
          }
          return tn;
        };
        // 多边形级间距：只对「钉点」量间距挡不住副露牌自己的角 ——
        // 被鸣横放牌经 tilt 旋转 + drop 侧移后，角点沿朝手牌方向比
        // 钉点凸出 10~20px（用户截图实锤）。改成单轴 SAT：
        // 手牌末端 8 角沿 away 轴的最小投影，必须比副露**全部牌**
        // 角点的最大投影还远 6px —— 沿一条轴分开，多边形必然不相交。
        let meldEdge = -Infinity;
        const meldLayout = seatMeldLayout(seat, state);
        if (meldLayout) {
          for (const t of meldLayout.tiles) {
            for (const c of t.corners) {
              const d = c.x * awayX + c.y * awayY;
              if (d > meldEdge) meldEdge = d;
            }
          }
        }
        /** 末端立牌 8 角沿 away 轴的最小投影 − 副露最大投影（负 = 侵入） */
        const clearance = (tn) => {
          let handMin = Infinity;
          for (const sx of [-1, 1]) {
            for (const sy of [-1, 1]) {
              for (const sz of [-1, 1]) {
                const c = this.pointAt(entry.group,
                  tn + sx * L.w / 2, L.h / 2 + sy * L.h / 2, sz * L.d / 2);
                handMin = Math.min(handMin, c.x * awayX + c.y * awayY);
              }
            }
          }
          return handMin - meldEdge;
        };
        // 间距往远挪 1px，手牌末端投影就远 1px：缺口多少补多少，迭代收敛
        let gap = baseGap;
        for (let it = 0; it < 4; it++) {
          tNear = solve(gap);
          const want = gap + (6 - clearance(tNear));
          if (Math.abs(want - gap) < .5) break;
          gap = want;
        }
        tNear = solve(gap);
      }

      // 远端出屏压缩：手牌带被钉在副露旁，副露一多远端就会被推出屏幕
      // （右家最狠：副露带 + 手牌沿同一条斜线挤右下角）。与副露带 maxLen
      // 同一哲学 —— 放不下就整列等比缩小，绝不出屏。透视保线型，牌列
      // 屏幕走向单调，越界只会发生在两端，量两端牌 8 角即可；压缩比对
      // 出屏量单调，线性反解收敛太慢，直接二分求「不越界的最大 k」。
      // 无副露（pin=null，居中布局）也要查：摸牌抬高 7px 会顶到上缘。
      entry.group.updateMatrixWorld(true);
      const LIMITS = { x0: 12, x1: 1908, y0: 12, y1: 1068 };
      const overflowOf = (kk) => {
        let worst = 0;
        for (const i of [0, n - 1]) {
          const t = tNear == null
            ? (i - 6) * L.step * kk * towardMeld
            : tNear + signAway * (n - 1 - i) * L.step * kk;
          const isDrawn = n > 13 && i === n - 1;   // 摸牌：抬高 7、前伸 5
          const fy = L.h * kk / 2 + (isDrawn ? 7 * kk : 0);
          const fz = isDrawn ? 5 * kk : 0;
          for (const sx of [-1, 1]) {
            for (const sy of [-1, 1]) {
              for (const sz of [-1, 1]) {
                const c = this.pointAt(entry.group,
                  t + sx * L.w * kk / 2, fy + sy * L.h * kk / 2, fz + sz * L.d * kk / 2);
                const ox = Math.max(LIMITS.x0 - c.x, c.x - LIMITS.x1, 0);
                const oy = Math.max(LIMITS.y0 - c.y, c.y - LIMITS.y1, 0);
                worst = Math.max(worst, ox, oy);
              }
            }
          }
        }
        return worst;
      };
      if (overflowOf(1) > 0) {
        const K_MIN = .55;
        if (overflowOf(K_MIN) <= 0) {
          let lo = K_MIN;
          let hi = 1;
          for (let it = 0; it < 12; it++) {
            const mid = (lo + hi) / 2;
            if (overflowOf(mid) > 0) hi = mid; else lo = mid;
          }
          k = lo;
        } else {
          k = K_MIN;   // 极端窗口比例压到下限仍放不下：兜底不再缩
        }
      }

      // 尺寸/张数管理：压缩比变了要按新尺寸重建（几何尺寸在创建时定格）
      if (entry.k !== k) {
        while (entry.tiles.length) {
          const t = entry.tiles.pop();
          entry.group.remove(t.holder);
          t.mesh.geometry.dispose();
        }
        entry.k = k;
      }
      while (entry.tiles.length > n) {
        const t = entry.tiles.pop();
        entry.group.remove(t.holder);
        t.mesh.geometry.dispose();
      }
      while (entry.tiles.length < n) {
        const mesh = makeTile3D({ code: 'back', faceUp: false, w: L.w * k, h: L.h * k, d: L.d * k });
        const holder = new THREE.Group();
        holder.add(mesh);
        entry.group.add(holder);
        entry.tiles.push({ holder, mesh });
      }

      for (let i = 0; i < n; i++) {
        const { holder } = entry.tiles[i];
        const t = tNear == null
          ? (i - 6) * L.step * k * towardMeld
          : tNear + signAway * (n - 1 - i) * L.step * k;
        const isDrawn = n > 13 && i === n - 1;
        // 局部 y 就是世界竖直方向：牌底贴桌面、牌背朝玩家，真正的立牌
        holder.position.set(t, L.h * k / 2 + (isDrawn ? 7 * k : 0), isDrawn ? 5 * k : 0);
        holder.userData = { kind: 'opponentHand', seat, index: i, t };
      }
      entry.group.visible = false;    // 由 renderOverlay 逐家点亮
    }
  }

  /** 牌列底座上局部 (t,0,0) 这一点投影回设计坐标（x 右、y 下） */
  basePoint(group, t) {
    return this.pointAt(group, t, 0, 0);
  }

  /** 牌列局部 (x,y,z) 投影回设计坐标：x 沿牌列、y 竖直、z 法向 */
  pointAt(group, x, y, z) {
    const p = new THREE.Vector3(x, y, z);
    group.localToWorld(p);
    p.project(this.camera);
    // NDC -> 设计坐标必须用**当前可见设计区域**（viewW/viewH，resize 时随
    // 窗口宽高比更新），不能写死 1920x1080：窗口比 16:9 宽时主相机 frustum
    // 是 vw=scale 换算出来的宽设计区（如 2062x1080），写死会把投影 x 向
    // 画面中心压缩 ~7%，钉牌解算据此把整条手牌链沿带方向放错位 ——
    // 左家手牌压进副露、右家整条被推出屏幕，全是这一个根因。
    return {
      x: DESIGN.CX + p.x * this.viewW / 2,
      y: DESIGN.CY - p.y * this.viewH / 2,
    };
  }

  setTurn(seat) {
    this.turn = seat;
  }

  /** 主桌（正交）渲染完成后叠加一次；clearDepth 让立牌永远压在台布之上 */
  renderOverlay(renderer) {
    for (const seat of [1, 2, 3]) {
      const entry = this.seats.get(seat);
      if (entry) entry.group.visible = true;
    }
    renderer.autoClear = false;
    renderer.clearDepth();
    renderer.render(this.scene, this.camera);
    renderer.autoClear = true;
  }

  resize(aspect, viewW = DESIGN.W, viewH = DESIGN.H) {
    this.camera.aspect = aspect;
    this.camera.updateProjectionMatrix();
    this.viewW = viewW;
    this.viewH = viewH;
    for (const [seat, entry] of this.seats) {
      this.placeGroup(entry.group, SEAT_LAYOUT[seat]);
    }
  }
}
