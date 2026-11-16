# 天凤牌谱 → 规则回归

用**天凤真实牌谱**给本仓库的日麻引擎做回归：每手真实和牌都是 `riichi::ScoreHand` 的一道
考题，期望值由天凤给出（符 / 番 / 点数 / 满贯分档 / 役种集合 / 宝牌张数）。

这条链路的价值在于覆盖**人写测试想不到的局面**（一发+抢杠、海底+杠、赤牌在副露里…），
并且期望值来自外部权威，不是自己算给自己看。

## 三条命令

```bash
# 1. 抓（可选的便利脚本；也支持直接给已有的 mjlog 目录）
python3 game/tools/tenhou_fetch.py --out game/tools/tenhou_corpus --days 1 --limit 20

# 2. 转成用例 JSON（解析 + 自校验 + 上下文推导）
python3 game/tools/tenhou_convert.py --mjlog game/tools/tenhou_corpus/*.xml \
    --out build/tenhou_cases.json

# 3. 跑引擎比对（退出码 0 = 全部一致）
cmake --build build --config Release --target tenhou_rules_check
./build/game/tenhou_rules_check build/tenhou_cases.json
```

`ctest -R tenhou` 也会跑一遍（语料在 `game/tools/tenhou_corpus/`，用例 JSON 在配置期不存在时该
测试 SKIP —— **牌谱不入库**，见下）。

## 文件

| 文件 | 作用 |
| --- | --- |
| `tenhou_mjlog.py` | mjlog XML 解析。牌 ID / 摸打 / 副露 / 结算编码的单一真值来源，带**逐手自校验** |
| `tenhou_convert.py` | 解析结果 → 判番用例 JSON，并把事件流推导成 `WinContext`（立直/一发/海底…） |
| `tenhou_rules_check.cc` | C++：读用例 JSON 跑 `ScoreHand`，逐项 diff |
| `tenhou_fetch.py` | 抓取（遵守天凤的 20 分钟 / 单会话 / size 判重 / 不再配布 等规约） |

## 编码要点（全部经真实牌谱核对，不是照抄教程）

- **牌 ID = 牌面 × 4 + 副本**；赤 5 = `16 / 52 / 88`（5m/5p/5s 的副本 0）。
- `<T><U><V><W>` 摸牌、`<D><E><F><G>` 弃牌，值都是裸牌 ID。ElementTree 会把 `<T93/>` 解成
  tag `"T93"` —— 数值要从 tag 尾部取。
- 弃牌**不区分摸切/手切**，只能跟「这家上一次摸到的牌」比对。
- `<AGARI hai=...>`：`hai` 是**门前手牌 + 和牌张**（荣和 тоже 含），`machi` 是和牌张，
  `yaku` 是 `[役ID, 番]` 平铺，`doraHai` / `doraHaiUra` 是**指示牌**而不是宝牌本身
  （46 手真实和牌按指示牌换算 46/46 一致，按宝牌读只有 40/46）。
- `ten[1]` 是**点数**（7800），`sc` 的差分是「百点」单位。`ten[1]` **不含**本场与供托。
- 加杠 `<N>` **就地升级已有的碰**（组数不变），不是新增一组 —— `.hai` 的张数按组数算才对齐。

## 自校验（`tenhou_mjlog.py`）

错了就抛 `MjlogError`，绝不静默：

| 断言 | 抓住什么 |
| --- | --- |
| 弃牌在重构手牌里 | 摸打追踪错位 |
| 副露消耗的牌在重构手牌里 | 吃/碰/杠位解码错 |
| `AGARI.hai` 长度 = `14 − 3×副露组数` | 加杠被当成新组 |
| 赤牌张数（门前+副露）== `yaku` 里 54 的番 | 赤 5 判定 |
| 副露的 `claimed` == 该家最近一次弃牌 | `from_seat` 相对座位解码 |

用 25 个真实半庄 / 232 手和牌验证：**0 失败**。

## 上下文一律从事件流推导

天凤的 `AGARI` 只给结果，不给「一发」「海底」这类标志。从 `yaku` 反推再喂给 `WinContext`
就是拿答案喂自己，判番 bug 会被掩盖。所以每个标志都只由事件顺序决定：

| 标志 | 口径 |
| --- | --- |
| `riichi` | 和牌者本局有 `<REACH step="1">` |
| `ippatsu` | 宣言后无鸣牌（暗杠不算），且自家此后**恰好摸过一次**（自摸）或**一次都没摸**（荣和） |
| `rinshan` | 和牌张就是本人上次杠之后的岭上补牌 |
| `chankan` | 荣和的那张正是他家刚加杠的牌 |
| `haitei` / `houtei` | 活牌摸过 `70 − 杠数` 张（牌山摸空）—— **不能用「这是最后一个动作」判**，一局在和牌时结束，最后一张弃牌永远满足它 |
| `tenhou` | 庄家第一次摸牌即自摸，此前无打牌 |
| `chiihou` | 闲家第一次摸牌即自摸，此前无鸣牌 |

`tenhou_convert.py` 把推导结果与天凤 `yaku` 里的条件役 id 逐条交叉校验（一发 2、抢杠 3、
岭上 4、海底 5、河底 6、天和 37、地和 38）；不一致的用例**不入库**并计入统计 ——
「推导写错」和「引擎判番写错」因此不会混在一起。

在 20 个半庄 / 183 手和牌上，183/183 全部一致。

## 已知口径差异（有意为之，不是 bug）

- 天凤把宝牌 / 里宝 / 赤宝牌（52/53/54）也算进 `yaku`；本工具把它们拆成独立字段比较。
- 天凤的自风 / 场风 / 三元役牌各有 id（10..20），本仓库统一成 `yakuhai`，因此役种集合按
  `yakuhai` 归一后比较，番数由引擎自己算。
- 人和（36）引擎没有对应役种 ⇒ 该手牌跳过（当前语料里未出现）。
- 三色同刻（26）**v1.26 起已实现**（见下节）；`tenhou_convert.py` 把它映射到
  `sanshoku_doukou`，不再整手跳过。

## 已修复的引擎缺陷（2026-11-16，183 手真实和牌全部一致）

第一次跑通时 183 手里有 42 处差异，逐条定位后是 5 个独立缺陷，**全部已修**：

| # | 现象 | 最小复现 | 修法 |
| --- | --- | --- | --- |
| 1 | **自风依赖场风**。`SeatWindOf` 让「庄家自风 = 场风」，南场庄家被算成南家 ⇒ 南刻子多 1 番 | 南场、庄家自和、`2z`(南) 刻子 | 自风只由「相对庄家的座位」决定（庄家恒为东家）；`round_wind` 参数保留但不参与推导 |
| 2 | **和牌张落进明副露被误判等待型**。`WaitOf` 也遍历 `from_meld` 的组 | 明吃 `7m8m9m` + 暗手 `6m7m8m`，荣和 `8m` 被判成嵌张 | 等待型只看暗手拆出的组（和牌张物理上只可能在门前手牌里） |
| 3 | **同一拆法内多等待型只取第一个**。和牌张同时落在两组暗顺里时少了平和 | 暗手 `0m 2p3p4p 4m4m 4s 5s5s 6m6s6s 7m7s`，自摸 `5s` | 新增 `WaitsOf` 返回全部候选，判番/算符对每个候选各算一遍取最优（高点法） |
| 4 | **荣和补成的刻子按暗刻计符** | 暗手 `1m1m1m 1p1p1p 1s1s 8s8s8s` + 明碰 `6p6p6p`，荣和 `1p` | `FuForDecomposition` 把「荣和补成的门前刻子」按明刻计（2/4 符） |
| 5 | **七对子提前返回压过更高的标准型** | `11s 1z1z 22s33s 66s77s 88s` 荣和 `3s`（二盃口+混一色 6 番） | 七对子改为与标准型一起参与取最优（`DetectYaku` / `FuOf` / `ScoreHand` 三处都改） |
| 6 | **国士十三面误判**。判据只看 `counts[和牌张] == 2`，荣和时 concealed 已含和牌张，于是「本来就有一对 + 和牌张补缺」也被当成十三面 ⇒ 多给 1 倍 | 13 种各一张 + `6z` 一对，荣和 `1s` | 判据改为「把和牌张去掉后，剩下 13 张是不是十三种各一张」 |

## 三色同刻（v1.26，新增役种）

引擎原先**没有**三色同刻：`protocol::YakuId` 里没有这个 id，判番也不产出它。

> 更正：我一度写成「真实牌谱语料里它出现过」，那是错的。实测 20 个半庄 / 183 手和牌里
> 天凤的 `yaku` id 26（三色同刻）**出现 0 次** —— 三色同刻比三色同顺稀少得多。所以这个役
> 属于「引擎缺失 + 语料也没覆盖」，补它靠的是日麻规则而不是语料证据。

| 位置 | 改动 |
| --- | --- |
| `protocol_enums.h` | `kSanshokuDoukou = 48`，`kYakuMax` 47 → 48 |
| `protocol/enums.json` | `yaku` 域追加 `"sanshoku_doukou": 48` |
| `web/js/protocol_enums.js` | 同步 48 |
| `docs/PROTOCOL.md` | §1.3.1 表追加第 48 行、标题 47 → 48 值、新增 v1.26 记录 |
| `riichi_rules.cc` | `YakuForDecomposition` 按「花色 × 数字」统计刻子并交叉判断；`YakuHan` 给 2 番（副露 1 番） |
| 客户端 | `tiles.py` / `ui.js` 加中文名「三色同刻」 |

**为什么追加在末尾而不是插在 `win_ron`(46)/`win_tsumo`(47) 之前**：那张表**禁止重编号**
（`enums.json` 顶部明写 "NEVER renumber: append only at the end of a domain"）。46/47 是
`results[].win_type` 的和牌方式哨兵，把它们往后挪会改掉所有已冻结的 golden 与既有客户端。
新值排在第 48 位，语义上仍是「番种」，与 46/47 的「和牌方式」互不干扰（客户端本来就按 id
查名字表，不依赖顺序）。

用例（`RiichiRareYaku.*`）：门前 2 番、副露 1 番、以及「同刻不得算成同顺 / 同顺不得算成同刻」
两条反向断言。

第 1 条是**规则错误**（不只是符/番算错），改它的同时更新了：
`riichi_rules_test.cc` 的 `RiichiWind.RoundAndSeatWind`（原先把错口径写成了期望值）、
新增 `RiichiYakuhai.DoubleEastWindInEastRound` / `SouthRoundDealerSouthTripletIsOneHan` 两个
正反例、以及 `PROTOCOL.md` §12.9 的「庄家的自风恒等于场风」。

riichi 的 golden 转录因此重新冻结（**帧数不变** 4672/2914/2969，只有 `game_end` 的
fu/han/points/yaku 变了）。

修复后：`tenhou_rules_check` **183/183 全部一致**，`riichi_rules_test` 76/76，
`mahjong_riichi_game_test` 21/21。

## 真实语料覆盖不到的部分（补在单测里）

天凤**没有**公开的「役满一览」接口（`/sc/yakuman/` 等均 404），归档日志每行只有「谁和了、
得分」而**没有役种**，所以真实牌谱撞不到役满 —— 第一版 20 半庄 / 183 手和牌里役满为零，
`chankan` / `rinshan` / `houtei` / `chanta` / `sankantsu` 等低频役同样缺席。

这些形态改用 `riichi_rules_test.cc` 里人工构造的手牌覆盖（`RiichiYakuman.*` 与
`RiichiRareYaku.*`），与真实语料互补：

- 真实语料保证「常见役在**真实局面**下算对」（含赤牌、副露、立直、一发等真实组合）；
- 人工用例保证「**稀有形态**不漏判/不错倍数」（天和、地和、国士/十三面、四暗刻单骑、
  纯正九莲、大四喜、字一色、绿一色、清老头、四杠子、小四喜、抢杠、岭上、河底、三杠子）。

写这批用例时又查出一条真 bug（上表第 6 条：国士十三面误判），正好说明两者的互补性。

> 与本次改动无关的既有失败：`mahjong_classic_game_test` 的 golden 在干净 HEAD 上就红
> （classic 走 `mahjong_rules`，不经过 `riichi_rules`）；`protocol_enums_consistency` 需要
> `node`，本机没装。

## 规约

天凤明确禁止：牌谱与索引的**再配布**、做「不特定多数人下载牌谱」的服务、用于竞争产品的开发。
因此：

- `game/tools/tenhou_corpus/` 与 `build/tenhou_cases.json` **不入库**（见 `.gitignore`）；
- `tenhou_fetch.py` 遵守 20 分钟最小间隔、单会话、按 `list.cgi` 的 size 判重、只下 `.log.gz`；
- 语料只用于本地回归，CI 上该测试 SKIP 是正常状态。
