/** 静态牌桌层：台布、木框、中央八角情报板。
 *  四家手牌一律是独立牌对象（hand.js / opponents.js），不在这里画；
 *  宝牌指示牌改由 DOM 场况卡第一行显示（ui.js renderDoraRow）。 */
import * as THREE from 'three';
import { DESIGN, BOARD, P, Z } from './config.js';
import { makeClothTexture, makeBoardTexture } from './textures.js';

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
}
