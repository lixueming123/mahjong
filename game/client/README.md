# 麻将 · ex_actor pygame 客户端（t5）

严格按 `game/docs/PROTOCOL.md`（NDJSON over TCP）与 `game/docs/UI_UX_SPEC.md`（布局/交互）实现。

## 运行

```bash
./.venv/bin/python game/client/main.py                      # 默认 127.0.0.1:9527
./.venv/bin/python game/client/main.py --host 127.0.0.1 --port 9527
./.venv/bin/python game/client/main.py --fullscreen          # 非验收项
./.venv/bin/python game/client/main.py --no-auto-login       # 有记住的账号时只预填，不自动登录
./.venv/bin/python game/client/main.py --forget-login        # 先清掉本机记住的账号口令
```

或者走启动脚本（固定用仓库根虚拟环境，参数原样透传）；服务端对应 `./game/start_server.sh`（缺二进制时自动编译）：

```bash
./game/start_client.sh --host 127.0.0.1 --port 9527
```

依赖：仓库根目录虚拟环境 `./.venv/bin/python`（pygame 2.6.1）。
美术：**只**用 pygame 几何绘制 + 系统字体，不加载任何外部图片/字体文件（UI_UX_SPEC §1.3 / UX-9）。
v2.4 起牌面是**图案化**的（万 / 筒 / 条按传统牌排布，见下文），质感层（绒面台、玻璃卡片、
投影、鎏金细线）由 `render/art.py` 用渐变 + 高光程序化生成并按尺寸缓存。
v2.5 起牌桌是**房间里的实体牌桌**：`art.room` 画房间、`art.table_surface` 画木边圆桌、
`art.portrait` 曾画四家半身立绘（v4.0 起移除）；侧家牌山仍沿桌边斜码，牌河则按 v5.0
改成中央板两侧的正交网格（牌横躺 90°、不倾斜）。

牌桌布局 v4.0 改按《雀魂》对局录像（2026-11-05 抽帧）重排：`art.riichi_cloth` 画铺满整屏的
深蓝台布（桌心放射线 + 四家牌河区细框 + 左右木边），桌心换成八边形情报板（四家持点 / 场风 /
余牌），四家名牌从顶部横条改成四角/四边的竖排窄卡（头像 + 名字 + 分数），场况收进左上金边卡片。
侧家牌山倾角一路收直（21° → 9° → v5.2 **0°，完全竖直**，`TABLE_FAR_HALF == TABLE_NEAR_HALF`），
橙金牌背由 `SIDE_STRIP_TINT` 对原贴图做 RGB 乘法得到，不改
CC BY 原始素材。v5.3 起 `faces/back.png`（素材自带大红）也统一调色：灰度 → `BACK_LIFT` 抬亮
→ 乘 `BACK`，暗杠占位与其它牌背不再是红卡片。牌尺寸 / 河流容量 / hit-test / 协议帧一律不动，验收走 `client_headless_test.py`。
v5.1 侧家副露改成「一组一行、纵向堆叠」：由 `side_flat_meld_rect()` 推导（v5.2 后约
`(238,176,124,160)` / 镜像 `(918,176,124,160)`），最多 4 组，贴牌山内侧、与中央牌河留出间隙；
不再与对家共用横排布局（多组横排会溢出压到弃牌）。

v5.0 继续按同一段录像把**四家牌河统一成紧贴八角板的方形环**：侧家弃牌从桌边斜轨搬进
板左右两侧（`RIVER_RING_L/R`，6 行竖排 × 3 列、横躺牌不再倾斜），对家牌山上移到屏幕顶沿
给三排牌河让位，本家/对家牌河第一排都贴板、后续排朝外叠（`center_river_rects(flip_rows=)`）。
牌面放大到 32×38（6 列 = 204 px，与板宽 216 px 基本等宽），八角板四角补上自风徽标。

v5.3 对家副露从「牌河右上角横排」（`(756,168,300,32)`，会与下家副露列相交、
4 组还会缩到糊掉）搬到顶部牌山左侧空档 `R_OPP_MELD=(230,56,240,80)`：
`meld_layout(groups_per_row=2)` 每行最多 2 组、多的换行（4 组 = 2 行），
第一行与橙色牌山同高连成一条横带，并改 `project_tiles=False` 与中央牌河一致。
`side_river_rects()` / 飞行动画终点 / 播报碰撞盒全部跟随新网格；河流容量仍是每家 18 张。

## 登录记忆（v4.6，`UI_UX_SPEC.md` §3.5）

「账号」模式下密码框右侧有「记住密码」勾选框（默认勾着）：**账号登录成功后**把账号名与口令
写到 `~/.ex_actor_mahjong/login.json`（目录 `0700`、文件 `0600`，**明文**、按 `host:port` 分条），
下次启动自动切到账号模式、填好两个框并直接登录一次，不用再敲。取消勾选**立即删档**。

- `--no-auto-login`：只预填、不自动提交（想换账号/先看大厅时用）。
- `--forget-login`：启动前删掉本服务器的存档。
- `MAHJONG_LOGIN_STORE=off ./game/start_client.sh`：这次运行完全不读不写存档（脚本与排障用）。
- 明文是**演示级**取舍（与 `ACCOUNT_DESIGN.md` §2.2 的口令口径一致）：要上生产就把
  `login_store.save()/load()` 换成系统钥匙串。明文口令不进日志、不进错误消息、不回显。

验收：`./.venv/bin/python game/tests/login_memory_test.py`。

## 规则

输入昵称 → 「进入牌桌」后自动串行握手：

```
login{name} → login_ok → create_room → room_joined → ready → game_start
```

牌桌交互：单击手牌选中（金色描边 + 上移 8px）→ 再点同一张 / `Enter` / 「打牌」按钮确认打出。
可选加速键：`1`..`9`/`0` 选中第 1..10 张、`Space` 点「过」、`Esc`/右键取消选中、`F1` 调试开关。

## 文件结构

| 文件 | 职责 |
| --- | --- |
| `main.py` | 入口与启动参数（`--host` / `--port` / `--fullscreen` / `--no-auto-login` / `--forget-login`） |
| `login_store.py` | 本机登录凭据存档（`~/.ex_actor_mahjong/login.json`，0600，按 `host:port` 分条） |
| `app.py` | 主循环、握手链路、输入意图、重试/断线/错误态（60 FPS，网络非阻塞） |
| `net_client.py` | TCP + NDJSON：独立读线程、独立写线程、发送队列上限 64、64 KB 单帧上限 |
| `game_state.py` | 唯一状态源：`apply(frame)` 分发表；`state_sync` 权威覆盖，其余帧为即时增量 |
| `tiles.py` | 牌编码/排序/多重集扣减（PROTOCOL §2 自包含；34 面 = 27 数牌 + 7 字牌） |
| `view.py` | 渲染层只读快照 `UiView` |
| `theme.py` | 颜色、字体、全部冻结坐标常量（含字牌墨色 `SUIT_Z` 与 6 档字牌字号） |
| `render/` | `art`（程序化质感层：房间/木边牌桌/人物立绘/玻璃卡片/按钮皮肤/投影，全部按尺寸缓存）、`widgets`（牌/按钮/文本原语）、`login`、`lobby`、`table`、`actionbar`、`tenpai`、`result`、`stats`、`broadcast`、`overlays` |

## 牌面口径（PROTOCOL v1.1 §2 / UI_UX_SPEC v1.3 §1.3、§4.4）

- **数牌** `1m`..`9s`（v2.4 图案化，细则见 `UI_UX_SPEC.md` §4.4.1）：
  **万** = 上「一…九」（墨蓝）+ 下「萬」（红）；**筒** = 同数目圆点，按传统牌三色排布
  （1 筒大饼、5 筒四角+红心、9 筒蓝红绿三行…）；**条** = 同数目竹节短棒（5 条中行 / 7 条首行 /
  9 条中列为红；1 条在手牌等大牌面画麻雀）。牌身统一「底沿厚度 + 顶沿高光」。
- **字牌** `1z`..`7z` = 東/南/西/北/白/發/中：**单字居中、无数字、无花色角标、不旋转**，颜色 `SUIT_Z (48,44,40)`；
  字号按牌位冻结为 手牌 34 / 中央牌河 20 / 左右牌河 17 / 副露 19 / 结算和牌张 56 / 结算手牌 28 px 粗體。
- 排序键 `(suit_index, rank)`，`m=0 < p=1 < s=2 < z=3` ⇒ **数牌在前、字牌 1z..7z 在后**；
  字牌**不可吃**（只能构成刻子/杠），顺子只由数牌构成。
- 服务端下发的 `hand[]` / `melds[].tiles[]` **不重排**；仅本地 `draw` 增量插入时按上述键重排。

## 界面映射（UI_UX_SPEC §2.1 四界面）

| 界面 ID | 落地位置 |
| --- | --- |
| `SCREEN_LOGIN` | `game_state.SCREEN_LOGIN` → `render/login.py` |
| `SCREEN_TABLE` | `game_state.SCREEN_TABLE` → `render/table.py` |
| `SCREEN_ACTIONBAR` | 同屏叠加层：`window is not None and screen == SCREEN_TABLE and not frozen` → `render/actionbar.py` |
| `SCREEN_RESULT` | `game_state.SCREEN_RESULT` → `render/result.py` |

## 无头验证

```bash
./.venv/bin/python game/tests/client_headless_test.py     # 退出码 0 即通过
```

产物：`game/tests/artifacts/client_login.png`、`client_table.png`（含字牌的手牌/牌河/四家副露）、
`client_result.png`（含字牌杠/刻子/做将的结算分组）。

## 私有信息隔离（UI_UX_SPEC §7）

`SeatView` 字段固定为 `seat, name, is_bot, hand_count, discards, melds, disconnected`，
**不存在**任何可存放他家牌面的字段；他家手牌只以 `hand_count` 张牌背渲染；
即使服务端缺陷下发 `state_sync.seats[].hand`，客户端也只记 WARN、不渲染、不写入状态。
