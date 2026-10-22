# 麻将里程碑协议（PROTOCOL）

- 文档 ID：PROTOCOL
- 版本：**v1.6（冻结）**
  v1.6 **A 批增补**（番型表 + 番数结算 + 一场四局/连庄/场次总分 + 暗杠/加杠）：① `action` 动作词表 5 → **7**（+`ankan`、+`kakan`），**不新增窗口类型**（两者是 `discard` 窗口的附加 `options`）；② `melds[].type` 3 → **5**（+`ankan`、+`kakan`），暗杠对他家的 `tiles` 为**空数组**并在局终揭示（§5.9、§8.1）；③ `game_end` **只增字段**（`win.yaku[]` / `win.han_breakdown{base,tsumo,dealer,total}` / `win.han` / `win.dealer_bonus` / `win.rinshan` / `results[].match_total` / 顶层 `round_index` / `dealer` / `renchan_count` / `match_will_continue`），既有字段形状一律不动（**`win.tags` 保持旧语义**，仍是 `yaku[].id` 的子集）；④ 新增服务端帧 **`match_end`**（场终，§5.15）；⑤ **零新增错误码**（仍 14 条，§7.1）；⑥ 明确 `seq` **跨局单调不重置**（§6.2）。见附录 C v1.6 行。
  v1.5 大厅选房：新增 `list_rooms` / `room_list`；`join_room` 在开局前替换一个 AI 座位并回 `room_joined`；`create_room` 不自动开局；`ready` 仍是开局信号。错误码不新增。见 §3、§4.2–§4.4、§4.11、§5.14。
- 状态：**已冻结**（t3 网关、t4 房间状态机、t5 客户端、t6 端到端脚本必须严格按本文实现；变更须走新的需求评审）
- 修订：v1.6 依 captain 裁决（A 批需求评审，见 `A1_ARCH_ASSESSMENT.md` 的 `0.3` 与附录 C v1.6 行）**增补**番型表/番数结算、一场四局/连庄/场次总分、暗杠/加杠：帧数 C→S **10 条不变**（动作词表扩到 7 值，不新增帧类型）、S→C **13 → 14**（+`match_end`），错误码 **14 条不变**。既有 14 条错误码的语义与阈值、15 s 窗口、`seq` 作废方案、136/83/84/21、零和口径、私有信息规则（除暗杠一处新增）**全部未动**；`M ∈ {1,4,16}` 的算分口径随 `RULESET`/`GAME_DESIGN` 的 A 批修订版改由番数表承载（本文只改承载字段，不改数值）
  v1.4 依用户需求（本轮 actor 化重构）补 §7.2 一句话：计数与阈值**逐字不变**，仅**计数归属**从 `Connection` 移到该连接的 `ConnectionActor`。**帧格式、字段、时序、错误码与配额数值一律未动**（见附录 C v1.4 行）
  v1.3 依 captain 裁决（账号功能需求）**增补**账号注册/登录、战绩查询与三条错误码；帧数 8→10（C→S）、12→13（S→C），错误码 11→14。既有 11 条错误码的语义与阈值、15 s 窗口、`seq` 作废、私有信息规则、136/83/84/21 与零和口径**全部未动**（见附录 C v1.3 行与 `ACCOUNT_DESIGN.md`）
- 作者 / 负责人：game-architect（t1）
- 上游源头：`game/docs/GAME_DESIGN.md` **v1.10（A 批修订版，待创意总监定稿后确认版本号）**（玩法源头）、`game/docs/UI_UX_SPEC.md` **v1.1**（表现层）。A 批（番型表/番数/局制/暗杠加杠）的**规则源头**以 `GAME_DESIGN.md` / `RULESET.md` 的 A 批修订版为准，与本文 v1.6 同批冻结（由创意总监定稿）；两者冲突时以 `GAME_DESIGN.md` 为准（§0.1）
- 关联文档：`game/docs/RULESET.md`（t1，规则数值与算法）、`game/docs/GAME_DESIGN_REVIEW.md`（t10/t12 门禁与 H1–H7 移交项）、`game/docs/ACCOUNT_DESIGN.md`（v1.3，账号与战绩）、`game/docs/A1_ARCH_ASSESSMENT.md`（v1.6 的架构评估与改造点清单）
- 适用里程碑：**单房间、四座位（1 真人 + 3 服务端 AI）；v1.6 起为「一场 4 局 + 连庄」的场次（§0.4）**

## 0. 适用范围、优先级与设计决断

### 0.1 优先级

1. `GAME_DESIGN.md` v1.1 是玩法源头；本文与它冲突时**以 GAME_DESIGN 为准**，并视为本文缺陷（须修订本文）。
2. 本文冻结**线协议**（帧、字段、时序、错误、配额、私有信息）；`RULESET.md` 冻结**规则与数值算法**（牌张集合、和牌判定、副露合法性、算分、牌山）。
3. 实现方（t3/t4/t5/t6）不得自行发明字段、消息类型或错误码；确有必要时先提需求评审。

### 0.2 本协议采纳的强制项（来自 t10/t12 门禁与 captain 指示）

| 编号 | 内容 | 本文落点 |
| --- | --- | --- |
| H1 | 打牌窗口的合法回复只有 `discard{tile,seq}`，或 `action` 且动作在该窗口 `options` 内（`win` / v1.6 起的 `kong` / `ankan` / `kakan`）；`action{pass,seq}` 属 `ILLEGAL_ACTION`。**v1.6 修订**：原文只允许 `discard` 或 `action{win}`，暗杠/加杠使该窗口出现自理杠动作，故按「以窗口 `options` 为准」重述，既有语义（`pass` 非法、不在 `options` 内即 `ILLEGAL_ACTION`）逐字不变 | §4.6 合法性矩阵、§6.1、§6.3 |
| H2 | `action` 与 `discard` **必须**携带 `seq` | §4.5、§4.6 |
| H3 | `action_event` 语义（`seat`=执行者、`tiles` 同提交语义、`from_seat`=被鸣/被荣和那张牌的打出者、自摸不带、`pass` 不产生） | §5.8 |
| H4 | `melds[]` 形状 `{type,tiles[]}`；`type` 值集 v1.6 起为 `pong/chow/kong/ankan/kakan`（`kong`/`ankan`/`kakan` 4 张；**`ankan` 对他家 `tiles` 为 `[]`**）；`results[].hand` 保持扁平 `14+杠数`，两者并存 | §5.9、§5.10、§5.12 |
| H5 | `draw`/`discard_event`/`action_event` 携带 `seq`；`game_end` 不携带 | §5.5、§5.6、§5.8、§5.10、§6.2 |
| H6 | `state_sync.seats[]` 不得含 `hand`（私有手牌只发本人） | §8.1、§8.2 |
| H7 | 错误码表 **14 条**逐字采用（v1.6 订正：本条原写"11 条"，那是 v1.3 之前的旧数字，权威值以 §7.1 为准）；`STALE_SEQ` 不作为错误码 | §7.1 |
| H8 | 服务端**永不采信**客户端的和牌/副露/算分结论，必须用规则库重算 | §8.3 |
| H9 | `state_sync` 为权威状态；`action_event` 仅为即时事件，两者到达顺序不作保证 | §5.12、§8.4 |
| H10 | 牌编码与排序口径自包含 | §2 |
| S1–S4 | 打牌窗口不接受 `pass`、`action` 带 `seq`、`melds[]` 形状、他家无 `hand` | H1/H2/H4/H6 |

### 0.3 术语（与 `GAME_DESIGN.md` §0.2 逐字一致）

座位 seat（0..3，出牌顺序 0→1→2→3→0）、自家/下家/对家/上家（对座位 `s` 分别是 `s`/`s+1`/`s+2`/`s+3` mod 4）、手牌 hand、副露 meld、牌河 discards、牌山 wall、摸牌 draw、打牌 discard、碰 pong、吃 chow、杠 kong（**仅明杠**）、荣和 ron、自摸 tsumo、荒庄 draw_exhausted、动作窗口 action window、`seq`、托管。

本文中 **MUST / MUST NOT / SHOULD / MAY** 按 RFC 2119 语义使用。

### 0.4 里程碑上限（明确不做）

单房间、四座位（1 真人客户端 + 3 服务端 AI 机器人）。**v1.6 起局制为「一场 4 局 + 连庄」**（`max_rounds = 4`、`max_renchan = 4` 双闸；连庄不占局数、超过连庄上限即结束），一场结束后房间仍按 §1.4 第 5 条回收。

**不做**（v1.6 确认）：观战（spectator）、断线重连与**对局状态**的持久化恢复（断线一律走托管，见 §8.5）、立直/宝牌/振听/符数/役满（番数封顶 6 番）、**抢杠**（加杠为自理动作，不设应答窗口；此项**永久不做**，不是延后）、多人同房/换座、移动端。

**在范围内**：账号与战绩持久化（v1.3，见 `ACCOUNT_DESIGN.md`）、番种表与番数结算、暗杠与加杠、多局场次/连庄/场次总分（以上四项 v1.6）。协议中不存在对应消息的，实现方不得私自扩展（§0.1 第 3 条）。

### 0.5 关键设计决断：动作窗口超时采用「`seq` 作废」（不可用取消机制）

ex_actor 的 `Send` **不支持取消**、mailbox 中的消息**不可撤回**、actor 执行**不可抢占**。因此：

1. 动作窗口超时**不实现任何取消/撤回/打断**机制；服务端在开窗时记录 `window_seq = room.seq`，并派发一个携带该 `window_seq` 的延时「超时消息」。
2. 超时消息到达 RoomActor 时，若 `window_seq != room.seq`（窗口已因玩家的动作或超时代打而结束）→ **直接丢弃，状态零变更**。
3. 客户端迟到的 `discard`/`action`（`seq` 已过期）→ **静默丢弃，不回 `error`**（见 §6.2/§6.3）。
4. 后续所有实现必须遵守本决断；任何「撤销上一帧」「打断当前动作」的设计都属违规。

### 0.6 牌张口径（v1.1 起：**34 面 / 136 张 / 牌山 83**）

本协议的协议字段不依赖牌张总数（只传输 `remaining` 整数），但编码表与 `remaining` 初值依赖它，故在此冻结最终口径：

- **牌面 = 3 门 × 9 级（27 种数牌）+ 7 种字牌 = 34 种**；每种 4 张 ⇒ **总张数 136**；
- 字牌编码 `1z`..`7z`（1z=东 / 2z=南 / 3z=西 / 4z=北 / 5z=白 / 6z=发 / 7z=中），详见 §2.1；
- 发牌仍为 53 张（庄家 14 + 其余三家各 13）⇒ **牌山初始剩余 83**（136 − 53）；
- 派生值：单局最多 **83 次摸牌 + 1 次庄家第一打 = 84 次打牌**；单座位牌河上限 **⌈84/4⌉ = 21**。
- **v1.6 口径说明（数值不变，适用范围明确化）**：以上四项（136 / 83 / 84 / 21）都是**每局**口径。一场 4 局（+连庄）时**每局重新建牌山、重新发牌**，故每局的 83 与 84 逐局重置；**场次总量上界 = 局数 × 84**（局数 ≤ `max_rounds + max_renchan`）。
- **杠补牌不改变上述上界**：明杠 / 暗杠 / 加杠的补牌一律**从同一副牌山取下一张**且 `remaining--`（`RULESET.md` §1.5），因此「最多 83 次摸牌」与「单座位 21 张牌河」在含杠的对局里仍然成立。**实现红线**：`remaining` 必须始终由「已摸牌数」派生（`remaining = 136 − 已摸牌数`），不得单独维护一个可能与牌山脱节的计数器。

**历史沿革（保留语境，勿再引用旧值）**：t1 曾冻结 108 张 / 牌山 55（当时为满足「无字牌」而把总数从 136 修正为 108），t13 据此做过一次 136→108 的级联。captain 于 t14 重新裁决：实测显示 108 口径下每人仅能摸 13–14 次牌（真实麻将 17–18 次），200 局中 172 局荒庄（86%），**牌局过短**；故**恢复 7 种字牌**，回到标准 136 张口径。四份文档（`GAME_DESIGN.md` v1.3 / `RULESET.md` v1.2 / `PROTOCOL.md` **v1.2** / `UI_UX_SPEC.md` v1.3）已同步一致，实现侧亦已同步（t15 完成）。

---

## 1. 传输层

### 1.1 地址与端口

| 项 | 冻结值 |
| --- | --- |
| 监听地址 | `127.0.0.1` |
| 默认端口 | **9527**（服务端 `--port` 可覆盖） |
| 客户端默认目标 | `127.0.0.1:9527`（`--host` / `--port` 可覆盖） |
| 端到端脚本端口 | **19527**（避免与本机默认端口冲突） |
| 传输 | 纯 TCP（无 TLS、无 WebSocket、无 ZeroMQ；本里程碑单进程，actor 之间用 `SendLocal`） |

### 1.2 编码与分帧（NDJSON）

1. 编码 **UTF-8**；每帧是一个 JSON 对象，以单个 `\n`（`0x0A`）结尾。
2. **单帧上限 64 KB（65536 字节，不含结尾 `\n`）**：超限 → 尽力回一次 `error{code:"FRAME_TOO_LARGE"}` 后**立即断开该连接**。
3. 缓冲区累积超过 64 KB 仍未见 `\n` → **立即断开该连接**（不做无界缓冲）。
4. 非法 UTF-8 或非法 JSON → 计一次坏帧，回 `error{code:"BAD_REQUEST"}`，连接保持（累计 10 次断开，见 §7.2）。
5. 帧内**不得**出现裸换行（JSON 字符串中的换行必须转义为 `\n`）。

### 1.3 帧格式与兼容策略

| 规则 | 冻结语义 |
| --- | --- |
| 帧结构 | 单行 JSON 对象；**MUST** 含 `"type"`（string） |
| 缺失/非字符串 `type` | `error{code:"BAD_REQUEST"}`，连接保持 |
| 未知 `type` | `error{code:"BAD_REQUEST"}`，连接保持 |
| 未知字段 | 服务端 **MUST** 忽略（不得报错） |
| 未知的服务端帧类型 | 客户端 **SHOULD** 忽略并记日志（前向兼容），不得崩溃 |
| 字段类型错误（如 `seq` 非整数） | `error{code:"BAD_REQUEST"}`，状态零变更 |
| 数值范围 | `seq`/`deadline_ms`/`remaining` 为非负整数（`seq ≥ 1`） |

### 1.4 连接生命周期

1. 一个 TCP 连接最多承载 **1 个玩家**；一个玩家最多属于 **1 个房间**。
2. 连接断开（FIN/RST 或心跳超时）→ 服务端回收该 socket 与其玩家映射，**不得影响其他连接、其他座位或房间状态**。
3. 大厅中（未开局）真人断开 → 释放座位；本里程碑房主断开即销毁房间（GAME_DESIGN §8.3）。
4. 对局中真人断开 → 该座位标记 `disconnected = true` 并进入**托管**（立即按默认口径代打，不等 15 s），牌局继续（§8.5）。
5. **场终**（收到 `match_end`）之后 30 秒仍未被客户端断开 → 服务端回收房间与连接资源。**局终（`game_end`）不触发回收**：v1.6 起房间跨局存活（v1.6 修订；原文为「`game_end` 之后 30 秒」）。
6. **局间**（两局之间，`game_end` 与下一局 `game_start` 之间）：连接保持、**不回收**、**不接受 `join_room`**（§4.3）、也不需要客户端任何输入。局间时长由服务端决定，客户端不得依赖其具体数值（协议不承诺秒数；服务端在下一局开始时直接下发 `game_start`）。
7. **账号会话（v1.3 增补）**：一个账号同时只能有一个在线连接。第二个连接用同一账号登录 → `error{code:"ALREADY_ONLINE"}`，连接保持（仍可改用游客身份登录）。占用在**连接关闭**时释放；游客（`login{name}`）不受此约束（无账号身份）。

### 1.5 时间

- `deadline_ms` = **服务端系统时钟的 epoch 毫秒（UTC）**。
- 服务端内部超时判定 **MUST** 使用单调时钟（`timer`/`poll` 超时），不得用 `deadline_ms` 反推。
- 客户端计算剩余时间见 UI_UX_SPEC §6.2（取服务端剩余与本地兜底的较小值）。

### 1.6 心跳

| 项 | 冻结值 |
| --- | --- |
| 客户端 `ping` 间隔 | **5000 ms** |
| 服务端响应 | `pong`（原样回一帧，无字段要求） |
| 服务端无入站帧超时 | **20000 ms** → 大厅中：断开连接；对局中：进入托管（不立即断开） |
| `ping` 是否计入限速 | **是**（§1.7） |

### 1.7 限速与配额（冻结值）

| 情形 | 阈值 | 行为 |
| --- | --- | --- |
| 入站帧速率 | 滑动 1 秒窗口内 > **100** 帧 | 丢弃超出帧；同一次限速事件最多回 1 帧 `error{code:"RATE_LIMITED"}` |
| 持续风暴 | 1 秒内 > **500** 帧，或连续 **3 秒**每秒都触发限速 | 立即断开连接 |
| 坏帧累计 | **10** 次 | 第 10 次后断开连接 |
| 非法操作累计 | **10** 次（§7.3） | 第 10 次后断开连接 |
| 昵称非法累计 | **3** 次 | 第 4 次断开连接 |
| 单帧大小 | > **65536** 字节 | 立即断开（先尽力回 `FRAME_TOO_LARGE`） |

### 1.8 服务端与客户端启动参数（冻结）

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| 服务端 `--port` | 9527 | 监听端口 |
| 服务端 `--seed` | 系统时间派生 | 牌山随机种子；端到端脚本 **MUST** 传固定值以复现 |
| 服务端 `--action-timeout-ms` | **15000** | 动作窗口时长；合法区间 `[1000, 60000]`，越界或非数字 → 启动报错退出（fail fast） |
| 客户端 `--host` / `--port` | 127.0.0.1 / 9527 | 目标服务器 |
| 客户端 `--fullscreen` | 关 | 可选，非验收项 |

> **不存在** `--ai-level`：AI 固定为 L1（GAME_DESIGN §7.1），无难度配置项。

---

## 2. 牌编码与排序（H10，客户端与服务端必须完全一致）

### 2.1 编码

- 数牌正则：`^[1-9][mps]$`；字牌正则：`^[1-7]z$`。合取正则：`^([1-9][mps]|[1-7]z)$`。
- `m` = 万、`p` = 筒、`s` = 条；`z` = 字牌（**无花牌编码**）。例：`"1m"`、`"9s"`、`"1z"`、`"5z"`。
- **字牌映射（逐字冻结）**：

| 编码 | `1z` | `2z` | `3z` | `4z` | `5z` | `6z` | `7z` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 牌面 | 东 | 南 | 西 | 北 | 白 | 发 | 中 |

- 牌面种类 **34**（3 门 × 9 级 = 27 种数牌 + 7 种字牌）；每种 4 张 ⇒ **总张数 136**。牌山初值见 §0.6 与 `RULESET.md` §1。
- 其他编码（含花牌、`0z`、`8z`、`z` 以外的花色字母）→ `error{code:"UNKNOWN_TILE"}`。
- **字牌只能构成刻子/杠，不可用于顺子（不可吃）**；和牌判定对字牌与数牌一视同仁（见 `RULESET.md` §3）。

### 2.2 排序（服务端下发顺序与客户端渲染顺序一致）

排序键：**先数牌后字牌**，即 `(suit_index, rank)`，其中 `m=0 < p=1 < s=2 < z=3`；数牌内 `rank` 1..9 升序、字牌内 `1z`..`7z` 升序；同牌面副本相邻。
示例：`1m 1m 2m 5m 9m 3p 3p 4p 1s 2s 2s 7s 9s 1z 5z 6z`。

**服务端 MUST 在 `game_start.hand`、`state_sync.hand`、`state_sync.seats[].melds[].tiles`、`game_end.results[].hand` 中按本规则排序**；客户端 **MUST NOT** 重新排序（UI_UX_SPEC §4.5）。

---

## 3. 消息总览

### 3.1 客户端 → 服务端（10 条，MUST 支持）

| # | `type` | 字段 | 何时发送 |
| --- | --- | --- | --- |
| C1 | `login` | `name` | 建连后第一步 |
| C2 | `create_room` | — | 大厅点「新建房间」 |
| C3 | `join_room` | `room_id` | 大厅点一间还没开局的房间 |
| C4 | `ready` | — | 已入座后点「开始对局」 |
| C11 | `list_rooms` | — | 停在大厅时约每 1 秒一次 |
| C5 | `discard` | `tile`, `seq` | 收到 `window=="discard"` 的 `action_request` 后 |
| C6 | `action` | `action`, `tiles[]`, `seq` | 收到 `action_request` 后（动作须在其 `options` 内；v1.6 动作词表见 §4.6） |
| C7 | `sync` | — | 任意时刻（错误恢复/对齐） |
| C8 | `ping` | — | 每 5000 ms |
| C9 | `register` | `username`, `password`, `name?` | 账号注册（v1.3，`ACCOUNT_DESIGN.md` §3.2）；注册成功即登录 |
| C10 | `stats_request` | — | 登录后任意时刻（v1.3，查询本人累计战绩） |

### 3.2 服务端 → 客户端（14 条）

| # | `type` | 字段 | 收件人 |
| --- | --- | --- | --- |
| S1 | `login_ok` | `player_id`, `name` | 本人 |
| S2 | `error` | `code`, `message`, `detail?` | 相关连接 |
| S3 | `room_joined` | `room_id`, `seat`, `seats[]` | 本人 |
| S4 | `game_start` | `hand[]`, `seat`, `dealer`, `round_index`, `round_count`（v1.6 增两个） | 每人一份（含本家手牌） |
| S5 | `draw` | `tile`, `seat`, `seq` | **仅摸牌者本人** |
| S6 | `discard_event` | `seat`, `tile`, `seq` | 广播 4 家 |
| S7 | `action_request` | `seq`, `deadline_ms`, `window`, `options[]` | 仅窗内相关座位 |
| S8 | `action_event` | `seat`, `action`, `tiles[]`, `from_seat?`, `seq` | 广播 4 家 |
| S9 | `state_sync` | `hand[]`, `melds[]`, `seats[]`, `remaining`, `current_seat`, `seq` | 每人一份（他家不含 `hand`；暗杠牌面对他家为空） |
| S10 | `game_end` | `reason`, `results[4]`, `win`（**局终**；v1.6 只增字段，见 §5.10） | 广播 4 家（**无 `seq`**） |
| S11 | `pong` | — | 对应 `ping` |
| S13 | `stats` | `games`, `wins`, `draws`, `net_score`, `recent[]` | 请求者本人（v1.3） |
| S14 | `room_list` | `rooms[]` | 请求 `list_rooms` 的连接（v1.5） |
| S15 | `match_end` | `hands_played`, `renchan_count`, `rankings[]` | 广播 4 家（**场终**，v1.6；**无 `seq`**） |

> 表中 `S12` 不存在：本版本共 13 类服务端帧 + `pong` 计 **14 条**（v1.6 由 13 增补 `match_end`），与 GAME_DESIGN §11.2 的对应关系见该节。
> （编号沿用 v1.0 的历史序：`S12` 从未存在，v1.3 新增的 `stats` 取 `S13`，v1.6 新增的 `match_end` 取 `S15`。）

---

## 4. 客户端 → 服务端（逐条字段规范）

### 4.1 `login`

```json
{"type":"login","name":"玩家_"}
```

| 字段 | 类型 | 必填 | 校验 |
| --- | --- | --- | --- |
| `name` | string | 是 | 1..16 个 UTF-8 字符；不含 `\n`/`\r`/控制字符（`0x00`–`0x1F`） |

**账号形态（v1.3 增补，`ACCOUNT_DESIGN.md` §3.1）**

```json
{"type":"login","username":"alice","password":"s3cret"}
```

| 字段 | 类型 | 必填 | 校验 |
| --- | --- | --- | --- |
| `username` | string | 账号形态必填 | 3..16 个字符，仅 `[A-Za-z0-9_]` |
| `password` | string | 账号形态必填 | 6..64 个字符，仅可见 ASCII（`0x20`–`0x7E`） |

- 判定：帧中出现 `username` 即走账号形态（此时忽略 `name`）；否则走上面的游客形态（行为与 v1.2 逐字相同）。
- 账号形态成功 → `login_ok{player_id:"u-<id>",name:<昵称>}`（字段形状与游客形态相同，仅取值不同）。
- 账号不存在与口令错误 → **同一个** `error{code:"AUTH_FAILED"}`（不泄露账号是否存在），连接保持；该错误计入 §7.2 的非法操作计数。
- 该账号已在别处登录 → `error{code:"ALREADY_ONLINE"}`，连接保持。
- 账号形态是异步的：发出后到 `login_ok` 之前，该连接上除 `login`/`register` 外的帧仍一律 `NOT_LOGGED_IN`；期间再发 `login`/`register` → `BAD_REQUEST`（`message:"already logged in"`）。
- MySQL 不可用 → `error{code:"INTERNAL"}`（`message:"account service unavailable"`），**不计配额**（`ACCOUNT_DESIGN.md` §2.3）。

- 成功 → `login_ok`；失败 → `error{code:"INVALID_NAME"}`，连接保持；第 4 次非法昵称 → 断开（§1.7）。
- 同一连接重复 `login` → `error{code:"BAD_REQUEST"}`（`message:"already logged in"`），连接保持。
- 未登录发送除 `login` 外的任何帧 → `error{code:"NOT_LOGGED_IN"}`。

### 4.2 `create_room`

```json
{"type":"create_room"}
```

- 行为：创建房间，调用者占据 `seat = 0`；`seat = 1,2,3` 先是 AI（`is_bot = true`，昵称 `AI-1`/`AI-2`/`AI-3`）。**不**发牌、**不**把房间标成已开局。
- 成功 → `room_joined`；已经在任意房间里 → `error{code:"BAD_REQUEST"}`（`message:"already in a room"`）；未登录 → `NOT_LOGGED_IN`。
- `room_id` 格式：`r-####`（4 位十进制，进程内从 1 递增）。

### 4.3 `join_room`

```json
{"type":"join_room","room_id":"r-0001"}
```

- 只在目标房间**还没**收到 `ready` 时成功：把编号最小的 AI 座位换成该玩家，`seat` 为该编号。已经坐在这间房里再发一次 → 再回一帧当前的 `room_joined`，状态不变。
- 已经在别的房间 → `error{code:"BAD_REQUEST"}`（`message:"already in a room"`）。
- 房间不存在、已经开局、或没有剩余 AI 座位 → `error{code:"ROOM_NOT_FOUND"}`，连接保持。
- 未开局时房主断开：房间删除。其他真人断开：该座位恢复为原来的 AI。

### 4.4 `ready`

```json
{"type":"ready"}
```

- 调用者已在一间还没开局的房间里 → 立即开始本局：洗牌、发牌，向每个座位单独下发 `game_start`。空着的座位仍是 AI，和真人一起开局。
- 未登录 → `NOT_LOGGED_IN`；不在房间 → `ROOM_NOT_FOUND`。

### 4.5 `discard`（H2）

```json
{"type":"discard","tile":"4p","seq":7}
```

| 字段 | 类型 | 必填 | 校验（全部通过才改变状态） |
| --- | --- | --- | --- |
| `tile` | string | 是 | 匹配 **§2.1 的合取正则 `^([1-9][mps]\|[1-7]z)$`**（数牌或字牌均可）且**在本家手牌中**；否则 `UNKNOWN_TILE` |
| `seq` | integer | 是 | **必须等于**当前窗口 `seq`；不等 → **静默丢弃**（§6.3，不回 `error`） |

- 仅当**当前存在针对本家的 `window == "discard"` 窗口**且本家为 `current_seat` 时可接受；否则 `error{code:"NOT_YOUR_TURN"}`。
- 副作用：本家手牌移除 1 张该牌面 → 加入本家牌河 → 关闭窗口 → `seq++` → 广播 `discard_event{seat,tile,seq}`。
- 打牌即「放弃自摸」的唯一正确表达（打牌窗口不接受 `pass`，见 §4.6）。

### 4.6 `action`（H1、H2）

```json
{"type":"action","action":"pong","tiles":["9s"],"seq":2}
```

| 字段 | 类型 | 必填 | 校验 |
| --- | --- | --- | --- |
| `action` | string | 是 | `∈ {pass, pong, chow, kong, ankan, kakan, win}`（v1.6 由 5 值增至 7 值） |
| `tiles` | array\<string\> | 是 | 见下表「`tiles` 语义」 |
| `seq` | integer | 是 | 必须等于当前窗口 `seq`；不等 → 静默丢弃 |

**窗口 × `action` 合法性矩阵（H1，冻结；v1.6 扩列）**

| 窗口 | `win` | `kong` | `ankan` | `kakan` | `pong` | `chow` | `pass` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `window == "discard"`（自家打牌窗口） | ✅（仅当 `options` 含 `win`） | ❌ | ✅（仅当含 `ankan`） | ✅（仅当含 `kakan`） | ❌ | ❌ | **❌ `ILLEGAL_ACTION`** |
| `window == "claim"`（鸣牌窗口） | ✅（仅当 `options` 含 `win`） | ✅（仅当含 `kong`） | ❌ | ❌ | ✅（仅当含 `pong`） | ✅（仅当含 `chow`） | ✅（恒在 `options` 内） |

- 打牌窗口的**合法回复**：`discard{tile,seq}`，或 `options` 中**实际出现**的自理动作之一——`win`（自摸/天和）、`ankan`、`kakan`（v1.6 新增后两者）。
  回 `action{action:"pass",seq}` → `error{code:"ILLEGAL_ACTION"}`（`pass` 不在该窗口 `options` 中）。
- **判定口径不变（H8）**：合法性一律**按窗口下发的 `options` 判定**，不按服务端重算的"还能做什么"判定；`options` 未提供的动作即便规则上可行也是 `ILLEGAL_ACTION`。
- **暗杠/加杠只出现在 `discard` 窗口**（即本家自己的回合）；`claim` 窗口永不提供 `ankan`/`kakan`。
- `action` 不在该窗口 `options` 中（或 `tiles` 不满足条件）→ `error{code:"ILLEGAL_ACTION"}`，状态零变更。
- 座位不是当前窗口收件人 → `error{code:"NO_PENDING_ACTION"}`。

**`tiles` 语义（冻结；v1.6 增两行）**

| `action` | `tiles` | 服务端校验 |
| --- | --- | --- |
| `win` | `[]` | 和牌张由上下文确定（荣和 = 被和的那张；自摸 = 刚摸到的牌）；**服务端必须重算**（§8.3） |
| `kong` | `[T]`（被鸣的那张牌） | `count(T, 手牌) == 3` 且 `remaining ≥ 1` |
| `ankan` | `[T]`（**本家手牌中凑满 4 张的牌面**） | `count(T, 手牌) == 4` 且 `remaining ≥ 1`；本家此刻必须是行动态（`concealed + 3×melds = 14`）。`T` 可以是数牌或字牌 |
| `kakan` | `[T]`（**已有碰组的牌面**） | 本家存在一组 `pong{T,T,T}` 且 `count(T, 手牌) ≥ 1` 且 `remaining ≥ 1`；**不设抢杠窗口**（§0.4） |
| `pong` | `[T]`（被鸣的那张牌） | `count(T, 手牌) ≥ 2` |
| `chow` | `[a,b]`（**手中的两张**，均为数牌） | 本家为打出者的下家（`(S+1)%4`）、**`T` 为数牌**、且 `{a,b,T}` 构成**同一数牌花色**的顺子（**字牌不可吃**，§2.1） |
| `pass` | `[]` | 无需校验（仅 `claim` 窗口） |

- `tiles` 中的每个元素都必须匹配 §2.1 的合取正则 `^([1-9][mps]|[1-7]z)$`；否则 `error{code:"UNKNOWN_TILE"}`（`chow` 的 `tiles` 元素必须为**数牌**，否则 `ILLEGAL_ACTION`）。
- `ankan`/`kakan` 的 `tiles` **恰好 1 个元素**（与 `kong` 同形）；元素多于 1 个 → `ILLEGAL_ACTION`。`ankan` 只报**牌面**（如 `"5m"`），不报具体副本（§2.2 副本不可区分）。
- **副露后的动作语义（v1.6）**：`ankan` 成功后本家 `melds[]` 追加一组 `{type:"ankan", tiles:[T×4]}`（对他家为空数组，见 §5.9）；`kakan` 成功后**替换**原来那组 `pong` 为 `{type:"kakan", tiles:[T×4]}`（组数不变、顺序位置不变）。两者之后都**立即补摸 1 张**（`remaining--`）再开打牌窗口（`RULESET.md` §1.5、§4.3）。
- `action` 的 `tiles` 与随后广播的 `action_event.tiles` **语义相同**（§5.8）。

### 4.7 `sync`

```json
{"type":"sync"}
```

- 行为：仅向**请求者**回一帧该座位的 `state_sync`（本家手牌 + 公开信息 + `seq`），不广播。

### 4.8 `ping`

```json
{"type":"ping"}
```

- 行为：回 `pong`；用于 RTT 计算与心跳保活。计入入站限速（§1.7）。

### 4.9 `register`（v1.3 新增，`ACCOUNT_DESIGN.md` §3.2）

```json
{"type":"register","username":"alice","password":"s3cret","name":"爱丽丝"}
```

| 字段 | 类型 | 必填 | 校验 |
| --- | --- | --- | --- |
| `username` | string | 是 | 3..16 个字符，仅 `[A-Za-z0-9_]` |
| `password` | string | 是 | 6..64 个字符，仅可见 ASCII（`0x20`–`0x7E`） |
| `name` | string | 否 | 昵称；缺省取 `username`；给出时按 §4.1 昵称规则校验（1..16 字符、无控制字符） |

- **注册成功即登录**：成功时直接下发 `login_ok{player_id:"u-<id>",name:<昵称>}`（**不新增 `register_ok`**），
  客户端后续流程与游客完全一致（`create_room` → `ready`）。
- 账号名已存在 → `error{code:"USERNAME_TAKEN"}`，连接保持，可换名重试（不计配额）。
- 字段缺失/类型错/校验不过 → `error{code:"BAD_REQUEST"}`。
- 已登录、或账号登录/注册正在途中 → `error{code:"BAD_REQUEST"}`（`message:"already logged in"`）。
- MySQL 不可用 → `error{code:"INTERNAL"}`（不计配额）。

### 4.10 `stats_request`（v1.3 新增）

```json
{"type":"stats_request"}
```

- 无字段。成功 → 下发一帧 `stats`（§5.13）。
- **游客也可发**：游客没有账号，回**零值**载荷（`games`/`wins`/`draws`/`net_score` 全 0、`recent` 为空数组），
  不是错误——客户端因此不必区分会话类型。
- 未登录 → `error{code:"NOT_LOGGED_IN"}`；账号服务不可用 → `error{code:"INTERNAL"}`。

### 4.11 `list_rooms`（v1.5）

```json
{"type":"list_rooms"}
```

- 无字段。成功 → 一帧 `room_list`（§5.14）。只包含还没 `ready` 的房间。
- 未登录 → `error{code:"NOT_LOGGED_IN"}`。客户端在大厅约每 1 秒发一次，不得高于入站限速。

---

## 5. 服务端 → 客户端（逐条字段规范）

### 5.1 `login_ok`

```json
{"type":"login_ok","player_id":"p-3f2a1c","name":"玩家_"}
```

- `player_id`：服务端生成的**非空不透明字符串**，用于区分重名玩家。客户端 **MUST NOT** 解析其格式。
  - 游客（`login{name}`）：`p-<6 位十六进制>`，**进程内**唯一，重启即变（与 v1.0 相同）。
  - 账号（`login{username,password}` / `register{...}`，v1.3）：`u-<账号 id>`，**全局**唯一且跨重启不变（`ACCOUNT_DESIGN.md` §1.2）。

### 5.2 `error`

```json
{"type":"error","code":"ILLEGAL_ACTION","message":"该动作当前不可用","detail":{"action":"pass","window":"discard"}}
```

- `code`：必须取自 §7.1 的 **14** 个取值之一（v1.6 订正：原文此处写"11 个取值"，而 §7.1 自 v1.3 起已是 **14 条**；以 §7.1 为准）。
- `message`：人类可读、**不得包含他家手牌等私有信息**。
- `detail`：可选对象；同样不得包含私有信息。

### 5.3 `room_joined`

```json
{"type":"room_joined","room_id":"r-0001","seat":0,
 "seats":[{"seat":0,"name":"玩家_","is_bot":false},
          {"seat":1,"name":"AI-1","is_bot":true},
          {"seat":2,"name":"AI-2","is_bot":true},
          {"seat":3,"name":"AI-3","is_bot":true}]}
```

- `seat`：本连接玩家的座位（0..3）。`seats` 恒为 4 项，按 `seat` 升序。
- 收件人：本人。客户端 **MUST** 缓存 `seats[].name`（结算卡片需要，GAME_DESIGN §6.5）。

### 5.4 `game_start`

```json
{"type":"game_start","seat":0,"dealer":0,"round_index":0,"round_count":4,
 "hand":["1m","1m","2m","3m","4m","5m","6p","6p","7p","8p","9s","9s","9s","9s"]}
```

| 字段 | 说明 |
| --- | --- |
| `hand` | **本家**手牌，按 §2.2 排序；长度 13（非庄家）或 **14**（庄家 = `dealer`） |
| `seat` | 本家座位 |
| `dealer` | **本局**庄家座位（0..3）。v1.6 起**不再恒为 0**：庄家按局轮转，连庄时不变；取值口径见 `RULESET.md` A 批修订版 |
| `round_index` | **v1.6 新增**：本局在整场中的序号，**0 基**（0..`round_count − 1`）。连庄**不**增加该值（连庄仍属同一局序号） |
| `round_count` | **v1.6 新增**：本场固定局数，恒为 **4** |

- 收件人：**逐座位单独下发**（每人一份，只含自己的手牌）。绝不广播同一份内容。
- 起始牌山的剩余张数由 `RULESET.md` §1 冻结；服务端在首个 `state_sync` 中给出。
- **v1.6 时序契约**：一场内每局开始时**各下发一次** `game_start`（第 2..4 局与第 1 局形状完全相同，仅字段值变化）。客户端收到 `game_start` 即视为"新一局开始，本地牌桌状态整体重置"；局间收到的 `state_sync` 不得跨局沿用。
- `round_index` / `round_count` 是**低频**信息（每局一次）；`state_sync` **不携带**任何场次进度字段（见 §5.9 的性能注），场次总分只在 `game_end` / `match_end` 下发。

### 5.5 `draw`（H5）

```json
{"type":"draw","tile":"7m","seat":0,"seq":9}
```

- 收件人：**仅摸牌者本人**（他家不得收到，UI 只能从其后打出的牌推断）。
- `seq` = 该摸牌所属窗口（摸牌后立即开窗，见 §6.1）。
- 杠补牌同样使用 `draw` 帧（`tile` = 补牌）。

### 5.6 `discard_event`（H5）

```json
{"type":"discard_event","seat":1,"tile":"4p","seq":7}
```

- 收件人：广播 4 家（牌河为公开信息）。
- 不含 `state_sync` 之外的额外字段；服务端 **不**因打牌额外下发 `state_sync`（GAME_DESIGN §11.4）。

### 5.7 `action_request`

```json
{"type":"action_request","seq":7,"deadline_ms":1791174514000,"window":"claim",
 "options":[{"action":"win","tiles":[]},
            {"action":"pong","tiles":["4p"]},
            {"action":"pass","tiles":[]}]}
```

| 字段 | 说明 |
| --- | --- |
| `seq` | 本窗口序号（客户端 **MUST** 在 `discard`/`action` 中原样回带） |
| `deadline_ms` | 窗口截止时刻（epoch ms UTC） |
| `window` | `"discard"` 或 `"claim"`（**仅此两值**；暗杠/加杠**不新增窗口类型**，它们是 `discard` 窗口的附加 `options`） |
| `options` | 数组，元素为 `{"action":<动作>, "tiles":[...]}`；**顺序固定** `win → kong → ankan → kakan → pong → chow → pass`（v1.6 在 `kong` 与 `pong` 之间**插入**两项，原有五项的相对顺序逐字不变）；`chow` 可出现多条（每个合法组合一条）。每个窗口只出现其适用的子集 |

- 收件人：`window=="claim"` 时**仅发给存在非 `pass` 选项的座位**；不存在非 `pass` 选项时**不下发任何帧**且窗口立即关闭。
- `window=="discard"` 时**一定发给当前行动座位**，其 `options` 为以下动作的**子集**（各动作的出现条件由 `RULESET.md` §5.2 冻结）：
  - `{"action":"win","tiles":[]}` —— 自摸/天和成立时；
  - `{"action":"ankan","tiles":["<T>"]}` —— 手牌凑满 4 张同面且 `remaining ≥ 1` 时（每个可暗杠的牌面一条，按 §2.2 牌面升序）；
  - `{"action":"kakan","tiles":["<T>"]}` —— 存在可加杠的碰组且 `remaining ≥ 1` 时（每个可加杠的牌面一条，按 §2.2 牌面升序）；
  - **可能为空 `[]`**（只能打牌）。「打牌」本身**不进入 `options`**。
- **v1.6 补充**：`options` 中出现 `ankan`/`kakan` 时，超时代打的默认动作仍然是「打牌」而**不是**杠（§6.4）——即**超时永不选杠**，选项存在只代表"可以选"，不代表"会被代选"。
- 同一时刻全房间最多一个未决窗口（由 `seq` 唯一标识）。
- 超时代打口径见 §6.4。

### 5.8 `action_event`（H3、H5、H9）

```json
{"type":"action_event","seat":1,"action":"pong","tiles":["9s"],"from_seat":0,"seq":2}
{"type":"action_event","seat":2,"action":"win","tiles":["8p"],"from_seat":3,"seq":11}
{"type":"action_event","seat":0,"action":"win","tiles":["5m"],"seq":12}
{"type":"action_event","seat":0,"action":"ankan","tiles":["5m"],"seq":13}
{"type":"action_event","seat":0,"action":"kakan","tiles":["7p"],"seq":15}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `seat` | integer | **执行该动作的座位**（鸣牌者 / 自理杠者 / 和牌者） |
| `action` | string | `∈ {pong, chow, kong, ankan, kakan, win}`（**`pass` 永不出现**；v1.6 由 4 值增至 6 值） |
| `tiles` | array | `pong`/`kong`/`ankan`/`kakan` = `[T]`；`chow` = `[a,b]`（手中的两张）；`win` = `[和牌张]` |
| `from_seat` | integer | 被鸣 / 被荣和那张牌的**打出者座位**；`pong`/`chow`/`kong`/荣和 **必带**；**自摸和牌、`ankan`、`kakan` MUST NOT 携带**（省略等同于 `null`）——三者都没有"被鸣的那张牌" |
| `seq` | integer | 触发该动作的窗口序号（= 对应 `action_request.seq`） |

**冻结规则**：

1. `pass` **不产生** `action_event`；窗口结算为「全部 `pass`」时不广播任何 `action_event`，直接进入下一家摸牌。
2. 被鸣牌**恒为** `from_seat` 座位最近一次打出的那张（鸣牌窗口正是由它触发）。客户端据此从 `from_seat` 的牌河移除**最后一张**即可：既不必匹配 `tiles[0]`，也不必猜测 `chow` 的被鸣牌归属。
3. `action_event` 是**即时事件**；服务端权威状态以随后到达的 `state_sync` 为准，两者**到达顺序不作保证**（H9）。
4. **v1.6：`ankan` / `kakan` 不移动任何牌河**（无 `from_seat`）。客户端处理这两类事件时**不得**从任何牌河移除牌；其副露变化一律等随后的 `state_sync.melds[]` 权威覆盖（§5.12 保证自理杠之后必定下发 `state_sync`）。
   > 这条是**硬约束**：若把暗杠实现成 `action:"kong"` 并带 `from_seat`，旧客户端会误删放炮者的牌河（`game_state.py` 的 `pong/chow/kong` 分支），属于必须避免的兼容性事故。

### 5.9 `state_sync`（H4、H6）

```json
{"type":"state_sync","seq":9,"remaining":83,"current_seat":1,
 "hand":["1m","2m","3m","5p","5p"],
 "melds":[{"type":"pong","tiles":["5p","5p","5p"]}],
 "seats":[{"seat":0,"name":"玩家_","is_bot":false,"hand_count":10,"discards":["9s","4p"],
           "melds":[{"type":"pong","tiles":["9s","9s","9s"]}],"disconnected":false},
          {"seat":1,"name":"AI-1","is_bot":true,"hand_count":13,"discards":[],"melds":[],"disconnected":false},
          {"seat":2,"name":"AI-2","is_bot":true,"hand_count":13,"discards":[],"melds":[],"disconnected":false},
          {"seat":3,"name":"AI-3","is_bot":true,"hand_count":13,"discards":[],"melds":[],"disconnected":true}]}
```

| 字段 | 收件人 | 说明 |
| --- | --- | --- |
| `hand` | 仅本家 | **本家**手牌（按 §2.2 排序）。**他家的 `hand` 字段 MUST NOT 存在**（H6） |
| `melds` | 仅本家 | 本家副露数组，元素 `{"type":<pong\|chow\|kong\|ankan\|kakan>,"tiles":[...]}`（H4，v1.6 扩 2 值）。**本家的 `ankan` 带 4 张真实牌面** |
| `seats[]` | 全体公开 | 恒 4 项，按 `seat` 升序；每项 `{seat,name,is_bot,hand_count,discards[],melds[],disconnected}`，**不含 `hand`**。**他家的 `ankan` 元素 `tiles` 恒为 `[]`**（见下） |
| `remaining` | 全体公开 | 牌山剩余张数（初始值见 `RULESET.md` §1） |
| `current_seat` | 全体公开 | 当前行动座位（0..3） |
| `seq` | 全体公开 | 当前窗口序号（见 §6.2）。**场次内跨局单调，不重置** |

**`melds[]` 元素形状（H4；v1.6 由 3 值扩到 5 值）**：`type ∈ {pong, chow, kong, ankan, kakan}`，组顺序 = 成组时间序。

| `type` | `tiles` 张数（本家视角） | `tiles` 张数（他家视角） | 说明 |
| --- | --- | --- | --- |
| `pong` | 3 | 3 | 碰，公开 |
| `chow` | 3 | 3 | 吃，公开 |
| `kong` | 4 | 4 | **明杠**（鸣他家打出的牌），公开 |
| `ankan` | **4** | **`[]`（空数组）** | **暗杠**：牌面对他家不可见（v1.6 终裁）。`hand_count` 仍按"1 组副露"计，故 `hand_count + 3×melds ∈ {13,14}` 的不变量不变 |
| `kakan` | 4 | 4 | **加杠**：由一组 `pong` **原位替换**而来（组数与位置不变），公开 |

- **暗杠的唯一实现口径（v1.6 终裁，MUST）**：`tiles` 的合法长度只有两种，且**按阶段绑定**，不是"任选其一"——
  - **本家视角**：恒为 **4 张真实牌面**（自己的牌，任何时候都可见）。
  - **他家视角 + 局终揭示之前**：恒为 **`[]`**。服务端在此期间**只发空数组**（这是唯一口径，不是可选优化）。
  - **他家视角 + 局终揭示那一份**：恒为 **4 张真实牌面**。
  - **客户端 MUST 同时支持 0 张与 4 张两种长度**的渲染（0 张画 4 张牌背，4 张画牌面）——这条是给客户端的硬要求：即便某天服务端策略变化，客户端也不得只处理一种形状。
- **暗杠的揭示时序契约（v1.6，MUST）**：
  1. 暗杠发生后的所有 `state_sync`（含 `sync` 单发）中，**他家** `seats[].melds[]` 里该元素恒为 `{"type":"ankan","tiles":[]}`。
  2. **局终揭示**：`game_end` 之前的**最后一份** `state_sync`（即 §5.12「和牌后 / 荒庄前」那一份）**必须**把**所有座位**的 `ankan` 补全为 4 张真实牌面，使客户端在结算页能画出完整手牌。**复用这份既有的 `state_sync`，不新增帧、不新增字段。**
  3. `game_end.results[].hand`（扁平手牌，长度 = 14 + 杠数）**总是**包含暗杠的 4 张（局已结束，不再有私有信息）。
  4. 违反 1.（提前泄露）或 2./3.（结算不完整）都按协议缺陷处理。
- **性能注（v1.6，架构约束）**：`state_sync` 是热路径（每次摸牌/副露都向 4 家各发一份）。场次进度（局序号、累计分、庄家）**不进** `state_sync`，由低频帧 `game_start` / `game_end` / `match_end` 承载。实现方不得为"牌桌上常显累计分"而给 `state_sync` 加字段。

### 5.10 `game_end`

> **v1.6：`game_end` 是「局终」帧**，一场 4 局会下发 4 次（连庄各算一次）。字段只增不改；完整形状示例见本节末。v1.5 形状（无 `round_index`/`match_total`/`yaku` 等新增字段）对旧客户端仍然可解析——新增字段一律可忽略。

```json
{"type":"game_end","reason":"win_ron",
 "results":[{"seat":0,"delta_score":-4,"hand":[],"win_type":[]},
            {"seat":1,"delta_score":4,
             "hand":["1m","1m","3m","3m","5m","5m","7m","7m","2p","2p","6p","6p","8p","8p"],
             "win_type":["seven_pairs","ron"]},
            {"seat":2,"delta_score":0,"hand":[],"win_type":[]},
            {"seat":3,"delta_score":0,"hand":[],"win_type":[]}],
 "win":{"seat":1,"tile":"8p","from_seat":0,"tsumo":false,
        "tags":["seven_pairs"],"multiplier":4,"per_payer":4,"total":4}}
```
> 上面这份是 **v1.5 形状（保留作为"既有字段不动"的对照）**；v1.6 追加的字段见本节末的完整示例。

| 字段 | 冻结语义 |
| --- | --- |
| `reason` | `"win_ron"` / `"win_tsumo"` / `"draw_exhausted"` 三者之一（**词表不变**） |
| `results` | 恒 4 项，按 `seat` 升序，`seat` 覆盖 `{0,1,2,3}` 无重复 |
| `results[].delta_score` | 整数；四家之和 **MUST 为 0**（`RULESET.md` §6） |
| `results[].hand` | 和牌者 = **扁平数组**（门前手牌 + 副露牌面，按 §2.2 排序，含和牌张，长度 = `14 + 杠数`）；其他人 `[]`。**不替代** `melds[]`，两者并存（H4） |
| `results[].win_type` | 和牌者 = `tags` + `["ron"]` 或 `["tsumo"]`（牌型 tags 在前，和牌方式在最后）；其他人 `[]` |
| `win` | 和牌时为对象，荒庄时为 `null` |
| `win.seat` / `win.tile` | 和牌者座位 / 和牌张（荣和 = 被和的那张；自摸 = 摸到的牌） |
| `win.from_seat` | 荣和 = 放炮者座位；**自摸 = `null`（字段必须存在）** |
| `win.tsumo` | 布尔 |
| `win.tags` | **旧语义原样保留**：只含 `seven_pairs` / `pure_one_suit` 两个旧 id（顺序 `seven_pairs` 在前），供旧客户端渲染中文标签；恒为 `win.yaku[].id` 的**子集**。荒庄/他家为 `[]`（v1.6：**不**把全部番种塞进 `tags`，番种明细的唯一出口是 `win.yaku[]`） |
| `win.multiplier` | **保留字段名，语义不变（支付倍率）**：荣和 = `per_payer`；自摸 = `per_payer / 2`。v1.6 起 = `R`（由番数合计推导），**不再限制为 `{1,4,16}`** |
| `win.per_payer` | 荣和 = 放炮者实付；自摸 = 每家实付（`RULESET.md` §6 A 批修订版） |
| `win.total` | 和牌者实得**分数**（= 三家实付之和）。**注意：此字段不是番数**；番数合计见 `win.han_breakdown.total`，两者名字不同、含义不同，实现方不得混用 |

**v1.6 新增字段（只增，不改既有形状）**

| 字段 | 类型 | 语义 |
| --- | --- | --- |
| `round_index` | int | 本局序号，0 基（与 `game_start.round_index` 同值） |
| `dealer` | int | **本局**庄家座位（0..3） |
| `renchan_count` | int | **截至本局结束**已发生的连庄次数（首次开局为 0；庄家和牌或荒庄则下一局 +1，见 `RULESET.md` A 批修订版） |
| `match_will_continue` | bool | `true` = 本场还有下一局（服务端稍后会下发新的 `game_start`）；`false` = 本场结束，随后下发 **`match_end`**（§5.15） |
| `results[].match_total` | int | 该座位在**本场**的累计得失分（含本局）：`Σ 本场各局 delta_score`，四家之和恒为 0 |
| `win.yaku` | array | 番种明细：`[{"id":<string>,"han":<int>}, …]`。**顺序 = `RULESET.md` 番种表的冻结判定序**（即结算页渲染序）；**每个 `id` 至多出现一条**（`yakuhai` 命中 n 组也合并为一条、`han = n`）。`Σ yaku[].han == win.han_breakdown.base`。荒庄/他家无此字段 |
| `win.han_breakdown` | object | 番数四段式：`{"base":<int>,"tsumo":<int>,"dealer":<int>,"total":<int>}`。`base` = 牌型番合计（= Σ `yaku[].han`）；`tsumo` = 自摸加成（0/1）；`dealer` = 庄家加成（0/1）；`total` = `min(base + tsumo + dealer, 6)`（**封顶 6 番**）。荒庄/他家无此字段 |
| `win.han` | int | **= `win.han_breakdown.total`**（合计番数，含加成、封顶 6）。保留作单值便利字段，与 `han_breakdown.total` 恒等。荒庄/他家无此字段 |
| `win.dealer_bonus` | bool | **派生便利字段**：等价于 `win.han_breakdown.dealer == 1`。荒庄/他家无此字段 |
| `win.rinshan` | bool | **派生便利字段**：等价于 `win.yaku[]` 含 `id == "rinshan"`。荒庄/他家无此字段 |

**`win.yaku[].id` 的冻结词表（v1.6，14 个，顺序即判定序）**

`menzen`（门前清）→ `tanyao`（断幺九）→ `pinfu`（平和）→ `iipeikou`（一杯口）→ `yakuhai`（箭刻）→ `rinshan`（杠上开花）→ `sanshoku_doujun`（三色同顺）→ `ittsuu`（一气通贯）→ `toitoi`（对对和）→ `sanankou`（三暗刻）→ `honitsu`（混一色）→ `shousangen`（小三元）→ `seven_pairs`（七对子）→ `pure_one_suit`（清一色）。

- 这 14 个 id 是**线协议词汇**（客户端按 id 渲染中文标签），因此在此冻结；**每个 id 的番值与判定条件以 `RULESET.md` 番种表为唯一权威**，本文不重复其数值，避免两处漂移。
- `tsumo`（自摸 +1）与庄家加成（+1）**不是** `yaku[]` 元素：自摸已由 `win.tsumo` 表达，庄家是身份加成不是牌型，两者只出现在 `win.han_breakdown` 的对应字段里。客户端必须把"番种明细 + 自摸 + 庄家 = 合计"渲染成一段可加和的式子。
- 旧 id `seven_pairs` / `pure_one_suit` 的**字符串与语义均不变**（七对子仍是七对子；清一色仍是"零字牌"），因此旧客户端的标签映射继续可用。

- **`game_end` 不携带 `seq`**，不参与 `seq` 比较（H5）。
- 收件人：广播 4 家（含托管座位；`results` 仍包含它）。
- **v1.6 语义澄清**：`game_end` **仍然只表示"局终"**，一场 4 局会下发 **4 次** `game_end`（连庄也各算一次）。"场终"由新增的 `match_end`（§5.15）表达，两者**不合并、不互相替代**。客户端不得把收到 `game_end` 当作"可以关掉对局界面"。`reason` 词表不因场次化而新增取值。

```json
{"type":"game_end","reason":"win_ron","round_index":1,"dealer":1,"renchan_count":0,"match_will_continue":true,
 "results":[{"seat":0,"delta_score":-M,"hand":[],"win_type":[],"match_total":-M},
            {"seat":1,"delta_score":M,"hand":["…14+杠数 张…"],"win_type":["seven_pairs","ron"],"match_total":M},
            {"seat":2,"delta_score":0,"hand":[],"win_type":[],"match_total":0},
            {"seat":3,"delta_score":0,"hand":[],"win_type":[],"match_total":0}],
 "win":{"seat":1,"tile":"8p","from_seat":0,"tsumo":false,
        "tags":["seven_pairs"],
        "yaku":[{"id":"menzen","han":1},{"id":"seven_pairs","han":2}],
        "han_breakdown":{"base":3,"tsumo":0,"dealer":0,"total":3},
        "han":3,"dealer_bonus":false,"rinshan":false,
        "multiplier":R,"per_payer":R,"total":R}}
```
> `N`/`R` 是**占位符**：番值、支付公式与封顶口径由 `RULESET.md` 番种表与算分节冻结（本文只冻结**字段形状、顺序、子集关系与零和约束**）。要点：①`Σ yaku[].han == han_breakdown.base`；②`han_breakdown.total = min(base+tsumo+dealer, 6)`；③`win.han == han_breakdown.total`；④`tags ⊆ yaku[].id`；⑤`match_total` 与 `delta_score` 在首局相等，从第 2 局起分化。

### 5.11 `pong`

```json
{"type":"pong"}
```

- 对应 `ping`，用于 RTT 显示与心跳保活。

### 5.12 `state_sync` 下发时机（H9）

| 时机 | 是否下发 |
| --- | --- |
| 摸牌后（含杠补牌） | ✅ **并行广播 4 家**（每人一份私有视图） |
| 副露（碰/吃/明杠/**暗杠/加杠**）后 | ✅ 并行广播 4 家 |
| 和牌后（紧随 `game_end`） | ✅ 并行广播 4 家 |
| 荒庄前（紧随 `game_end`） | ✅ 并行广播 4 家（**局终揭示**：这一份必须补全所有 `ankan` 的真实牌面，见 §5.9） |
| 打出牌后 | ❌（已有 `discard_event`） |
| 客户端 `sync` | ✅ 仅回请求者 |
| 窗口开启/结束（仅 `seq` 变化） | ❌ 不额外下发 |
| **局间**（`game_end` 与下一局 `game_start` 之间） | ❌ **不**下发；局间状态由 `game_end.match_will_continue` 与下一局 `game_start` 表达 |
| **场终**（`match_end` 前后） | ❌ 不下发（`match_end.rankings[]` 已含最终分数） |

- 服务端 **MUST** 并行发出多座位推送（`stdexec::when_all` / `exec::async_scope`），**禁止**在循环内串行 `co_await` 逐座位发送。
- 客户端 **MUST** 把 `state_sync` 视为权威整体覆盖（UI_UX_SPEC §4.10）。

### 5.13 `stats`（v1.3 新增，`ACCOUNT_DESIGN.md` §3.4）

```json
{"type":"stats","games":12,"wins":5,"draws":3,"net_score":-14,
 "recent":[{"room_id":"r-0007","reason":"win_ron","delta_score":12,"win":true,"ended_at":1759900000}]}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `games` | int | 已完成对局总数（含荒庄） |
| `wins` | int | 和牌局数（荣和 + 自摸） |
| `draws` | int | 荒庄局数 |
| `net_score` | int | 累计 `delta_score` |
| `recent[]` | array | 最近 ≤10 局，按结束时间**倒序**；元素 `{room_id, reason, delta_score, win, ended_at}`（`ended_at` 为 Unix 秒） |

- 无 `seq`（与 `login_ok`/`room_joined`/`game_end` 同类，见 §6.2）。
- 战绩口径见 `ACCOUNT_DESIGN.md` §4：每局每个**账号座位**一行，游客与 AI 不记录。

### 5.14 `room_list`（v1.5）

```json
{"type":"room_list","rooms":[{"room_id":"r-0001","host":"小明","players":1,"seats":4}]}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `rooms[]` | array | 还没开局的房间；没有则为 `[]` |
| `rooms[].room_id` | string | `r-####` |
| `rooms[].host` | string | `seat 0` 的昵称 |
| `rooms[].players` | int | 已入座的真人数（1..4） |
| `rooms[].seats` | int | 恒为 4 |

- 无 `seq`。已开局的房间不得出现在这帧里。

### 5.15 `match_end`（**v1.6 新增**，场终）

```json
{"type":"match_end","hands_played":4,"renchan_count":1,
 "rankings":[{"seat":1,"final_score":37,"rank":1},
             {"seat":0,"final_score":0,"rank":2},
             {"seat":3,"final_score":0,"rank":2},
             {"seat":2,"final_score":-37,"rank":4}]}
```
> 上例同时演示三件事：`Σ final_score == 0`；并列第 2（`seat 0` 与 `seat 3` 同分，按 `seat` 升序排列）；**并列跳号**（不存在 `rank == 3`）。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `hands_played` | int | 本场**计入局数的**局数（= `round_count`，恒 4；连庄不占局数，故连庄不会让它变大）。**实际下发的 `game_end` 次数 = `hands_played + renchan_count`**（连庄的每一手也各下发一次 `game_end`） |
| `renchan_count` | int | 本场累计**连庄次数**（与最后一次 `game_end.renchan_count` 同值） |
| `rankings[]` | array | 恒 4 项，覆盖 `{0,1,2,3}` 无重复。元素 `{seat, final_score, rank}` |

**`rankings[]` 的冻结排序与并列口径（v1.6）**

1. 排序键：`final_score` **降序**；同分时 `seat` **升序**（座位号小者在前）。
2. `rank` 从 **1** 开始；**并列跳号**（例：两名并列第 1 ⇒ 前两名 `rank = 1`，第三名 `rank = 3`）。即 `rank` = `1 + (final_score 严格大于本座位的人数)`。
3. `Σ final_score == 0`（MUST）——四家累计得失分仍是零和（`RULESET.md` §6）。
4. `final_score` 与最后一次 `game_end.results[].match_total` **逐座位相等**（同一份数据，不得两算）。

- 无 `seq`（与 `game_end` 同类，见 §6.2）；不参与 `seq` 比较。
- 收件人：广播 4 家（含托管座位）。
- **时序**：`match_end` 只在 `match_will_continue == false` 的那一局之后下发，且**每场恰好一次**；`match_end` 之后服务端按 §1.4 第 5 条在 30 秒后回收房间与连接，不再下发任何对局帧。
- **与 `game_end` 的分工**：`game_end` 表达"这一局怎么结束的"（含和牌明细，可能 4 次）；`match_end` 表达"整场谁赢了"（含最终名次，恰好 1 次）。客户端**不得**用 `match_end` 替代 `game_end` 的明细展示。

---

## 6. 动作窗口与 `seq` 作废

### 6.1 窗口类型与唯一性

| 窗口 | `window` | 开窗时机 | 收件人 | `options` |
| --- | --- | --- | --- | --- |
| 打牌窗口 | `"discard"` | 摸牌后 / 庄家第一打 / 副露后待打牌（含**明杠·暗杠·加杠的补牌后**） | 仅 `current_seat` | `[]`，或以下动作的**子集**（相对顺序固定，v1.6）：`win` → `ankan` → `kakan` |
| 鸣牌窗口 | `"claim"` | 某座位打出 `T` 且至少一个他家存在非 `pass` 选项 | 仅存在非 `pass` 选项的座位 | 至少含 `pass` + 各自可选动作（`win`/`kong`/`pong`/`chow`） |

- **v1.6：窗口类型仍只有两种**。暗杠/加杠是**打牌窗口内的附加选项**，不是新窗口——因此 `seq` 语义、超时机制、`window` 字段取值全部不变。新增窗口类型会同时影响协议、客户端白名单与超时兜底，属于**明确避免**的设计。
- 同一时刻全房间**最多一个未决窗口**，由 `room.seq` 唯一标识。
- 窗内所有座位都收到 `pass`；打牌窗口**不含** `pass`（H1）。

### 6.2 `seq` 规则（H2、H5）

1. `room.seq` 在**本场开局**时为 **1**（v1.6：整场内单调递增、**跨局不重置**，见第 5 条），**每个窗口过渡恰好自增一次**：进入一个新窗口时自增；窗口因动作/超时结束而关闭本身**不自增**（其自增由紧随其后的新窗口过渡承担）。单调递增、永不回退。
   > **裁决（captain，t4 提出）**：原文「在窗口开启或窗口结束时自增」与 §9.4（`draw{seq:9}` 与 `action_request{seq:9}` 同值）及 `GAME_DESIGN.md` §4.3（碰后 `state_sync` 与 `DISCARD_WINDOW` 同为 `seq=3`）不自洽；§9.2 的 `action_request{seq:4}` 已据此改为 `seq:3`。同窗口内多帧共用同一 `seq` 是自洽的，客户端按单调比较处理，两种编号在客户端侧行为等价。
2. **携带 `seq` 的服务端帧**：`action_request`、`state_sync`、`action_event`、`discard_event`、`draw`。
3. **不携带 `seq`**：`game_end`（不参与比较）、**`match_end`（v1.6，同 `game_end`）**；`login_ok`/`error`/`room_joined`/`game_start`/`pong` 亦无 `seq`。
4. 客户端 **MUST** 在 `discard`/`action` 中回带最近一次收到的 `action_request.seq`。
5. **v1.6：`seq` 在整场内单调递增，跨局不重置。** 下一局的第一份 `action_request` 的 `seq` **严格大于**上一局任何一帧的 `seq`。这不是新语义，而是 §6.2 第 1 条「单调递增、永不回退」在"一场多局"下的直接推论；实现上等价于：**每局开局不得把 `seq` 归零**（重新从 1 开始会直接违反本条，并破坏客户端"严格更大才推进窗口"的判定）。
   - 客户端**不得**依赖"每局从 1 开始"来做本地重置；本地重置的唯一信号是 `game_start`（§5.4）。
   - 局间**没有**未决窗口，故局间不存在任何 `action_request`；`game_end` / `match_end` 不参与 `seq` 比较，客户端不会因局终而误判窗口。

### 6.3 入站动作的判定表（冻结）

| 输入情形 | 处理 |
| --- | --- |
| `seq != room.seq`（窗口已结束/已被推进） | **静默丢弃**，不回 `error`，状态零变更 |
| `seq` 匹配，但座位不是当前窗口收件人 | `action` → `NO_PENDING_ACTION`；`discard`（轮不到他）→ `NOT_YOUR_TURN` |
| `seq` 匹配且座位在窗口内，但动作不在其 `options` 中 | `ILLEGAL_ACTION` |
| `seq` 匹配、座位在窗口内、动作合法 | 接受，按 `RULESET.md` §5.4 优先级裁决 |
| 缺 `seq` 或 `seq` 类型非法 | `BAD_REQUEST` |
| 牌编码非法 / 不在手牌 | `UNKNOWN_TILE` |
| **v1.6**：`seq` 匹配、座位在打牌窗口内，但回 `pass` | `ILLEGAL_ACTION`（`pass` 永不在打牌窗口 `options` 内） |
| **v1.6**：`seq` 匹配、座位在**鸣牌**窗口内，却回 `ankan`/`kakan` | `ILLEGAL_ACTION`（自理杠只在打牌窗口） |
| **v1.6**：`seq` 匹配、座位在打牌窗口内，回 `pong`/`chow`/`kong` | `ILLEGAL_ACTION`（鸣牌动作只在鸣牌窗口） |

> 理由：**迟到**（`seq` 过期）的合法回复是「`seq` 作废」方案的正常产物，必须静默丢弃；「`seq` 正确但越权/非法」才是真错误，必须回 `error`。

### 6.4 超时代打（服务端代为执行，确定性）

| 窗口 | 座位 | 超时行为 |
| --- | --- | --- |
| `discard` | 有摸牌记录（含杠补牌） | 放弃 `win`/`ankan`/`kakan` → 打出**刚摸到的那张牌** |
| `discard` | 无摸牌记录（庄家第一打 / 碰吃后待打牌） | 打出**手牌排序后的最后一张**（最大牌） |
| `claim` | 窗内某座位 | 记 `pass`（不鸣牌、不和牌） |
| `claim` | 全部座位超时 | 全部 `pass` → 移交给 `S+1`，进入其摸牌流程 |
| 任意窗口 | 托管座位（`disconnected == true`） | **立即**执行同口径默认动作，**不等** 15 s |
| **任意窗口** | **局间（无窗口）** | 无需任何动作：局间不接受入站对局帧，也不派发超时消息（`seq` 未变，任何迟到超时消息按上表第一行静默丢弃） |

- **v1.6 冻结：超时与托管永不选杠。** `options` 中出现 `ankan`/`kakan` 只代表玩家**可以**选；自动路径（超时代打、托管、AI 座位）一律按上表打牌，不杠。理由：杠会额外摸牌并改变牌山消耗节奏，让"超时=确定性回退"这条可复现契约变复杂；且 `GAME_DESIGN §5.6.2` 的默认动作口径本就是"打牌"。

### 6.5 框架约束（后续实现必须遵守）

1. 不得依赖任何取消/撤回/抢占机制；超时一律走 `seq` 作废（§0.5）。
2. 不得在循环内串行 `co_await` 逐座位推送（§5.12）。
3. AI 与托管决策必须在 RoomActor 单次消息处理内同步完成，不得 `co_await` 其他 actor 的返回。

---

## 7. 错误码与配额记账

### 7.1 错误码表（**14 条**，冻结；H7；v1.3 由 11 条增补；**v1.6 未增补，仍 14 条**）

> **v1.6 事实订正**：本版**不新增任何错误码**。此前若有文档/任务描述称"现有 11 条错误码"，那是 v1.3 之前的旧数字——自 v1.3 起就是 **14 条**（新增 `AUTH_FAILED` / `USERNAME_TAKEN` / `ALREADY_ONLINE`），权威定义见下表。暗杠/加杠的全部非法情形都复用既有的 `ILLEGAL_ACTION` / `NO_PENDING_ACTION` / `NOT_YOUR_TURN` / `UNKNOWN_TILE` / `BAD_REQUEST`，**不引入新码**；场次/连庄同理。`STALE_SEQ` 依旧不作为错误码（§6.3 静默丢弃）。

| `code` | 触发 | 连接是否保持 |
| --- | --- | --- |
| `INVALID_NAME` | `login.name` 为空/超 16 字符/含控制字符 | 保持（第 4 次断开） |
| `NOT_LOGGED_IN` | 未 `login` 就发送其他帧 | 保持 |
| `ROOM_NOT_FOUND` | `join_room` 目标不存在/已关闭/满座；不在房间却发房间内消息 | 保持 |
| `NOT_YOUR_TURN` | `discard` 时非本家回合且无针对本家的打牌窗口 | 保持 |
| `NO_PENDING_ACTION` | `action` 时本家不在当前窗口内 | 保持 |
| `ILLEGAL_ACTION` | 动作不在 `options` 中 / `tiles` 不满足条件 / 打牌窗口回 `pass` / 伪造 `win` | 保持 |
| `UNKNOWN_TILE` | 牌编码非法，或该牌不在本家手牌 | 保持 |
| `BAD_REQUEST` | 非法 JSON、未知/缺失 `type`、字段类型或 `seq` 缺失、重复 `login`/`create_room`、非法 UTF-8 | 保持（第 10 次坏帧断开） |
| `FRAME_TOO_LARGE` | 单帧 > 64 KB | **断开** |
| `RATE_LIMITED` | 入站帧速率超限 | 保持（持续风暴则断开） |
| `INTERNAL` | 服务端内部错误；账号服务不可用时账号类帧亦回此码（不计配额） | 保持（客户端可提示并返回登录） |
| `AUTH_FAILED` | 账号不存在或口令错误（v1.3） | 保持（**计入非法操作计数**，第 10 次断开） |
| `USERNAME_TAKEN` | `register` 的账号名已存在（v1.3） | 保持（不计配额） |
| `ALREADY_ONLINE` | 该账号已在另一连接登录（v1.3） | 保持（不计配额） |

- **`STALE_SEQ` 不作为错误码下发**：`seq` 不匹配按 §6.3 静默丢弃（H7）。

### 7.2 计数与断开

> v1.4 归属说明：以下计数与阈值**逐字不变**，仅**计数归属**从 `Connection` 移到该连接的 `ConnectionActor`（同一条连接 = 同一个 actor = 同一把计数器）。房间级错误经 `ConnectionActor` 的座位投递消息记账，语义与断开时机不变。

- 坏帧计数（`BAD_REQUEST` 类）：累计 10 → 断开。
- 非法操作计数（`NOT_YOUR_TURN`/`NO_PENDING_ACTION`/`ILLEGAL_ACTION`/`UNKNOWN_TILE`，以及 v1.3 的 `AUTH_FAILED`）：累计 10 → 断开。
- 昵称非法计数（`INVALID_NAME`）：累计 3 → 第 4 次断开。
- `USERNAME_TAKEN` / `ALREADY_ONLINE` **不**计入任何配额（客户端可换名/换身份重试）；账号服务不可用的 `INTERNAL` 亦不计（服务端故障，非客户端错误）。
- 断开一个连接 **MUST NOT** 影响其他连接、其他座位或房间状态（§1.4/§8.5）。
- 客户端的 `error` 不改变本地牌桌状态（UI_UX_SPEC §8.5 统一规则）。

### 7.3 状态零变更保证（H8 前置）

任何被拒绝的入站帧：**不得**改变房间状态、**不得**广播任何帧、**不得**崩溃；仅向该连接回一帧 `error`（`seq` 过期情形除外，见 §6.3）。

---

## 8. 私有信息与服务端权威

### 8.1 私有信息矩阵

| 信息 | 本家 | 他家 | 承载帧 |
| --- | --- | --- | --- |
| 自家手牌牌面 | ✅ | ❌ 绝不 | `game_start.hand`、`state_sync.hand`、`draw.tile`（仅摸牌者） |
| 他家的手牌牌面 | ❌ 绝不 | ❌（同一规则） | **不存在任何字段**（§8.2） |
| 他家手牌张数 | ✅ | ✅ | `state_sync.seats[].hand_count` |
| 牌河（牌面与顺序） | ✅ | ✅ | `discard_event`、`state_sync.seats[].discards[]` |
| 副露（分组与牌面） | ✅ | ✅（**暗杠除外**，见下一行） | `action_event`、`state_sync.melds` / `seats[].melds[]` |
| **暗杠的 4 张牌面（v1.6）** | ✅ 本家始终可见 | ❌ **绝不**（`tiles` 恒为 `[]`），**局终揭示**（最后一份 `state_sync` 与 `game_end.results[].hand`） | `state_sync.melds[]`（本家）/ `seats[].melds[]`（他家，空数组） |
| 牌山剩余 | ✅ | ✅ | `state_sync.remaining` |
| 当前行动座位 | ✅ | ✅ | `state_sync.current_seat` |
| `options` | ✅ 仅窗内座位 | ❌ | `action_request`（只发给窗内座位） |
| 摸到的牌 | ✅ 仅摸牌者 | ❌ | `draw`（只发给摸牌者） |
| 和牌者手牌 | ✅ 结算公开 | ✅ 结算公开 | `game_end.results[win.seat].hand` |

### 8.2 H6：`seats[].hand` 必须不存在

1. `state_sync.seats[i]` **MUST NOT** 含 `hand` 字段；`game_start` 与 `state_sync` 的 `hand` 只为**收件人本人**生成。
2. 客户端即使收到（服务端缺陷）也 **MUST NOT** 渲染或写入本地状态，**SHOULD** 记一条 WARN（UI_UX_SPEC §7.3）。
3. 协议中**不存在**查询他家手牌或牌山内容的请求类型；`sync` 只返回调用者视角。

### 8.3 H8：服务端永不采信客户端结论

1. 收到 `action{action:"win"}` **MUST** 用 `RULESET.md` §3 的和牌判定**重算**；不成立 → `ILLEGAL_ACTION`，状态零变更。
2. 碰/吃/杠同样 **MUST** 按 `RULESET.md` §4 重算合法性（含 `{a,b,T}` 顺子成立、非下家不得吃、`remaining≥1` 才可杠）。**v1.6 补充**：`ankan`（本家 4 张同面）与 `kakan`（已有碰组 + 手牌 1 张同面）同样必须重算，且必须校验 `remaining ≥ 1`（无牌可补时不得开杠，`RULESET.md` §1.5/§4.3）。
3. `delta_score`、`win_type`、`tags`、`M` **MUST** 由服务端规则库计算；客户端上报的任何分数一律忽略。**v1.6 补充**：`yaku[]`、`han`、`han_breakdown{base,tsumo,dealer,total}`、`dealer_bonus`、`rinshan`、`match_total`、`match_end.rankings[]` 全部由服务端计算，客户端上报一律忽略（派生字段 `han`/`dealer_bonus`/`rinshan` MUST 与 `yaku[]`/`han_breakdown` 恒等，不得各算一次）。
4. **v1.6：杠补牌不得由客户端触发**。补摸是 `ankan`/`kakan`/`kong` 被服务端接受后的**副作用**，客户端不能单独请求摸牌。

### 8.4 H9：权威状态与即时事件

- `state_sync` 是**权威**：客户端 **MUST** 以其整体覆盖本地牌桌状态。
- `action_event`/`discard_event`/`draw` 是**即时增量**，用于低延迟反馈；三者与 `state_sync` 的**到达顺序不作保证**。
- 服务端在副露/和牌后必然补发 `state_sync`（§5.12），客户端据此收敛。

### 8.5 断线（托管）

| 场景 | 期望行为 |
| --- | --- |
| 大厅中真人断开 | 释放座位；房主断开即销毁房间 |
| 对局中真人断开（socket 关闭或 20 s 无入站帧） | 该座位 `disconnected = true` 并**立即托管**（不等 15 s），牌局继续；`state_sync.seats[].disconnected` 告知其他座位 |
| 托管座位参与结算 | 照常计算并出现在 `game_end.results`；昵称由客户端加「（断线托管）」后缀 |
| **局间真人断开（v1.6）** | 与"对局中"同口径：座位保持 `disconnected = true` 并**跨局延续**（托管标记不因局终而清除），下一局照常为该座位代打；房间不因局间断线而销毁 |
| **场次中任何时刻重连** | **不在本里程碑范围**；重连是新的 `player_id`，无法回到原房间（原座位由托管跑完本场） |
| 断线重连恢复 | **不在本里程碑范围**；重连只能重新 `login` + `create_room` 开新场 |

---

## 9. 帧序列示例

### 9.1 开局握手（t6 脚本必须按此顺序）

```
C→S {"type":"login","name":"玩家_"}
S→C {"type":"login_ok","player_id":"p-3f2a1c","name":"玩家_"}
C→S {"type":"create_room"}
S→C {"type":"room_joined","room_id":"r-0001","seat":0,"seats":[…]}
C→S {"type":"ready"}
S→C {"type":"game_start","seat":0,"dealer":0,"hand":[14 张]}          # 庄家 = 真人
```

### 9.1b 账号登录与注册（v1.3，`ACCOUNT_DESIGN.md` §3.6）

```
C→S {"type":"login","username":"alice","password":"s3cret"}
S→C {"type":"login_ok","player_id":"u-000042","name":"爱丽丝"}
C→S {"type":"create_room"}                     # 之后与 9.1 完全相同
```

```
C→S {"type":"register","username":"alice","password":"s3cret"}   # 注册成功即登录
S→C {"type":"login_ok","player_id":"u-000042","name":"alice"}
```

```
C→S {"type":"login","username":"alice","password":"wrong"}
S→C {"type":"error","code":"AUTH_FAILED","message":"wrong username or password"}   # 连接保持
```

### 9.1c 战绩查询（v1.3）

```
C→S {"type":"stats_request"}
S→C {"type":"stats","games":12,"wins":5,"draws":3,"net_score":-14,"recent":[…]}
```

### 9.2 一次「打牌 → 他家碰 → 鸣牌者打牌」

```
S→C {"type":"action_request","seq":1,"deadline_ms":…,"window":"discard","options":[]}   # 仅 seat 0
C→S {"type":"discard","tile":"9s","seq":1}
S→C discard_event{seat:0,tile:"9s",seq:1}                       # 广播 4 家
S→C action_request{seq:2,window:"claim",options:[pong/pass]}    # 仅 seat 1（有非 pass 选项）
C→S {"type":"action","action":"pong","tiles":["9s"],"seq":2}
S→C action_event{seat:1,action:"pong",tiles:["9s"],from_seat:0,seq:2}   # 广播
S→C state_sync{seq:3,…}                                          # 并行广播 4 家（副露后）
S→C action_request{seq:3,window:"discard",options:[]}            # seat 1 待打牌（与上一帧 state_sync 同 seq：同窗口过渡共用同一 seq，§6.2 第 1 条）
```

### 9.3 一次「自摸和牌」

```
S→C draw{tile:"5m",seat:1,seq:9}                    # 仅 seat 1
S→C action_request{seq:9,window:"discard",options:[{"action":"win","tiles":[]}]}   # 仅 seat 1
C→S {"type":"action","action":"win","tiles":[],"seq":9}
S→C action_event{seat:1,action:"win",tiles:["5m"],seq:9}         # 无 from_seat（自摸）
S→C state_sync{seq:10,…}
S→C game_end{reason:"win_tsumo",results:[…],win:{…,"from_seat":null,"tsumo":true,…}}
```

### 9.4 一次「全部 pass 的鸣牌窗口」

```
S→C discard_event{seat:0,tile:"3p",seq:5}                       # 广播
S→C action_request{seq:6,window:"claim",options:[…pass…]}       # 仅 seat 1（唯一有选项者）
C→S {"type":"action","action":"pass","tiles":[],"seq":6}
（不广播任何 action_event）
S→C draw{tile:"7m",seat:1,seq:7} + state_sync{seq:7,…}          # 下一家摸牌
```

### 9.5 一次「暗杠 → 补牌 → 打牌」（v1.6）

```
S→C draw{tile:"5m",seat:0,seq:12}                                # 仅 seat 0（摸到第 4 张 5m）
S→C action_request{seq:12,window:"discard",
     options:[{"action":"ankan","tiles":["5m"]},                  # 4 张同面 + remaining≥1
              {"action":"kakan","tiles":["7p"]}]}                 # 另有一组 7p 碰 + 手牌 1 张 7p
C→S {"type":"action","action":"ankan","tiles":["5m"],"seq":12}
S→C action_event{seat:0,"action":"ankan","tiles":["5m"],seq:12}   # 广播；**无 from_seat**
S→C draw{tile:"9s",seat:0,seq:13}                                # 补牌，仅 seat 0
S→C state_sync{seq:13, "melds":[{"type":"ankan","tiles":["5m","5m","5m","5m"]}, …],
     "seats":[{…"melds":[]…}, …]}                                 # 本家带 4 张；他家该组 tiles 为 []
S→C action_request{seq:13,window:"discard",options:[]}            # 打牌窗口（补牌后）
C→S {"type":"discard","tile":"9s","seq":13}
```
> 关键点：暗杠**不移动任何牌河**（无 `from_seat`）；`seq` 由 12 推进到 13 恰好一次（补牌后开的新打牌窗口）。

### 9.6 一场四局 + 连庄 + 场终（v1.6）

```
… 第 1 局（round_index:0, dealer:0）…
S→C game_end{round_index:0,dealer:0,renchan_count:0,match_will_continue:true,
             results:[…,match_total:…], win:{…,han:N,yaku:[…]}}     # 局终 ×1
S→C game_start{seat:0,dealer:1,round_index:1,round_count:4,hand:[…]} # 下一局（dealer 轮转）
… 第 2 局：庄家 seat 1 荒庄 ⇒ 连庄 ⇒ 仍是 round_index:1，dealer 仍为 1 …
S→C game_end{round_index:1,dealer:1,renchan_count:1,match_will_continue:true, …}
S→C game_start{seat:0,dealer:1,round_index:1,round_count:4,hand:[…]} # 连庄：round_index 不变
… 第 3、4 局 …
S→C game_end{round_index:3,dealer:3,renchan_count:1,match_will_continue:false, …}
S→C match_end{hands_played:4,renchan_count:1,
              rankings:[{seat,…,"final_score":…,"rank":…}, …]}       # 场终恰好一次
（30 s 后服务端回收房间与连接，§1.4 第 5 条）
```
> 注意三件事：①`game_end` 与 `match_end` 是**两个**语义层，前者 `hands_played + renchan_count` 次、后者恰好 1 次（上例共 5 手 = 4 局 + 1 连庄）；②连庄时 `round_index` **不递增**（连庄不占局数），但 `renchan_count` 递增；③`match_total` / `final_score` 是**累计**值，`delta_score` 永远是**单局**值。

---

## 10. 一致性清单（实现方自检）

### 10.1 t3（TCP JSON 网关）

- [ ] NDJSON 分帧、UTF-8 校验、64 KB 上限、坏帧计数、限速与断开阈值（§1）
- [ ] `login`/`create_room`/`join_room`/`ready` 与 `error` 路由（§4.1–§4.4、§7）
- [ ] 未知 `type`、非法 JSON 不崩溃且不影响其他连接（§1.3、§7.3）
- [ ] 私有信息按连接隔离（§8.1）

### 10.2 t4（RoomActor）

- [ ] `seq` 单调与「窗口唯一」（§6.1、§6.2）；**v1.6：`seq` 跨局不重置**
- [ ] 动作窗口超时严格走 `seq` 作废（**无取消机制**）；超时代打口径（§0.5、§6.4）；**超时/托管永不选杠**
- [ ] H1 打牌窗口不接受 `pass`；H3 `action_event` 语义；H8 一律重算（§4.6、§5.8、§8.3）
- [ ] 广播并行发出（§5.12）
- [ ] `game_end` 零和与字段完整（§5.10）；**`Σ delta_score == 0` 且 `Σ match_total == 0`**
- [ ] **v1.6 场次级**：`game_end` 每局一次、`match_end` 每场恰好一次；`round_index` 连庄不递增；`max_rounds = 4` 与 `max_renchan = 4` 双闸生效；**局终不回收房间、场终才回收**（§1.4 第 5 条）
- [ ] **v1.6 暗杠**：他家 `seats[].melds[]` 的 `ankan.tiles` 恒为 `[]`；局终最后一份 `state_sync` 补全；`ankan`/`kakan` 的 `action_event` 不带 `from_seat`；杠补牌后 `remaining` 仍由「已摸牌数」派生
- [ ] **v1.6 局间**：无未决窗口、不下发 `state_sync`、不接受 `join_room`、迟到超时消息静默丢弃

### 10.3 t5（pygame 客户端）

- [ ] 所有出站 `discard`/`action` 回带 `seq`（§6.2）
- [ ] 只在 `options` 内取值；打牌窗口不提供「过」按钮（H1）；**v1.6 新增 `ankan`/`kakan` 两个按钮位**
- [ ] 按 `from_seat` 从**打出者**牌河移除被鸣牌（§5.8）；**`ankan`/`kakan` 不动任何牌河**
- [ ] 他家不渲染任何牌面；`seats[].hand` 即使出现也不消费（§8.2）；**他家 `ankan.tiles == []` 时按"扣着的杠"渲染**
- [ ] `state_sync` 整体覆盖，`action_event` 仅即时反馈（§8.4）
- [ ] **v1.6 场次级**：`game_start` 是"新一局"的唯一重置信号；`game_end`（局终）不得当作"对局结束"退出牌桌；`match_end`（场终）才进最终结算页；局间不显示倒计时（协议不承诺秒数）

### 10.4 t6（端到端脚本）

- [ ] 握手顺序 `login → create_room → ready`（§9.1）
- [ ] 每帧回带 `seq`；`window=="discard"` 时优先 `win`，否则 `discard{刚摸到的牌}`
- [ ] 断言：`login_ok.player_id` 非空、`game_start.hand` 长度 13/14、至少一次 `draw` 与 `discard_event`、`game_end.results` 长度 4、`Σ delta_score == 0`、全程零 `error`
- [ ] **v1.6 场次级断言**：`game_end` 次数 == `match_end.hands_played + match_end.renchan_count`；`match_end` 恰好 1 次且 `Σ final_score == 0`；`rankings` 覆盖 4 个座位且 `rank` 并列跳号；`round_index` 单调不减且连庄处相等；`seq` 跨局严格递增（§6.2 第 5 条）
- [ ] **v1.6 暗杠断言**：他家视角 `ankan.tiles` 恒 `[]`，局终揭示后被补全；杠后 `remaining` 递减且牌数守恒（`remaining + 各家手牌 + 副露 + 牌河 == 136`）
- [ ] **v1.6 番数断言**（对齐 `GAME_DESIGN` A13–A15）：①`yaku[]` 严格按冻结判定序、**每 id 至多一条**、`tags ⊆ yaku[].id`；②`Σ yaku[].han == han_breakdown.base`、`han_breakdown.total == min(base+tsumo+dealer, 6)`、`win.han == han_breakdown.total`、`dealer_bonus == (dealer==1)`、`rinshan == (yaku 含 rinshan)`；③同一手牌判番 1000 次、且打乱输入顺序后逐字段相同（确定性回归）

---

## 附录 A：消息 × 字段速查

| `type` | 方向 | 关键字段 | `seq` |
| --- | --- | --- | --- |
| `login` | C→S | `name`（游客）**或** `username`,`password`（账号，v1.3） | — |
| `register` | C→S | `username`, `password`, `name?` | — |
| `stats_request` | C→S | — | — |
| `create_room` / `join_room` / `ready` | C→S | `room_id`（仅 join） | — |
| `discard` | C→S | `tile`, `seq` | ✅ 必带 |
| `action` | C→S | `action`（`pass/pong/chow/kong/ankan/kakan/win`）, `tiles[]`, `seq` | ✅ 必带 |
| `sync` / `ping` | C→S | — | — |
| `login_ok` | S→C | `player_id`, `name` | — |
| `error` | S→C | `code`（**14 条之一**）, `message`, `detail?` | — |
| `room_joined` | S→C | `room_id`, `seat`, `seats[]` | — |
| `game_start` | S→C | `hand`, `seat`, `dealer`, `round_index`, `round_count`（后两个 v1.6） | — |
| `draw` | S→C | `tile`, `seat` | ✅ |
| `discard_event` | S→C | `seat`, `tile` | ✅ |
| `action_request` | S→C | `deadline_ms`, `window`（仅 `discard`/`claim`）, `options[]` | ✅ |
| `action_event` | S→C | `seat`, `action`, `tiles[]`, `from_seat?`（`ankan`/`kakan`/自摸 不带） | ✅ |
| `state_sync` | S→C | `hand`, `melds[]`（5 种 type）, `seats[]`（他家 `ankan.tiles` 为 `[]`）, `remaining`, `current_seat` | ✅ |
| `game_end` | S→C | **局终**：`reason`, `results[4]`（+`match_total`）, `win`（+`yaku[]`/`han_breakdown{base,tsumo,dealer,total}`/`han`/`dealer_bonus`/`rinshan`；`tags` 保持旧语义）, `round_index`, `dealer`, `renchan_count`, `match_will_continue`（后四个 v1.6） | ❌ |
| `match_end` | S→C | **场终（v1.6）**：`hands_played`, `renchan_count`, `rankings[]{seat,final_score,rank}` | ❌ |
| `stats` | S→C | `games`, `wins`, `draws`, `net_score`, `recent[]` | ❌ |
| `pong` | S→C | — | — |

> **v1.6 帧数小结**：C→S **10 条**（帧类型不变，`action` 动作词表 5→7）；S→C **13 → 14**（新增 `match_end`）；错误码 **14 条不变**。

## 附录 B：采纳 UI_UX_SPEC §10.2 数据项清单的对照

| UI 清单项 | 本协议落点 | 结论 |
| --- | --- | --- |
| A1–A7（`login`/`create_room`/`ready`/`discard`/`action`/`sync`/`ping`） | §4.1–§4.6、§4.7、§4.8 | ✅ 全部采纳（`discard`/`action` 增加 `seq`，见 H2） |
| B1–B11（`login_ok`/`room_joined`/`game_start`/`draw`/`discard_event`/`action_request`/`action_event`/`state_sync`/`game_end`/`error`/`pong`） | §5.1–§5.11 | ✅ 全部采纳；`draw`/`discard_event` 增加 `seq`（H5），`action_event` 增加 `from_seat`（H3），`melds[]` 形状冻结（H4） |
| C1（`action` 必带 `seq`） | §4.6、§6.2 | ✅ |
| C2（`melds[]` 形状） | §5.9 | ✅ 冻结为 `{type,tiles[]}`（`kong` 4 张）；**v1.6** 扩到 5 种 type，`ankan` 对他家 `tiles` 为 `[]` |
| C3（打牌窗口不接受 `pass`） | §4.6、§6.3 | ✅ 冻结为 `ILLEGAL_ACTION` |
| D（不需要的字段：他家 `hand`、`game_end` 重复昵称、窗口总时长字段、每座位独立倒计时、宝牌/立直/符数字段） | §5.9、§5.10、§5.7、§0.4 | ✅ 一律不提供；实现方不得添加。**v1.6 变更**：`yaku[]` 与 `han` **进入**范围（番型表），立直/宝牌/振听/符数/役满仍不提供 |
| **A 批（v1.6）增补项** | §4.6、§5.4、§5.9、§5.10、§5.15、§6.2 | ✅ 全部为**增补**：不改既有必填字段、不改既有错误码、不新增窗口类型 |

## 附录 C：修订记录

| 版本 | 内容 | 来源 |
| --- | --- | --- |
| **v1.6** | **A 批增补**（番型表 + 番数结算 + 一场四局/连庄/场次总分 + 暗杠/加杠），全部为**增补式**：① §4.6 `action` 动作词表 5 → **7**（+`ankan`、+`kakan`），合法性矩阵由 5 列扩 7 列，`tiles` 语义表增 2 行；**不新增窗口类型**（§6.1 明文，两者是 `discard` 窗口的附加 `options`）；② §5.9 `melds[].type` 3 → **5**（+`ankan`、+`kakan`），新增**暗杠私有权重**与**局终揭示时序契约**（他家 `tiles` 恒 `[]`，局终最后一份 `state_sync` 与 `game_end.results[].hand` 补全）；③ §5.4 `game_start` 增 `round_index`/`round_count`，`dealer` **不再恒为 0**；④ §5.10 `game_end` **只增字段**（`round_index`/`dealer`/`renchan_count`/`match_will_continue`/`results[].match_total`/`win.yaku[]`/`win.han_breakdown{base,tsumo,dealer,total}`/`win.han`/`win.dealer_bonus`/`win.rinshan`），`reason` 词表与既有字段形状一律不变（**`win.tags` 保持旧语义**、`results[].win_type` 保持旧形状），并澄清 `game_end` **仍只表示局终**；冻结 `win.yaku[].id` 的 14 个词汇与判定顺序（番值以 `RULESET` 为唯一权威）；⑤ 新增 §5.15 **`match_end`**（S→C 14 条），冻结 `rankings[]` 的排序与**并列跳号**口径；⑥ §6.2 第 5 条明确 **`seq` 跨局单调不重置**；⑦ §6.3 判定表增 3 行（打牌窗口回 `pass`、鸣牌窗口回 `ankan`/`kakan`、打牌窗口回鸣牌动作均 `ILLEGAL_ACTION`）；⑧ §6.4 冻结**超时/托管永不选杠**，并明确局间无窗口；⑨ §1.4 第 5 条回收时机改为**场终**（局终不回收），新增第 6 条局间契约；⑩ §0.2 **H1 重述**（按窗口 `options` 判定）、§0.4 局制改为「一场 4 局 + 连庄（max_rounds=4 / max_renchan=4）」并把抢杠列为**永久不做**、§0.6 把 136/83/84/21 标注为**每局**口径并写明杠补牌不改变上界；⑪ §8.1 私有信息矩阵增「暗杠」一行，§8.3 补暗杠/加杠重算与补牌不得由客户端触发；⑫ §9 增 §9.5（暗杠链路）与 §9.6（一场四局+连庄+场终）示例，§10 三份清单补场次级与暗杠自检项，附录 A 补 `match_end` 与字段速查。**零新增错误码（仍 14 条）**；既有 14 条错误码语义与阈值、15 s 窗口、`seq` 作废方案、136/83/84/21 数值、零和口径**全部未动** | captain 裁决（A 批需求评审，Q1–Q6）；架构评估与改造点清单见 `A1_ARCH_ASSESSMENT.md`；实现落点提示见其 §3（C1–C15） |
| v1.0 | 首次冻结：传输/分帧/帧格式、8+12 条消息逐字段规范、`seq` 作废与窗口判定表、超时代打、11 条错误码与配额、私有信息矩阵、服务端权威（H8）、帧序列示例、实现方自检清单；相对任务草案的增补：`discard`/`action`/`draw`/`discard_event`/`action_event` 的 `seq`、`action_event.from_seat`、`action_request.window` 与 `options` 形状、`melds[]` 形状、`game_end.win` 明细、11 条错误码 | t1（依 GAME_DESIGN v1.1、UI_UX_SPEC v1.1、t10/t12 评审与 H1–H10） |
| v1.1 | 仅同步上游版本指针与批准状态（当时口径为 108 张 / 牌山 55），**未改动本文任何字段** | captain（t13） |
| v1.5 | 大厅选房：新增 C11 `list_rooms`、S14 `room_list`；`join_room` 在开局前替换 AI 座位；`create_room` 不自动开局；`ready` 仍立即洗牌。不新增错误码 | 用户需求（大厅要能新建并进入其他房间） |
| v1.4 | 依用户需求（本轮 actor 化重构）在 §7.2 补一句归属说明：三把计数器的阈值与断开语义**逐字不变**，仅**计数归属**由 `Connection` 移到该连接的 `ConnectionActor`（房间级错误改经 `ConnectionActor` 的座位投递消息记账）。帧格式/字段/时序/错误码/配额数值**全部未动** | 用户需求（本轮 actor 化重构）；实现在 `game/server/player_actor.*` 与 `seat_endpoint.h` |
| v1.3 | 依 captain 裁决（账号功能需求）**增补账号与战绩**：① §3.1/§3.2 帧数 8→**10**（+`register`、+`stats_request`）、12→**13**（+`stats`）；② §4.1 `login` 增加**账号形态**（可选 `username`/`password`，缺省仍是游客形态，游客行为逐字不变），新增 §4.9 `register`（注册成功即登录，复用 `login_ok`）、§4.10 `stats_request` 与 §5.13 `stats`；③ §7.1 错误码 11→**14**（+`AUTH_FAILED`、+`USERNAME_TAKEN`、+`ALREADY_ONLINE`），§7.2 明确 `AUTH_FAILED` 计入非法操作计数；④ §1.4 增加「一个账号同时只有一个在线连接」（`ALREADY_ONLINE`）；⑤ §5.1 `player_id` 区分为游客进程内 id 与账号全局持久 id（`u-<id>`）；⑥ §0.4 把持久化例外交给 `ACCOUNT_DESIGN.md`；⑦ §9 增补账号/注册/战绩帧序列，附录 A 补字段速查。**未改动**：15 s 窗口与 1000..60000、原有 11 条错误码的语义与阈值、`seq` 作废方案、私有信息规则、136/83/84/21、底分 1 与 ×4/×4/×2、`M ∈ {1,4,16}`、零和口径 | captain 裁决（用户需求「做账号功能，存储用 MySQL」）；规格源头见 `game/docs/ACCOUNT_DESIGN.md` v1.0 |
| v1.2 | 依 captain 裁决（t14）**恢复 7 种字牌，牌张口径 108 → 136**：① §2.1 编码表新增 `1z`..`7z` 及逐字映射（1z=东 / 2z=南 / 3z=西 / 4z=北 / 5z=白 / 6z=发 / 7z=中），牌面种类 27 → **34**、总张数 108 → **136**，并明确「字牌只能构成刻子/杠，不可吃」；② §2.2 排序改为**先数牌后字牌**（`m<p<s<z`）；③ §0.6 由「待裁决冲突」改写为**最终口径**（136 / 牌山 83 / 84 次打牌 / 单座位 21）并保留历史沿革；④ §5.9 示例 `remaining` 55 → **83**。**未改动**：15 s 窗口与 1000..60000、11 条错误码、`seq` 作废方案、私有信息规则、状态机时序语义、`M ∈ {1,4,16}` | captain 裁决（t14 执行：恢复字牌以解决牌局过短与荒庄率过高） |
