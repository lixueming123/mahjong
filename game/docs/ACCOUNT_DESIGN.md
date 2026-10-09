# 账号与战绩设计（ACCOUNT_DESIGN）

- 文档 ID：ACCOUNT_DESIGN
- 版本：**v1.2**
- 修订：v1.2 依用户需求（本轮 actor 化重构）更正 §6.1 第 1 条的**论据**：DB 不放进 actor 的 handler，理由只有「会阻塞」——原文另举的「handler 一旦挂起就不再持有 mailbox」自 `GAME_DESIGN.md` §0.3 v1.8 起已不成立（每 actor 的 handler 由 `AsyncGate` 保证互斥，挂起不再破坏原子性）。**实现形状与 §1–§5 口径一律未动**
  v1.1 依 captain 裁决（用户需求「客户端登陆后不要立马进对局，进入大厅界面」）把战绩的主界面从结算页扩到**大厅界面**（`UI_UX_SPEC.md` §12，v1.8）：登录成功后先进大厅，那里常驻显示身份与战绩；结算页战绩块与牌桌弹窗保持不变。协议与本文件 §1–§4 的口径**一律未动**
- 状态：**本次需求评审产物**——本文是账号功能的规格源头；`PROTOCOL.md` / `GAME_DESIGN.md` / `UI_UX_SPEC.md` 依本文做**增补式**修订（各加一行修订记录），既有冻结数值与流程一律不动
- 依据：captain 裁决（用户需求「做账号功能，存储用 MySQL」）。按三份冻结文档的既有规则（「不得自行发明字段、消息类型或错误码；确有必要时先提需求评审」），先有本次评审再有实现
- 关联文档：`GAME_DESIGN.md`（玩法源头）、`PROTOCOL.md`（线协议）、`UI_UX_SPEC.md`（客户端表现层）、`RULESET.md`（与本文无关）

## 0. 与既有规格的关系

| 项 | 处置 |
| --- | --- |
| 游客登录 `login{name}` | **完全不变**：行为、校验、`login_ok{player_id,name}` 字段形状逐字保持。客户端不传账号凭据时走的仍是这条路径 |
| `PROTOCOL.md` §3.1 客户端帧 8 条 | 8 → **10**（+`register`、+`stats_request`） |
| `PROTOCOL.md` §3.2 服务端帧 12 条 | 12 → **13**（+`stats`） |
| `PROTOCOL.md` §7.1 错误码 11 条 | 11 → **14**（+`AUTH_FAILED`、+`USERNAME_TAKEN`、+`ALREADY_ONLINE`） |
| §0.4「不做状态持久化恢复」 | 收窄为「不做**对局状态**的持久化恢复」；账号与战绩的持久化自本版起**在范围内** |
| 其它已冻结数值（15 s 窗口、底分 1、×4/×4/×2、M∈{1,4,16}、136/83/84/21、零和口径、`seq` 作废、私有信息规则） | **全部未动** |

## 1. 账号模型

1. **两种会话**：**游客**（`login{name}`，昵称即身份，进程内 `player_id`）与**账号**（`login{username,password}` 或 `register{...}`，持久身份）。同一连接二选一，登录后不可切换。
2. **身份映射**：账号的 `player_id = "u-" + <accounts.id 十进制>`（如 `u-000042`）；游客仍是既有 `PlayerIdAllocator` 的 `p-<6 位十六进制>`；机器人是 `bot-<seat>`。前缀 `u-` / `p-` / `bot-` 互不重叠，`player_id` 对客户端仍是不透明串。
3. **一个账号同时只有一个在线连接**：第二个连接用同一账号登录 → `error{code:"ALREADY_ONLINE"}`，连接保持（该连接仍可改用游客身份登录）。首个连接关闭时释放。
   - 释放时机是**连接关闭**（FIN/RST、心跳超时断开、配额断开），而不是「进入房间」或「对局结束」。因此登录后一直挂在登录界面、又持续 `ping` 保活的连接，会一直占用该账号（这是刻意的：简单、可预期，且客户端把 `ALREADY_ONLINE` 单独提示为「该账号已在别处登录」而不是泛化错误）。
4. **账号不改变对局语义**：房间、座位、窗口、超时、托管、结算一律与游客相同；账号只影响身份来源、战绩落库与战绩查询。
5. 账号的昵称（`nickname`）在注册时确定，默认取账号名；对局内展示用的是昵称，规则仍受 `GAME_DESIGN` §3.5 约束（1..16 字符、无控制字符，**允许重名**）。

## 2. 存储

### 2.1 连接与库表

- 引擎：MySQL 8.0（本机 `/usr/local/mysql`）。连接参数由启动参数给出（§6.1）；口令不写进源码、不进日志。
- 库名默认 `mahjong`（测试用 `mahjong_test`）；服务端启动时 `CREATE DATABASE IF NOT EXISTS` + `CREATE TABLE IF NOT EXISTS` 自建，无需手工建表。
- 字符集 `utf8mb4`，引擎 InnoDB。

```sql
CREATE TABLE IF NOT EXISTS accounts (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username      VARCHAR(32)     NOT NULL,
  password_salt CHAR(32)        NOT NULL,   -- 32 位十六进制（16 字节随机盐）
  password_hash CHAR(64)        NOT NULL,   -- SHA-256 十六进制
  nickname      VARCHAR(64)     NOT NULL,
  created_at    TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_login_at TIMESTAMP       NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_accounts_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS matches (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  account_id  BIGINT UNSIGNED NOT NULL,
  room_id     VARCHAR(16)     NOT NULL,
  reason      VARCHAR(16)     NOT NULL,   -- win_ron / win_tsumo / draw_exhausted
  delta_score INT             NOT NULL,   -- 本局该座位的得失分（零和口径不变）
  win         TINYINT         NOT NULL,   -- 1 = 该座位本局和牌
  ended_at    TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_matches_account (account_id, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### 2.2 口令口径（**演示级**，此处明写取舍）

- 存储：每账号 16 字节随机盐（`std::random_device`）+ 口令的**单轮 SHA-256**，散列由 MySQL 侧计算：
  `password_hash = SHA2(CONCAT(password_salt, <明文口令>), 256)`。
- 校验：`SELECT id, nickname, (password_hash = SHA2(CONCAT(password_salt, <明文口令>), 256)) AS ok FROM accounts WHERE username = <username>`。
- 取舍：**无 KDF 拉伸**（不用 bcrypt/scrypt/PBKDF2），且明文口令会出现在发往本机 MySQL 的查询里。理由是本里程碑是单机演示、DB 与进程同机同用户，而换取的是**零新增依赖**（不需要 libcrypto/OpenSSL）。要生产级只需把上述两个 SQL 表达式替换为 libcrypto 的 PBKDF2-HMAC-SHA256，其它逻辑不变。
- 注入防护：`username` / `password` / `nickname` 在 C++ 侧先做字符集与长度校验（§3.1），拼 SQL 时再经 `mysql_real_escape_string` 转义；两条一起保证注入在结构上不可能。
- 明文口令**不入日志、不入错误消息、不回显**。

### 2.3 降级语义（**MySQL 不可用时**）

| 情形 | 服务端行为 |
| --- | --- |
| 启动时连不上 / 建表失败 | 记一条 error 日志说明原因，账号功能标记为不可用；**服务端照常启动**（游客登录与整局游戏完全不受影响） |
| 运行中查询失败 | 该次操作按失败处理，并尝试在下一个任务前重连；连续失败不致命 |
| 账号相关帧（`register` / 带凭据的 `login` / `stats_request`） | `error{code:"INTERNAL", message:"account service unavailable"}`，连接保持；**不计入任何配额**（这是服务端故障，不是客户端错误） |
| 战绩落库 | fire-and-forget；失败只记日志，**绝不影响 `game_end` 的广播**（客户端必须照常收到结算） |

## 3. 协议增补（对 `PROTOCOL.md` 的增补，编号沿用其章节）

### 3.1 `login`（扩展 §4.1）

```json
{"type":"login","name":"玩家_"}
{"type":"login","username":"alice","password":"s3cret"}
```

| 字段 | 类型 | 必填 | 校验 |
| --- | --- | --- | --- |
| `name` | string | 游客路径必填 | 1..16 个 UTF-8 字符；不含 `\n`/`\r`/控制字符（`0x00`–`0x1F`）——**与既有 §4.1 逐字相同** |
| `username` | string | 账号路径必填 | 3..16 个字符；仅 `[A-Za-z0-9_]`（ASCII） |
| `password` | string | 账号路径必填 | 6..64 个字符；仅可见 ASCII（`0x20`–`0x7E`），不含控制字符 |

- **判定规则**：帧中出现 `username` 即走账号路径（此时 `password` 必填，`name` 忽略）；否则走既有游客路径，行为与 v1.2 逐字相同。
- 账号路径成功 → `login_ok{player_id:"u-<id>", name:<nickname>}`（**字段形状与游客路径完全相同**，仅取值不同）。
- 账号路径失败 → `error{code:"AUTH_FAILED", message:"wrong username or password"}`（账号不存在与口令错误**返回同一错误**，不泄露账号是否存在）；该错误**计入 §7.2 的 illegal-operation 计数**（累计 10 次断开，与既有配额机制一致），连接保持。
- 同一账号在别处在线 → `error{code:"ALREADY_ONLINE"}`，连接保持。
- 重复 `login`（含账号路径挂起期间再发）→ `error{code:"BAD_REQUEST", message:"already logged in"}`，连接保持。
- 账号路径校验不过（用户名/口令长度或字符集非法）→ `error{code:"BAD_REQUEST"}`，连接保持。
- 账号路径是**异步**的：服务端收到后先发起查询，`login_ok` 出现前该连接上除 `login` 外的帧仍一律 `NOT_LOGGED_IN`（与既有 §4.1 末条一致）。

### 3.2 `register`（新增，**注册成功即登录**）

```json
{"type":"register","username":"alice","password":"s3cret","name":"爱丽丝"}
```

| 字段 | 类型 | 必填 | 校验 |
| --- | --- | --- | --- |
| `username` | string | 是 | 同 §3.1（3..16，`[A-Za-z0-9_]`） |
| `password` | string | 是 | 同 §3.1（6..64，可见 ASCII） |
| `name` | string | 否 | 昵称；缺省取 `username`；给出时按既有昵称规则校验（1..16 字符、无控制字符） |

- 成功 → **直接下发 `login_ok{player_id:"u-<id>", name:<nickname>}`**（不新增 `register_ok` 帧）：注册即登录，客户端后续流程与游客完全一致（`create_room` → `ready`）。
- 账号名已存在 → `error{code:"USERNAME_TAKEN", message:"username already taken"}`，连接保持，可换名重试。
- 字段缺失/类型错/校验不过 → `error{code:"BAD_REQUEST"}`。
- 已登录（含账号路径挂起期间）再发 `register` → `error{code:"BAD_REQUEST", message:"already logged in"}`。

### 3.3 `stats_request`（新增）

```json
{"type":"stats_request"}
```

- 无字段。成功 → 下发 `stats`（§3.4）。
- **游客也可发**：游客没有账号，返回零值载荷（`games/wins/draws/net_score` 全 0、`recent` 为空数组），**不是错误**——客户端因此不必区分会话类型。
- 未登录 → `error{code:"NOT_LOGGED_IN"}`（与其它帧一致）。
- 账号服务不可用 → `error{code:"INTERNAL", message:"account service unavailable"}`。

### 3.4 `stats`（新增）

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
| `recent[]` | array | 最近 ≤10 局，按结束时间**倒序**；元素含 `room_id` / `reason` / `delta_score` / `win` / `ended_at`（Unix 秒） |

### 3.5 错误码与配额增补（对 §7.1 / §7.2 的增补）

| 错误码 | 触发 | 连接 |
| --- | --- | --- |
| `AUTH_FAILED` | 账号不存在或口令错误 | 保持（**计入 illegal-operation 计数**，第 10 次断开） |
| `USERNAME_TAKEN` | `register` 的账号名已存在 | 保持（不计配额） |
| `ALREADY_ONLINE` | 该账号已在另一连接登录 | 保持（不计配额） |

错误码总数 11 → **14**；`PROTOCOL.md` §7.1 的「恰好这 11 条」表述相应改为 14 条。既有 11 条的语义、触发条件、断开阈值一律不变。

### 3.6 帧序列增补（对 §9 的增补）

游客（**与 v1.2 相同，未变**）：

```
C→S login{name} → S→C login_ok → C→S create_room → S→C room_joined → C→S ready → S→C game_start → … → game_end
```

账号登录：

```
C→S login{username,password} → S→C login_ok{player_id:"u-…"} → C→S create_room → …（同上）
```

注册即登录：

```
C→S register{username,password[,name]} → S→C login_ok{player_id:"u-…"} → C→S create_room → …（同上）
```

战绩查询（任意时刻，登录后即可）：

```
C→S stats_request → S→C stats{...}
```

## 4. 战绩口径

1. **写入时机**：`RoomActor` 判定局终（和牌或荒庄）后，随既有「局终通知」一次性发出（该通知已有「每局恰好一次」的实现与测试保证），由 `LobbyActor` 提交落库；**fire-and-forget**，不阻塞、不改变任何对局行为。
2. **写什么**：每局每个**账号座位**一行（`!is_bot` 且 `account_id != 0`）。游客座位、AI 座位不写。
3. **取值**：`reason` 取 `game_end.reason`；`delta_score` 取该座位在 `game_end.results[]` 中的值（零和口径不变）；`win` 为该座位是否和牌方。
4. **荒庄**：照记（`reason = "draw_exhausted"`，四家 `delta_score = 0`，`win = 0`），并计入 `games` 与 `draws`。
5. **中途断线托管后跑完的局**：照记（局终事件与断线无关）。
6. 写入失败只记日志；同一局重复通知不会重复入库（依赖既有的「恰好一次」保证，不做去重键）。

## 5. 客户端表现（对 `UI_UX_SPEC.md` §3 的增补）

1. 登录界面新增**模式切换**（「游客」/「账号」），默认**游客**——保证既有客户端测试与既有交互不变。
2. 账号模式下：输入框 1 为**账号名**，新增**密码框（●掩码）**，主按钮文案「登录」，新增次按钮「注册」（`register`，昵称缺省用账号名）。
3. 本地预校验与提示：账号名 3..16 且仅 `[A-Za-z0-9_]`；口令 6..64 可见 ASCII；密码框**不回显明文、不写入日志**。
4. 错误文案：`AUTH_FAILED` →「账号或密码错误」；`USERNAME_TAKEN` →「账号名已被占用」；`ALREADY_ONLINE` →「该账号已在别处登录」；`INTERNAL` →「账号服务暂不可用」。
5. **战绩在哪里看（v1.1 更新）**——三处共用同一份 `stats` 快照与同一套行版式，游客会话三处都不显示（其 `stats` 恒为零值）：
   - **大厅界面**（`UI_UX_SPEC.md` §12，v1.8）：登录成功后先进大厅，常驻显示身份行 + 汇总 + 最近 ≤10 局（含时间）；进入大厅时发一次 `stats_request`。
   - **牌桌**（`UI_UX_SPEC.md` §4.12，v1.7）：HUD「战绩」按钮弹出同一份数据，随时可看。
   - **结算页**（`UI_UX_SPEC.md` §9.2/§9.6，v1.7）：按钮上方内嵌「汇总 + 最近 ≤10 局（2 列 × 5 行、不显示时间）」；收到 `game_end` 后发一次 `stats_request`。
6. `SCREEN_LOGIN` 的坐标随账号模式扩展（`UI_UX_SPEC.md` §3.2 的修订表）；`SCREEN_LOBBY` 与 `SCREEN_RESULT` 的战绩块坐标见该文档 §12.1 与 §9.2。

## 6. 服务端实现约束（不可协商）

1. **DB 访问绝不发生在 actor 的 handler 里**：理由是**阻塞**——handler 内不得阻塞，会占住调度线程。因此所有查询走**独立的后台线程池**，结果以**消息**投回 actor（与 `WindowTimer` 同一模式）；`ConnectionActor` 的登录/注册/战绩 handler 只负责「校验 → 提交 → 立即返回」。
   （v1.2 更正论据：原文另举「handler 一旦挂起就不再持有 mailbox」为理由。自 v1.2 起每个 actor 的 handler 在 `AsyncGate` 保护下互斥（见 `GAME_DESIGN.md` §0.3），**挂起本身已不再破坏原子性**；DB 仍留在 actor 之外，只因为它会阻塞。实现形状不变。）
2. **状态变更只发生在消息 handler 里**：`logged_in_` / `player_id_` / `name_` / 在线表登记只在 `OnLoginResult` / `OnRegisterResult` 这类消息中修改。
3. **迟到结果**：若结果到达时连接已关闭，**不登记在线表、不发帧**（`SendFrame` 在关闭后本就是 no-op，但在线表必须显式释放）。
4. **关停顺序**：`gateway.Stop()` → `WindowTimer::Stop()` → **DB 工作线程 `Stop()`（join）** → `ex_actor::Shutdown()`。DB 线程的回调会往 actor 投消息，必须早于 actor world 拆除，与既有定时器同理。
5. 启动参数（`main.cc`）：`--mysql-host` / `--mysql-port` / `--mysql-user` / `--mysql-password`（缺省读环境变量 `MAHJONG_MYSQL_PASSWORD`）/ `--mysql-db`；源码内不含任何默认口令。

## 7. 验收

1. **游客链路零回归**：`gateway_smoke_test.py`、`room_full_game_test.py`、`client_headless_test.py` **一行不改**且全绿（其中 `login_ok` 字段形状的严格断言不变）。
2. **账号链路**：注册 → 自动登录 → 建桌 → 打完一局 → `stats_request` 与刚打完那局一致；重复注册 `USERNAME_TAKEN`；错口令 `AUTH_FAILED` 且累计 10 次断开；同账号二次登录 `ALREADY_ONLINE`。
3. **持久性**：重启服务端后同一账号 `player_id` 不变、战绩仍在。
4. **降级**：`--mysql-port 1` 起动 → 游客登录与整局照常，账号帧回 `INTERNAL`，服务端不崩。
5. `./scripts/format.sh` 无新增格式问题。

## 8. 修订记录

| 版本 | 修订内容 | 来源 | 是否改动冻结数值 |
| --- | --- | --- | --- |
| v1.2 | §6.1 第 1 条的**论据**更正：原文以「handler 一旦挂起就不再持有 mailbox」为理由之一；自 v1.2 起每个 actor 的 handler 由 `AsyncGate` 保证互斥（`GAME_DESIGN.md` §0.3 v1.8），挂起本身不再破坏原子性，DB 留在 actor 之外的理由收窄为「它会阻塞」。**实现形状（submit → 结果以消息回投）与 §1–§5、§6 其余约束不变** | 用户需求（本轮 actor 化重构） | **否**（账号模型、口令口径、身份映射、错误码、降级语义、战绩写入时机与字段**全部未动**；仅一处论据表述更新） |
| v1.1 | 依 captain 裁决（用户需求）把战绩的主界面扩到**客户端大厅界面**（`UI_UX_SPEC.md` §12，v1.8）：登录成功后先进大厅、那里常驻战绩；结算页战绩块与牌桌弹窗维持 v1.0 口径 | captain 裁决（用户需求） | **否**（账号模型、口令口径、身份映射、错误码、降级语义、战绩写入时机与字段**全部未动**；仅客户端多了一个展示位） |
| v1.0 | 首次制定：账号注册/登录、MySQL 持久化、战绩存档与查询、降级语义；对 `PROTOCOL.md`（帧 8→10、12→13，错误码 11→14）、`GAME_DESIGN.md`（§9/§11/§12/附录A 增补）、`UI_UX_SPEC.md`（§3 登录界面扩展、§10 映射）做增补式修订 | captain 裁决（用户需求） | **否**（既有 15 s / 底分 1 / ×4/×4/×2 / M∈{1,4,16} / 136·83·84·21 / 零和口径 / `seq` 作废 / 既有 11 条错误码语义与阈值全部不变） |
