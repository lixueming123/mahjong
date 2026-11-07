/** 静态牌桌层：台布、木框、中央八角情报板、宝牌指示。
 *  四家手牌一律是独立牌对象（hand.js / opponents.js），不在这里画。 */
import * as THREE from 'three';
import { DESIGN, BOARD, P, Z } from './config.js';
import { makeClothTexture, makeBoardTexture } from './textures.js';
import { makeMiniTile } from './tiles.js';

/**
 * 台布是不透明实体并参与深度测试：
 * 它既要接收 3D 牌投下的真实阴影，也不能在透明队列里反向盖住牌。
 * （2D 的 HUD/牌河层仍走 renderOrder，位置互不重叠，不影响。）
 */
function fullScreenPlane(tex, order) {
  const m = new THREE.Mesh(
    new THREE.PlaneGeometry(DESIGN.W, DESIGN.H),
    new THREE.MeshStandardMaterial({ map: tex, roughness: 1, metalness: 0 }),
  );
  m.position.z = -22;
  m.receiveShadow = true;
  m.renderOrder = order;
  return m;
}

export class TableView {
  constructor(scene, state) {
    this.scene = scene;
    this.state = state;

    scene.add(fullScreenPlane(makeClothTexture(), Z.CLOTH));

    this.boardMesh = null;
    this.refreshBoard();

    this.dora = new THREE.Group();
    scene.add(this.dora);
    this.renderDora();
  }

  refreshBoard() {
    if (this.boardMesh) {
      this.scene.remove(this.boardMesh);
      this.boardMesh.geometry.dispose();
      this.boardMesh.material.map?.dispose();
      this.boardMesh.material.dispose();
    }
    const { texture, width, height } = makeBoardTexture(this.state);
    const m = new THREE.Mesh(
      new THREE.PlaneGeometry(width, height),
      new THREE.MeshBasicMaterial({
        map: texture, transparent: true, depthTest: false, depthWrite: false,
      }),
    );
    const [x, y] = P(BOARD.x, BOARD.y);
    m.position.set(x, y, 0);
    m.renderOrder = Z.BOARD;
    this.scene.add(m);
    this.boardMesh = m;
  }

  /**
   * 顶部偏左那一排小牌，严格按录像：
   *   - 内容是**两组顺子**：2-3-4筒、6-7-8筒（用户点名的「上边的
   *     二三四筒六七八筒」），不是随便挑几张筒子、也不能有重复牌。
   *   - 清一色翻开的明牌，**没有任何橙金牌背**；
   *     旧版「2 张牌背 + 3 张明牌」是照旧客户端口径自己加的，录像里不存在。
   *   - 白牌身 + 下缘橙金厚边、紧贴成一排、位置贴画面上沿。
   */
  renderDora() {
    this.dora.clear();
    const codes = ['2p', '3p', '4p', '6p', '7p', '8p'];
    codes.forEach((code, i) => {
      const t = makeMiniTile(code, 39, 40, Z.RIVER + i * .01);
      const [x, y] = P(385 + i * 40, 55);
      t.position.set(x, y, i * .6);
      this.dora.add(t);
    });
  }
}
