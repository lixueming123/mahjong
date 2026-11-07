/**
 * 装配入口。只包含启动、缩放、渲染循环和模块之间的事件接线；
 * 具体布局在 config.js，绘制在 textures/tiles/board/rivers/hand.js，
 * 协议与会话在 client.js，牌桌数据在 state.js。
 */
import * as THREE from 'three';
import { DESIGN } from './config.js';
import { loadAtlas } from './assets.js';
import { createState } from './state.js';
import { TableView } from './board.js';
import { RiverView, riverSlot } from './rivers.js';
import { MeldView } from './melds.js';
import { HandView } from './hand.js';
import { OpponentHandsView } from './opponents.js';
import { UI } from './ui.js';
import { Lobby } from './lobby.js';
import { Input } from './input.js';
import { Sfx } from './audio.js';
import { MahjongClient } from './client.js';

const canvas = document.getElementById('stage');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: false });
renderer.setClearColor(0x060a12, 1);
// 立体感由几何 + 光照 + 阴影产生：开关一开，牌的侧面/落影就是真的了
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFSoftShadowMap;

const scene = new THREE.Scene();
// 正交正俯视：设计像素 == 屏幕像素，无透视畸变（1:1 复刻录像的关键）。
// 注意「1:1」只锁布局坐标，不锁渲染方式 —— 牌本身仍是 3D 实体并绕自身轴后仰。
const camera = new THREE.OrthographicCamera(-960, 960, 540, -540, .1, 200);
camera.position.set(0, 0, 50);
camera.lookAt(0, 0, 0);

// 固定主光从左上前方来：象牙白侧棱受光、真实投影落在右下
scene.add(new THREE.AmbientLight(0xffffff, .92));
const key = new THREE.DirectionalLight(0xfff2dc, .95);
key.position.set(-480, 520, 1300);
key.target.position.set(0, 0, 0);
key.castShadow = true;
key.shadow.mapSize.set(2048, 2048);
key.shadow.camera.left = -1250; key.shadow.camera.right = 1250;
key.shadow.camera.top = 850; key.shadow.camera.bottom = -850;
key.shadow.camera.near = 100; key.shadow.camera.far = 3000;
key.shadow.bias = -.0006;
key.shadow.normalBias = 2;
key.shadow.radius = 3;
scene.add(key, key.target);

// 牌后仰后牌背法线朝世界 -y，主光从 +y 来会让它偏暗。
// 这盏补光不投影，只托亮牌背；阴影方向仍完全由主光决定。
const fill = new THREE.DirectionalLight(0xdfe9ff, .45);
fill.position.set(180, -900, 900);
scene.add(fill);

let opponents = null;

function resize() {
  const w = window.innerWidth, h = window.innerHeight;
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.setSize(w, h, false);
  const scale = Math.min(w / DESIGN.W, h / DESIGN.H);   // contain，完整显示 1920x1080
  const vw = w / scale, vh = h / scale;
  camera.left = -vw / 2; camera.right = vw / 2;
  camera.top = vh / 2; camera.bottom = -vh / 2;
  camera.updateProjectionMatrix();
  if (opponents) opponents.resize(w / h, vw, vh);
  document.documentElement.style.setProperty('--ui-scale', String(scale));
}
window.addEventListener('resize', resize);
resize();

/** 桥接服务与页面同源；直接开本地文件时退回默认端口 */
function socketUrl() {
  if (!location.host) return 'ws://127.0.0.1:8080/ws';
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${location.host}/ws`;
}

/** 联机参数全部走 URL，不新增界面元素：
 *  ?name=小明&mode=riichi&length=hanchan&room=r-xxxx
 *  账号：?user=alice&pass=secret（&register=1 走注册） */
function clientOptions() {
  const params = new URLSearchParams(location.search);
  return {
    name: params.get('name') || '',
    mode: params.get('mode') === 'riichi' ? 'riichi' : 'classic',
    length: params.get('length') === 'hanchan' ? 'hanchan' : 'tonpu',
    room: params.get('room') || '',
    user: params.get('user') || '',
    pass: params.get('pass') || '',
    register: params.get('register') === '1',
  };
}

async function boot() {
  await loadAtlas();

  const app = createState();
  const sfx = new Sfx();
  sfx.load();                                  // 不阻塞首屏

  const table = new TableView(scene, app.state);
  // 三家手牌走独立透视通道：牌真正竖立，主桌仍保持 1:1 正交
  opponents = new OpponentHandsView();
  const scale0 = Math.min(window.innerWidth / DESIGN.W, window.innerHeight / DESIGN.H);
  opponents.resize(window.innerWidth / window.innerHeight,
    window.innerWidth / scale0, window.innerHeight / scale0);
  const rivers = new RiverView(scene);
  const melds = new MeldView(scene);
  const hand = new HandView(scene);
  const ui = new UI(app);

  app.on('change', () => {
    opponents.render(app.state);
    rivers.render(app.state);
    melds.render(app.state);
    hand.render(app.state);
    table.refreshBoard();
    ui.update(app.state);
  });
  app.on('discard', ({ code, from, seat }) => {
    sfx.play('discard');
    if (seat === 0 && from) {
      hand.fly(code, from, riverSlot(0, app.state.rivers[0].length - 1));
    }
  });
  app.on('draw', ({ seat }) => { if (seat === 0) sfx.play('draw'); });

  // 协议层：一条 WS = 一条游戏连接；所有牌桌状态都来自服务端帧
  const auto = !!(clientOptions().name || clientOptions().user);
  const client = new MahjongClient({ app, ui, sfx, url: socketUrl(), options: clientOptions() });
  const lobby = new Lobby({ app, client, auto });
  ui.attach({ client, lobby });
  lobby.attachUI(ui);
  new Input({ canvas, camera, app, hand, sfx, ui, client });

  if (auto) {
    // URL 直接带昵称/账号：跳过大厅（自动化验收与分享链接都走这条）
    client.connect().catch((err) => lobby.showLogin(`无法连接服务器：${err.message}`));
  } else {
    lobby.showLogin();
  }

  // 离线预览：本地 file:// 打开、或服务端没启动时，1.5 秒内收不到任何帧
  // 就灌一份静态示例牌局，保证界面可见。联机时 phase 会被 game_start 置为
  // playing，这段不会执行；大厅可见时也不执行（登录界面不该透出假牌桌）。
  setTimeout(() => {
    if (app.state.phase !== 'idle' || lobby.visible) return;
    const s = app.state;
    s.hands[0] = ['1m', '2m', '3m', '4m', '5p', '5p', '5p', '3s', '4s', '5s', '1z', '2z', '3z'];
    for (let i = 1; i < 4; i++) s.hands[i] = Array(13).fill('back');
    s.rivers = [
      ['2m', '3s', '6p', '1z', '7m', '5s'],
      ['5m', '8p', '2s', '1z', '9s', '6m'],
      ['3p', '7z', '1m', '9p', '4m', '2p'],
      ['8s', '5z', '7p', '3m', '6s', '1s'],
    ];
    s.melds = [{ seat: 3, kind: 'chi', tiles: ['2m', '3m', '4m'], rotated: 2 }];
    s.scores = [29500, 27400, 22800, 20300];
    s.round = '東2局'; s.remaining = 57; s.turn = 0;
    s.lastAction = '离线示例 · 未连接服务端';
    app.changed();
  }, 1500);

  app.changed();

  // 顶部系统按钮：帮助 / 音效（底部小按钮与它同一开关）
  const soundButtons = [document.getElementById('btn-sound'), ui.el.toggleSfx].filter(Boolean);
  const syncSound = () => {
    if (ui.el.toggleSfx) ui.el.toggleSfx.textContent = sfx.enabled ? '♪ 音效开' : '♪ 音效关';
    const top = document.getElementById('btn-sound');
    if (top) top.classList.toggle('off', !sfx.enabled);
  };
  soundButtons.forEach((button) => button.addEventListener('click', () => {
    sfx.enabled = !sfx.enabled;
    syncSound();
  }));
  syncSound();
  document.getElementById('btn-help')?.addEventListener('click', () => {
    ui.toast('点牌选中，再点一次（或 Enter、或「打出」）出牌；空格 = 过；S = 同步；Esc = 取消；点「立直」后点高亮牌即宣言', '', 6000);
  });

  let frames = 0, last = performance.now();
  function loop(now) {
    requestAnimationFrame(loop);
    hand.update(now);
    renderer.render(scene, camera);
    opponents.renderOverlay(renderer);   // 透视立牌叠加在 1:1 主桌之上
    frames++;
    if (now - last >= 1000) {
      ui.stats({
        calls: renderer.info.render.calls,
        tris: renderer.info.render.triangles,
        fps: Math.round(frames * 1000 / (now - last)),
      });
      frames = 0; last = now;
    }
  }
  requestAnimationFrame(loop);

  // 供自动化验收读取
  window.__mahjong = { app, client, lobby, table, opponents, rivers, melds, hand, ui, renderer, scene, camera };
  document.getElementById('boot').classList.add('ready');
}

boot().catch((err) => {
  console.error(err);
  const el = document.getElementById('boot');
  el.textContent = `加载失败：${err.message}`;
  el.classList.add('error');
});
