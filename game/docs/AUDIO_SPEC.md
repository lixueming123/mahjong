# AUDIO_SPEC v1.0 — 麻将 B 批「手感表现」音效资产规格

| 项 | 值 |
| --- | --- |
| 版本 | v1.0（首版；B 批「手感表现」交付） |
| 作者 | 雷鸣远（音效总监） |
| 日期 | 2026-10-22 |
| 资产落位 | `game/client/assets/sfx/sfx_<id>.ogg`（16 条，实测合计 **95.7 KiB**） |
| 授权归档 | `game/client/assets/sfx/CREDITS.md` |
| 配套规格 | `game/docs/FEEL_SPEC.md` **v1.6（冻结）**（触发点/优先级/打断/时长上界/格式参数） |
| 上游事实来源 | `game/docs/PROTOCOL.md` **v1.7（冻结）**（帧字段，含 S16 `tenpai`）、`game/client/game_state.py`（客户端消费面） |
| 素材来源 | **开源音效包（用户拍板）**，全部为 **CC0 1.0**；不做程序合成、不引入需联网校验的素材 |

---

## 0. 权威边界与本文的差异记录

### 0.0 版本对齐（已复核，不是假设）

本文初稿对齐 `FEEL_SPEC` v1.3；交付时该文档已冻结在 **v1.6**（`PROTOCOL` 同步到 **v1.7**）。我逐条比对过 §5 的两版差异：

- **§5.1 播放口径（F-AU-00 / 00b / 00c / 00d）、§5.2 的 16 个 id 与时长上界、§5.3 D1–D5、§5.4 F-AU-20..26 —— 与 v1.3 逐字一致，未变**，本文的资产与合规结论**不受影响**。
- 实质变化都在**本文不拥有的区域**（听牌提示/可吃碰提示/自动理牌/动画），其中两处与音效触发有关，已在 §3.1 / §4.2 按 v1.6 + `PROTOCOL` v1.7 重新写死：① `tenpai` 已成为正式帧（S16，带 `is_tenpai` / `waits[]` / `tenpai_seq`）；② `tenpai_seq` 独立于 `room.seq`。
- **一处仍未修**：§5.2 `F-AU-09/10` 的触发条件仍写作 `win.tsumo == false/true`，该字段只在 `game_end` 内。可执行口径见 §4.2 第 4 条（用 `action_event` 的 `from_seat`），已同步给创意总监。

### 0.1 两个权威不并存（依 FEEL_SPEC v1.6 §5 裁决）

| 事项 | 权威 | 本文的处理 |
| --- | --- | --- |
| 文件名与路径（`sfx_<id>.ogg` → `game/client/assets/sfx/`） | **FEEL_SPEC §5.3 D1** | 本文映射表**直接落到该路径**，未另立目录 |
| 格式参数（44.1 kHz 单声道 / 峰值 ≤ −3 dBFS / 首尾无静音 / 不循环） | **FEEL_SPEC §5.3 D2 + §5.1 F-AU-00c** | 本文只**复测并按实测登记**（§6.2），不另定第二套参数 |
| 触发点集合、优先级、并发、打断、去重、时长上界 | **FEEL_SPEC §5.2 / §5.4** | 本文只把每条**翻译成可执行判定条件**（§4.2），不改规则 |
| **音色选择**（哪条样本配哪个 id）、**许可证与授权归档**、**事件→文件映射** | **本文** | 本文为唯一出处 |

### 0.2 与主理人任务书的差异（需主理人知悉）

1. **路径**：任务书写 `game/assets/audio/<pack>/`；`FEEL_SPEC.md` §5.3 D1（v1.3 起，v1.6 未变）明确「`game/client/assets/sfx/` 是命名与路径的唯一权威，映射表不得另立目录」。
   → 本文与资产**按 D1 落地**（`game/client/assets/sfx/`），`CREDITS.md` 与样本同目录，避免同一批样本出现两个路径。
   若主理人改为任务书路径，只需改一个常量（映射表已按逻辑 id 组织，与物理目录解耦）。
2. **事件数**：任务书列 14 个事件；`FEEL_SPEC.md` §5.2 冻结为 **16 个 id**（局终/场终拆为 `hand_end` / `match_end`，并新增 `countdown_tick`）。
   → 本文按 16 个 id 交付，**任务书 14 个事件全部被覆盖**（见 §3.4 覆盖映射）。
3. **不做音频工程实现**：本文给出**配方**（源文件 + 入点 + 时长 + 增益 + 归一化 + 编码）与**实测结果**，不写客户端播放代码；播放逻辑的实现依据见 §7。资产文件已生成并逐条复测（§6.2）。

---

## 一、声音基调

### 1.1 一句话

> **这是一张安静的桌子。声音只负责「确认动作已经发生」，不做表演。**

### 1.2 三条不可违反的基调（承 `FEEL_SPEC` §5.2「三条音色总原则」）

1. **不抢牌桌信息**：一局里出牌会出现 20 次以上。宁可寡淡，不可悦耳过头 —— 任何一条音效的「好听」都不得以「抢走听牌/鸣牌提示的注意力」为代价。
2. **同族必须同材质**：动作应答族（碰/吃/明杠/暗杠/加杠）用同一套木材与力度体系；收束族（荒庄/局终）用同一套闷木；和牌族与场终用同一套钢片铃（`Steel jingles`）。玩家不需要看文字就能听出「这是同一类事」。
3. **禁用项**（`FEEL_SPEC` §5.2 末条，逐条落到选材上）：无人声、无循环 BGM、单条样本内音高事件 ≤ 4、**不使用真实洗牌/牌堆倒塌声**（会被误判为「有人在鸣牌」）。因此 `Casino Audio` 的 `card-shuffle`、`cards-pack-open`、`dice-shake` **一律不用**。

### 1.3 整体音色方向

| 维度 | 方向 | 落地做法（可测量） |
| --- | --- | --- |
| 主材质 | **干木 + 硬质牌面**（麻将牌落在台呢上） | 主能量压在 **80–900 Hz**：`impactWood_light`（质心 299 Hz）、`impactWood_medium`（200–247 Hz）、`impactWood_heavy`（92–123 Hz）、`impactGeneric_light`（790–929 Hz） |
| 副材质 | 少量**轻金属/玻璃铃**，只用于「信息提示」与「达成」 | `glass_005`（2840 Hz）、`glass_001`（1984 Hz）、`Steel jingles`（质心 ~1040–1170 Hz） |
| 明确回避 | 高频长尾与「筹码/塑料片」的亮脆声作主线 | `Casino Audio` 筹码族质心 **6300–7700 Hz**：单次好听，20 次/局必疲劳 → 只取 `card-slide-2`（质心 4381 Hz）作 `tile_draw` 的**布面摩擦层**，且压到 −8 dB 当垫底 |
| 动态 | 弱起、短收；除和牌/场终外**不留长尾** | 见 §6.2 实测时长与峰值 |

### 1.4 情绪基线与三处关键反差

| 场景 | 情绪基线 | 声学手段 | 目标有效电平（50 ms 窗口） |
| --- | --- | --- | --- |
| 局内动作（摸/出/鸣/杠） | 中性 · 确认 | 干木、快收、无旋律 | **−19.7 ~ −24.2 dBFS** |
| 信息提示（轮到自己/听牌/倒计时） | 唤起 · 不催促 | 单点铃、极短 | **−21.1 ~ −31.6 dBFS** |
| 和牌（荣和/自摸） | 达成（全场最亮） | 钢片铃上行、允许长尾 | **−14.8 ~ −15.3 dBFS** |
| 场终 | 落幕（比和牌更完整） | 同族钢片铃，最长 | **−15.0 dBFS** |
| UI | 中性（最安静） | 纸/木轻触，≤ 43 ms | **−30.1 dBFS** |

> 三处反差（和牌 ≫ 动作 ≫ UI，相差约 **7 dB / 9 dB**）是本批「手感」的主结构：玩家在局内听到的一切都比和牌小一个数量级，和牌才有「炸一下」的份量。

---

## 二、候选音效包清单与授权审查

### 2.1 结论

| 角色 | 包 | 用途 |
| --- | --- | --- |
| **首选（主素材）** | **Kenney — Impact Sounds** | 牌张落桌、碰/吃/杠的全部主材质、收束族（10 条成品的源） |
| 配套（信息层） | Kenney — Interface Sounds | `ui_click` / `turn_start` / `tenpai_on` / `countdown_tick` |
| 配套（达成层） | Kenney — Music Jingles（仅 `Steel jingles` 族） | `win_ron` / `win_tsumo` / `match_end` |
| 辅助（1 层垫底） | Kenney — Casino Audio | 仅 `card-slide-2` 作 `tile_draw` 的布擦层 |

**为什么首选 Impact Sounds**：麻将的核心手感事件是「牌落在桌面上」，需要**同一套木质/硬质冲击**覆盖从「轻放」到「整块下压」的 6 级力度（`light / medium / heavy / soft / plate / generic`），而每个力度都有 **5 个变体**（可支撑后续去重复疲劳）。Kenney 的其他包没有这套力度体系：Casino 是纸牌/筹码（材质不对，且过亮），Interface 是电子 UI 短音，Jingles 是短乐句。只有 Impact 能同时满足「同族同材质」与「力度分层」。

### 2.2 逐包核实（许可证原文链接 = 官方页面，非二手转述）

| # | 包名 / 版本 | 来源页 URL | 许可证 | 许可证原文链接 | 授权义务 | 格式 / 采样率（实测） | 总体积（zip） | 音效数 | 有麻将/棋牌牌张碰撞音？ |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Impact Sounds 1.0（2019-12-19） | https://kenney.nl/assets/impact-sounds | **CC0 1.0** | https://creativecommons.org/publicdomain/zero/1.0/ | 无（署名自愿、可商用、可改、可再分发） | OGG Vorbis **44.1 kHz / 立体声** | 800,850 B（782 KiB） | **130** | **有（最接近）**：`impactWood_light/medium/heavy`（木牌）、`impactGeneric_light`（硬质轻触）、`impactSoft_heavy`（闷木+低频）、`impactPlate_*`（板/石）；无「麻将牌」专用录音 |
| 2 | Interface Sounds 1.0（2020-11-02） | https://kenney.nl/assets/interface-sounds | **CC0 1.0** | https://creativecommons.org/publicdomain/zero/1.0/ | 同上 | OGG Vorbis **44.1 kHz**，单声道为主（少数立体声） | 834,536 B（815 KiB） | **100** | 无（UI 短音：click/select/glass/confirmation/tick/switch…） |
| 3 | Music Jingles（2014/2017） | https://kenney.nl/assets/music-jingles | **CC0 1.0** | https://creativecommons.org/publicdomain/zero/1.0/ | 同上 | OGG Vorbis **44.1 kHz / 立体声** | 1,239,525 B（1.18 MiB） | **85**（5 族 × 17：8-Bit / Hit / Pizzicato / Sax / **Steel**） | 无（短乐句），**Steel 族**为钢片铃音色，用于和牌与场终 |
| 4 | Casino Audio 1.1（2012 首发） | https://kenney.nl/assets/casino-audio | **CC0 1.0** | https://creativecommons.org/publicdomain/zero/1.0/ | 同上 | OGG Vorbis **44.1 kHz / 立体声** | 876,839 B（856 KiB） | **54** | **有（棋牌类）**：`card-slide-*`（纸牌滑动）、`card-place-*`（落牌）、`chips-collide-*`（筹码互击，质心 6.6–7.1 kHz，最像硬牌互击但过亮）、`chips-stack-*`（筹码叠放）；**无麻将牌录音** |

**关于「麻将/棋牌类牌张碰撞音」的诚实结论**：四个包里**没有任何一个是麻将牌实录**。最接近的是 Casino 的 `chips-collide`（硬塑料互击）与 Impact 的木/板冲击。本文选择「用木/硬质冲击重建牌张落桌」，代价是与真实麻将牌音色不完全一致；收益是**不过亮、耐听、可做力度分层**。这是本方案的已知取舍，已列入 §8 风险。

### 2.3 实测下载记录（我实际拉取并核对，非引用他人结论）

四包均从**官方页面上的 `.zip` 直链**下载（链接取自 `https://kenney.nl/assets/<pack>` 页面内嵌的 zip 地址），并逐包解压核对。

| 包 | 下载 URL（官方直链） | 实测字节 | zip 完整性 | 抽样文件头（实测） | pygame 解码 |
| --- | --- | --- | --- | --- | --- |
| Impact Sounds | `https://kenney.nl/media/pages/assets/impact-sounds/87b4ddecda-1677589768/kenney_impact-sounds.zip` | 800,850 B | 通过（`unzip -l` → 134 项，`Audio/` 130 个 `.ogg`） | `impactWood_light_003.ogg`：`44100 Hz / 2ch / 266 ms`；`impactSoft_heavy_002.ogg`：`44100 Hz / 2ch / 572 ms` | ✅ `pygame.mixer.Sound()` 加载成功 |
| Interface Sounds | `https://kenney.nl/media/pages/assets/interface-sounds/fa43c1dd4d-1677589452/kenney_interface-sounds.zip` | 834,536 B | 通过（`Audio/` 100 个 `.ogg`） | `select_001.ogg`：`44100 Hz / 2ch / 43 ms`；`glass_005.ogg`：`44100 Hz / 1ch / 111 ms`；`tick_001.ogg`：`44100 Hz / 1ch / 23 ms` | ✅ |
| Music Jingles | `https://kenney.nl/media/pages/assets/music-jingles/f37e530b9e-1677590399/kenney_music-jingles.zip` | 1,239,525 B | 通过（`Audio/` 85 个 `.ogg`，5 族） | `jingles_STEEL13.ogg`：`44100 Hz / 2ch / 1052 ms`；`STEEL12`：1256 ms；`STEEL07`：1555 ms | ✅ |
| Casino Audio | `https://kenney.nl/media/pages/assets/casino-audio/2472606a04-1721639069/kenney_casino-audio.zip` | 876,839 B | 通过（`Audio/` 54 个 `.ogg`） | `card-slide-2.ogg`：`44100 Hz / 2ch / 584 ms`；`chips-collide-1.ogg`：259 ms | ✅ |

补充实测（选材的客观依据，非听感）：

| 文件 | 时长 | 峰值 | 起音(90%) | 谱质心 | 主频 前半→后半 | 说明 |
| --- | --- | --- | --- | --- | --- | --- |
| `impactWood_light_003` | 266 ms | −0.9 dB | 0.8 ms | 299 Hz | 256 → 90 Hz | 干木轻击，硬起音、快衰 |
| `impactGeneric_light_000` | 139 ms | −1.2 dB | 0.9 ms | 838 Hz | 490 → 504 Hz | 硬质轻触，比木轻、比木亮 |
| `impactWood_medium_000` | 333 ms | −1.0 dB | 1.4 ms | 210 Hz | 270 → 168 Hz | 中力度木击 |
| `impactWood_heavy_003` | 313 ms | −0.9 dB | 2.3 ms | 123 Hz | 134 Hz 恒定 | 整块下压，低频厚重 |
| `impactSoft_heavy_002` | 572 ms | −0.9 dB | 3.8 ms | 82 Hz | 94 → 87 Hz | 最闷，无高频，「隐忍」 |
| `jingles_STEEL13` | 1052 ms | −2.4 dB | 271 ms | 1170 Hz | 334 → 353 Hz（上行） | 3 个音高事件，短促收 |
| `jingles_STEEL12` | 1256 ms | −1.2 dB | 404 ms | 1050 Hz | 237 → 332 Hz（上行、跨度大） | 4 个音高事件，更厚 |
| `jingles_STEEL07` | 1555 ms | −2.6 dB | 273 ms | 1040 Hz | 334 → 297 Hz | 尾最长（−42 dB），最仪式化 |

### 2.4 排除清单与理由

| 候选 | 官方页面 | 结论 | 理由（按纪律，宁可少不要错） |
| --- | --- | --- | --- |
| OpenGameArt「Thwack Sounds」（AntumDeluge） | https://opengameart.org/content/thwack-sounds | **排除** | 页面 `License(s)` 字段显示为 **`MySFX` / `sfx`**（非标准 CC0 标签），只有描述文字写「Creative Common Zero (CC0)」。**许可字段与描述不一致 = 证据不足**，排除 |
| Freesound.org（CC0 过滤） | https://freesound.org | **排除** | 逐条许可、下载需登录。**无法在不登录条件下取得文件并归档原始许可页**，不满足 D4 的可审查要求；且逐条署名台账成本高 |
| Sonniss GDC Audio Bundle | https://sonniss.com/gameaudiogdc | **排除** | 体量为 GB 级（与本项目「素材进仓库、包体可控」冲突）；其条款对「作为独立文件/音效库再分发」有限制的说法，我**未从官方页面逐条核实**，故不作依据、直接排除 |
| 任何 GPL / LGPL 音频素材 | — | **排除** | 传染性会波及分发与源码授权方式；本项目不引入 |
| 任何 **CC-BY-NC** 素材 | — | **排除** | 与本项目商业发行路径冲突 |
| 任何 **CC-BY-ND** 素材 | — | **排除** | ND 禁止改作，而 D2 要求降混单声道 + 裁剪 + 归一化，**必然构成改作**，物理上不可能合规 |
| 麻将专用录音包 | — | **未找到可核实的 CC0 来源** | OpenGameArt 检索 `mahjong` 仅得 BGM（`Mahjongg NES BGM`）；Freesound 需登录（同上） |
| Kenney Casino Audio 作**主线** | 见 §2.2 | **不选（许可无问题）** | 材质是纸牌/筹码，质心 6.3–7.7 kHz 过亮；20 次/局会疲劳。**仅保留 1 条 `card-slide-2` 作摩擦垫底** |

---

## 三、声音清单（16 条，全部已生成并复测）

### 3.1 清单与触发（可执行条件）

> 优先级 / 时长上界 / 可静音 三列直接承 `FEEL_SPEC` §5.2；「触发条件」列把该表翻译成代码可判定形式（帧字段依据 `PROTOCOL.md` v1.6）。

| 声音名（id） | 类别 | 触发条件（可执行） | 时长区间（实测 / 上界） | 优先级 | 可与他人混播 |
| --- | --- | --- | --- | --- | --- |
| `ui_click` | UI | `MOUSEBUTTONDOWN` 且命中任意按钮/房间行/结算按钮 | 43 ms / ≤ 80 ms | P3 | 是（UI 组独占） |
| `tile_draw` | 牌张 | 帧 `type=="draw"` 且 `seat==my_seat`，在手牌插入后同一帧 | 159 ms / ≤ 160 ms | P1 | 是（ACTION 组） |
| `tile_discard` | 牌张 | 帧 `type=="discard_event"`（任一 `seat`） | 91 ms / ≤ 100 ms | P1 | 是（ACTION 组） |
| `call_pong` | 动作应答 | `type=="action_event"` 且 `action=="pong"` | 163 ms / ≤ 180 ms | P1 | 是（ACTION 组） |
| `call_chow` | 动作应答 | 同上且 `action=="chow"` | 154 ms / ≤ 180 ms | P1 | 是（ACTION 组） |
| `call_kong` | 动作应答 | 同上且 `action=="kong"`（明杠） | 252 ms / ≤ 260 ms | P1 | 是（ACTION 组） |
| `call_ankan` | 动作应答 | 同上且 `action=="ankan"` | 254 ms / ≤ 260 ms | P1 | 是（ACTION 组） |
| `call_kakan` | 动作应答 | 同上且 `action=="kakan"` | 235 ms / ≤ 240 ms | P1 | 是（ACTION 组） |
| `win_ron` | 和牌 | `type=="action_event"` 且 `action=="win"` 且 **帧内存在 `from_seat`**（`PROTOCOL` E8：荣和带 `from_seat`） | 1052 ms / ≤ 1200 ms | **P0** | **否：起播即让 ACTION/UI 静场** |
| `win_tsumo` | 和牌 | 同上且 **帧内无 `from_seat`**（`PROTOCOL` E7：自摸无 `from_seat`） | 1256 ms / ≤ 1400 ms | **P0** | 同上 |
| `draw_hand` | 收束 | `type=="game_end"` 且 `reason=="draw_exhausted"`，切页后 60 ms | 501 ms / ≤ 900 ms | P2 | 是 |
| `turn_start` | 信息 | `type=="action_request"` 且 `window=="discard"` 且本局该窗口**首次**出现，操作栏显示后 80 ms | 73 ms / ≤ 220 ms | P1 | 是 |
| `countdown_tick` | 信息 | `remaining_ms` 跨过 3000 / 2000 / 1000 各一次 | 12 ms / ≤ 60 ms | P2 | 是（UI 组，**默认关**） |
| `tenpai_on` | 信息 | 帧 `type=="tenpai"` 且 `is_tenpai==true`，且本地**上一次状态为隐藏**（false→true 的跨越），与听牌条 `F-TP-06` 出现动画**同起**；`is_tenpai==false` 只清空听牌条、**不发声** | 278 ms / ≤ 300 ms | P2 | 是 |
| `hand_end` | 收束 | `type=="game_end"` 且 `reason!="draw_exhausted"`，切到局终页后 100 ms | 333 ms / ≤ 400 ms | P2 | 是 |
| `match_end` | 场终 | `type=="match_end"`，切到场终页后 120 ms | 1555 ms / ≤ 1600 ms | **P0** | **否：起播即静场** |

### 3.2 源文件 → 成品映射与配方

「入点/时长」为毫秒；叠层用 `+`，偏移以该层起点计。同一套配方可逐字节复现（见 §6.3）。

| id | 成分（包 / 源文件） | 入点 | 时长 | 叠层偏移 · 增益 | 成品 |
| --- | --- | --- | --- | --- | --- |
| `ui_click` | Interface / `select_001.ogg` | 0 | 60 ms | — | `sfx_ui_click.ogg` |
| `tile_draw` | ① Casino / `card-slide-2.ogg`（布擦层）② Impact / `impactGeneric_light_000.ogg`（落手层） | 0 | 120 ms | ① 0 ms · **−8 dB**；② **105 ms** · 0 dB | `sfx_tile_draw.ogg` |
| `tile_discard` | Impact / `impactWood_light_003.ogg` | 0 | 95 ms | — | `sfx_tile_discard.ogg` |
| `call_pong` | ① Impact / `impactWood_medium_000.ogg` ② Impact / `impactWood_light_002.ogg`（第二击，质心更高） | 0 | 120 ms | ① 0 ms · 0 dB；② **60 ms** · −5 dB | `sfx_call_pong.ogg` |
| `call_chow` | ① Impact / `impactGeneric_light_001.ogg` ② Impact / `impactGeneric_light_004.ogg`（更轻更亮） | 0 | 85 ms | ① 0 ms · −1 dB；② **45 ms** · −4 dB | `sfx_call_chow.ogg` |
| `call_kong` | Impact / `impactWood_heavy_003.ogg` | 0 | 255 ms | — | `sfx_call_kong.ogg` |
| `call_ankan` | Impact / `impactSoft_heavy_002.ogg` | 0 | 255 ms | — | `sfx_call_ankan.ogg` |
| `call_kakan` | ① Impact / `impactWood_medium_001.ogg` ② Impact / `impactWood_heavy_000.ogg`（第二段更实） | 0 | 90 ms | ① 0 ms · 0 dB；② **85 ms** · −1 dB | `sfx_call_kakan.ogg` |
| `win_ron` | Jingles / `Steel jingles/jingles_STEEL13.ogg` | 0 | 1200 ms | — | `sfx_win_ron.ogg` |
| `win_tsumo` | Jingles / `Steel jingles/jingles_STEEL12.ogg` | 0 | 1400 ms | — | `sfx_win_tsumo.ogg` |
| `draw_hand` | Impact / `impactSoft_heavy_004.ogg` | 0 | 900 ms | — | `sfx_draw_hand.ogg` |
| `turn_start` | Interface / `glass_005.ogg` | 0 | 220 ms | — | `sfx_turn_start.ogg` |
| `countdown_tick` | Interface / `tick_001.ogg` | 0 | 60 ms | — | `sfx_countdown_tick.ogg` |
| `tenpai_on` | Interface / `glass_001.ogg` | 0 | 300 ms | — | `sfx_tenpai_on.ogg` |
| `hand_end` | Impact / `impactWood_medium_002.ogg` | 0 | 400 ms | — | `sfx_hand_end.ogg` |
| `match_end` | Jingles / `Steel jingles/jingles_STEEL07.ogg` | 0 | 1600 ms | — | `sfx_match_end.ogg` |

### 3.3 音量建议（`0..1`，对应 `Sound.set_volume()` 的「样本自身音量」）

> 计算口径承 `FEEL_SPEC` F-AU-00b：`最终 = 样本自身音量 × 组乘子 × 主音量(0.70)`。
> 「有效电平」= 成品文件在最响 50 ms 窗口的 RMS 经上述三级乘积后的实测值（dBFS），用于核对层次是否成立。

| id | 组 | 样本自身音量 | 有效电平 | 判据 |
| --- | --- | --- | --- | --- |
| `win_ron` | WIN（×1.00） | **1.00** | −14.8 dBFS | 全场最亮 |
| `win_tsumo` | WIN | **1.00** | −15.3 dBFS | 与荣和同量级，厚度差在音色不在音量 |
| `match_end` | WIN | **1.00** | −15.0 dBFS | 与和牌同量级，但更长 |
| `tile_discard` | ACTION（×0.80） | **1.00** | −22.1 dBFS | 出现频率最高，明确定位在「听得到但不烦」 |
| `tile_draw` | ACTION | **0.85** | −24.2 dBFS | 比出牌低 2 dB：摸是「自己的事」，出是「对人的事」 |
| `call_pong` | ACTION | **0.95** | −21.0 dBFS | 宣告性 |
| `call_chow` | ACTION | **1.00** | −22.8 dBFS | 比碰低 ~2 dB（更轻更滑） |
| `call_kong` | ACTION | **0.75** | −20.7 dBFS | 源素材低频能量大，**必须压**，否则盖住后续动作 |
| `call_ankan` | ACTION | **0.50** | −21.7 dBFS | 内敛：不向对手宣告牌面 |
| `call_kakan` | ACTION | **0.95** | −19.7 dBFS | 动作应答族中最实的一下（升级感） |
| `turn_start` | ACTION | **1.00** | −22.9 dBFS | 唤起注意但不催 |
| `tenpai_on` | ACTION | **0.60** | −21.1 dBFS | 铃音穿透力强，压低仍清晰 |
| `draw_hand` | ACTION | **0.50** | −21.7 dBFS | 与 `hand_end` 同量级（收束族） |
| `hand_end` | ACTION | **0.85** | −21.7 dBFS | 同上 |
| `ui_click` | UI（×0.55） | **0.70** | −30.1 dBFS | 比动作族低 8 dB，绝不抢注意 |
| `countdown_tick` | UI | **1.00** | −31.6 dBFS | 12 ms 干击，能量小；仍属「听得到就够」 |

> **音量是试听可调项**：以上数值由客观电平对齐得出，人耳复核后可整体 ±0.1 微调；**层次关系（WIN ≫ ACTION ≫ UI）不得变动**。

### 3.4 覆盖对照（任务书 14 个事件 → 本文 16 个 id）

| 任务书事件 | 覆盖 id |
| --- | --- |
| 摸牌 | `tile_draw` |
| 出牌（不同牌型可同音） | `tile_discard`（全牌型同一条，符合「可同音」；不在音效层区分牌型） |
| 碰 / 吃 | `call_pong` / `call_chow` |
| 明杠 / 暗杠 / 加杠 | `call_kong` / `call_ankan` / `call_kakan` |
| 荣和 / 自摸 | `win_ron` / `win_tsumo` |
| 荒庄 | `draw_hand` |
| 轮到自己（提示音） | `turn_start` |
| 按钮点击 | `ui_click` |
| 听牌提示出现 | `tenpai_on` |
| 局终 / 场终 | `hand_end` / `match_end`（FEEL_SPEC 拆两条） |
| —（新增） | `countdown_tick`（倒计时最后 3 秒，默认关） |

**覆盖率：任务书 14/14 事件全覆盖；FEEL_SPEC 16/16 id 全覆盖。**

---

## 四、分层与触发规则

### 4.1 材质族（决定「听起来是不是一家人」）

| 族 | 成员 | 统一要素 | 族内区分手段 |
| --- | --- | --- | --- |
| **牌张族** | `tile_draw`、`tile_discard` | 干木主音 + 硬起音（≤ 1 ms 源起音）+ 快收（不拖尾） | 摸牌多一层布擦（−8 dB）；出牌更硬更干 |
| **动作应答族** | `call_pong`、`call_chow`、`call_kong`、`call_ankan`、`call_kakan` | 全部来自 `impactWood_* / impactGeneric_* / impactSoft_*`，同一木材体系 | 力度（light→medium→heavy→soft）、击数（1/2 击）、间隔（45/60/85 ms）、谱质心（82→929 Hz 递减序） |
| **信息族** | `turn_start`、`tenpai_on`、`countdown_tick` | 全部来自 `Interface Sounds`，短、无旋律、单点 | 时长 12 / 73 / 278 ms；质心 2840 / 1984 / 7920 Hz；听牌更长更「上扬」 |
| **达成族** | `win_ron`、`win_tsumo`、`match_end` | 全部来自 `Steel jingles`（钢片铃） | 长度 1052 / 1256 / 1555 ms；音高事件 3 / 4 / 3；上行跨度 19 / 95 Hz |
| **收束族** | `draw_hand`、`hand_end` | 闷木（soft/medium，质心 79–247 Hz），**无任何旋律** | 荒庄 501 ms 最闷最低（82 Hz），局终 333 ms 中木 |
| **UI** | `ui_click` | 纸/木轻触，43 ms | — |

> 族内一致性是硬要求：`FEEL_SPEC` §5.2「同族必须同材质」。`call_ankan` 与 `call_kong` 同为木材体系，但暗杠走 `soft`（无高频、纯低频），明杠走 `heavy`（有板体共鸣）——**不会混淆，也不会出族**。

### 4.2 触发与抑制规则（可执行）

1. **`tile_discard` 不区分本家/他家**（`FEEL_SPEC`：任一 `discard_event`）。若试听认为他家过响，可对他家降 3 dB —— **需先改 FEEL_SPEC，不在 v1 私自加规则**。
2. **`turn_start` 只在「本局该窗口首次出现」触发**：以 `(id, window_seq)` 记账，同一 `action_request.seq` 只响一次。
3. **`countdown_tick` 用 `remaining_ms` 穿越判定**：记录上一帧 `remaining_ms`，当 `prev > 3000 >= now` / `prev > 2000 >= now` / `prev > 1000 >= now` 各触发一次；**不得**用「整数秒取整相等」判定（帧率抖动会漏触发）。
4. **和牌判定用 `from_seat`**：`action_event{action:"win"}` **无 `from_seat` ⇒ `win_tsumo`**，**有 ⇒ `win_ron`**（`PROTOCOL` E7/E8）。**不要**等 `game_end.win.tsumo`（`game_end` 偏晚，`PROTOCOL` E7 明确要求音效在 `action_event` 触发）。
   → 附带建议（需主理人裁决）：`FEEL_SPEC` F-AU-09/10 的触发条件写作「`win.tsumo == false/true`」，该字段只存在于 `game_end`，建议在 FEEL_SPEC v1.4 改成上述 `from_seat` 口径。
5. **`draw_hand` / `hand_end` 互斥**：以 `reason=="draw_exhausted"` 唯一分流，禁止两条都播。
6. **`tenpai_on` 的触发口径（`PROTOCOL` v1.7 S16）**：`tenpai` 是**独立帧**，携带 `is_tenpai` / `waits[]` / **`tenpai_seq`**；`is_tenpai:false` 是**清空信号**（服务端必须照发、`waits:[]`），因此「隐藏→显示」只能靠**本地保存上一次 `is_tenpai`** 做跨越判定（false→true 才播），**不得**用「收到 tenpai 帧就播」。
   **去重键例外**：`tenpai_seq` **不参与 `room.seq` 比较**（`PROTOCOL` §11.2），所以该 id 的去重键用 `("tenpai_on", tenpai_seq)`，不要拿 `room.seq` 当键。
   现状：客户端 `game/client/` **尚未消费 `tenpai` 帧**（全文检索无 tenpai 逻辑）；样本已交付，主程序按 `FEEL_SPEC` §2 接线后自动生效，缺失时不播也不报错。
7. **去重**：每帧播放前查 `(id, seq)`，同一 `seq` 的同一 id 在 **200 ms** 内只播一次（`F-AU-24`）。两处例外：① `tenpai_on` 用 `tenpai_seq`（见第 6 条）；② 完全不带序号的帧（如 `game_end` / `match_end`）用 `(id, 单调递增计数)` 退化处理，不参与去重比较。
8. **静音/缺资产**：`M` 关闭或文件缺失 ⇒ 不出声，但**照常推进时机判定**（不补播积压，`F-AU-25`）。
9. **切屏静场**：`SCREEN_TABLE ↔ SCREEN_RESULT ↔ SCREEN_LOBBY` 切换时三组通道 `fadeout(40 ms)`（`F-AU-26`）。

### 4.3 随机化与重复疲劳（v1 的现实约束与 v1.1 计划）

- **v1 现实**：`FEEL_SPEC` D1 冻结「16 个 id 各 1 个样本」，因此**本版没有变体池**，同一个 `tile_discard` 一局会响 20 次以上。
- **v1 的缓解手段（已生效）**：① 音量层次把出牌压在 −22 dBFS，不刺耳；② 样本极短（91 ms）无长尾，不糊下一次动作；③ `(id, seq)` 200 ms 去重挡住抖动连播；④ 选材时刻意避开高频亮脆（质心 299 Hz 而非 7 kHz）。
- **v1.1 建议（需 FEEL_SPEC 改 D1）**：每个牌张/动作 id 追加 2 个变体：`sfx_tile_discard_v2.ogg`、`_v3.ogg`（命名预留 `_v<n>`），播放层按 `seq % 3` 轮换 + 每次播放 `set_volume(base × rand(0.95, 1.05))`。变体源已在手：`impactWood_light_000..004`（5 个）、`impactGeneric_light_000..004`（5 个）等，**不需要新素材、不需要新授权**。
- **禁止**：用「随机变调」代替变体池（pygame 无原生变速变调，且变调会破坏材质族的音高一致性）。

---

## 五、混音架构

### 5.1 总线与通道分配（承 `FEEL_SPEC` F-AU-00 / F-AU-20，本文给出具体通道号）

| 组 | 通道 | 成员 | 组乘子 | 行为 |
| --- | --- | --- | --- | --- |
| **WIN** | **Channel 0** | `win_ron`、`win_tsumo`、`match_end` | 1.00 | 只被 P0 使用；起播时对 Channel 1/2 执行 `fadeout(30)`；自身**永不被抢占** |
| **ACTION** | **Channel 1** | `tile_draw`、`tile_discard`、`call_*`、`turn_start`、`tenpai_on`、`draw_hand`、`hand_end` | 0.80 | 新音**立即抢占**旧音（`stop()` 后起播，不做交叉淡入） |
| **UI** | **Channel 2** | `ui_click`、`countdown_tick` | 0.55 | 只与自己抢；**不打断** ACTION/WIN |
| （保留） | Channel 3–15 | — | — | `pygame.mixer.set_reserved(3)` 让 0/1/2 不被 `Sound.play()` 自动分配机制偷走 |

- **并发上限 3**（WIN 1 + ACTION 1 + UI 1）由「三个固定通道」天然保证，无需额外计数。
- **一个事件只占一个通道**：`call_pong` 的两层已在**资产阶段烘焙**成一条文件，因此运行时不需要一次播两条（这正是选择「烘焙叠层」而非「运行时叠层」的原因：并发上限只有 3，运行时叠层会把 ACTION 组吃满）。

### 5.2 电平与响度目标（实测校准值）

| 项 | 目标 | 实测 |
| --- | --- | --- |
| 样本峰值（文件级） | ≤ **−3.0 dBFS** | 16/16 通过：−3.03 ~ −3.45 dBFS（§6.2） |
| WIN 组有效电平 | ≈ −15 dBFS | −14.8 / −15.0 / −15.3 |
| ACTION 组有效电平 | ≈ −20 ~ −23 dBFS | −19.7 ~ −22.9（`tile_draw` 刻意 −24.2） |
| UI 组有效电平 | ≈ −30 dBFS | −30.1 / −31.6 |
| 组间反差 | ≥ 5 dB | WIN→ACTION **≈ 6.5 dB**；ACTION→UI **≈ 8 dB** |
| 静音开关后 | 绝对 0（无残留底噪） | 由 `Channel.set_volume(0)` 实现，样本本身首尾无静音段、无 DC 偏移 |

### 5.3 优先级（P0–P3）与打断矩阵

| 正在播 → 新事件 | P0 新起 | P1 新起 | P2 新起 | P3 新起 |
| --- | --- | --- | --- | --- |
| WIN 组正在播（P0） | 覆盖（和牌/场终不会同刻发生） | **不打断 WIN** | 不打断 WIN | 不打断 WIN |
| ACTION 组正在播 | **fadeout(30) 后停**，WIN 起播 | **立即抢占**（`stop()`→`play()`） | 立即抢占 | 不打断 ACTION |
| UI 组正在播 | **fadeout(30) 后停** | 不打断 UI | 不打断 UI | 立即抢占（UI 组内互抢） |

> 一句话实现口径：**P0 只在 Channel 0；ACTION 在 Channel 1；UI 在 Channel 2。P0 起播 = 先把 1/2 淡出，再播 0。**

---

## 六、音频规格与命名

### 6.1 准入线（`FEEL_SPEC` D2 / F-AU-00c）与逐条实测

要求：**44.1 kHz / 单声道 / 峰值 ≤ −3 dBFS / 首尾无静音段（首帧即 attack，起音 ≥ 5 ms 防爆音）/ 不循环 / 时长 ≤ 各自上界**。

| id | 文件 | 采样率 | 声道 | 实测时长 | 上界 | 余量 | 峰值 | 首部静音 | 尾部静音 | 体积 | 合规 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `ui_click` | `sfx_ui_click.ogg` | 44100 | 1 | 43 ms | 80 | 37 | −3.35 | 0 | 0 | 4,279 B | ✅ |
| `tile_draw` | `sfx_tile_draw.ogg` | 44100 | 1 | 159 ms | 160 | **1** | −3.03 | 2 ms（斜坡） | 0 | 5,223 B | ✅（紧） |
| `tile_discard` | `sfx_tile_discard.ogg` | 44100 | 1 | 91 ms | 100 | 9 | −3.41 | 0 | 0 | 4,225 B | ✅ |
| `call_pong` | `sfx_call_pong.ogg` | 44100 | 1 | 163 ms | 180 | 17 | −3.23 | 0 | 0 | 4,595 B | ✅ |
| `call_chow` | `sfx_call_chow.ogg` | 44100 | 1 | 154 ms | 180 | 26 | −3.32 | 0 | 0 | 4,710 B | ✅ |
| `call_kong` | `sfx_call_kong.ogg` | 44100 | 1 | 252 ms | 260 | 8 | −3.15 | 0 | 0 | 4,466 B | ✅ |
| `call_ankan` | `sfx_call_ankan.ogg` | 44100 | 1 | 254 ms | 260 | 6 | −3.37 | 0 | 0 | 4,301 B | ✅ |
| `call_kakan` | `sfx_call_kakan.ogg` | 44100 | 1 | 235 ms | 240 | 5 | −3.40 | 0 | 0 | 4,630 B | ✅ |
| `win_ron` | `sfx_win_ron.ogg` | 44100 | 1 | 1052 ms | 1200 | 148 | −3.19 | 1 ms | 0 | 11,053 B | ✅ |
| `win_tsumo` | `sfx_win_tsumo.ogg` | 44100 | 1 | 1256 ms | 1400 | 144 | −3.37 | 1 ms | 0 | 12,515 B | ✅ |
| `draw_hand` | `sfx_draw_hand.ogg` | 44100 | 1 | 501 ms | 900 | 399 | −3.34 | 0 | 0 | 4,753 B | ✅ |
| `turn_start` | `sfx_turn_start.ogg` | 44100 | 1 | 73 ms | 220 | 147 | −3.33 | 0 | 0 | 4,205 B | ✅ |
| `countdown_tick` | `sfx_countdown_tick.ogg` | 44100 | 1 | 12 ms | 60 | 48 | −3.22 | 0 | 0 | 3,941 B | ✅（见 §8 R3） |
| `tenpai_on` | `sfx_tenpai_on.ogg` | 44100 | 1 | 278 ms | 300 | 22 | −3.42 | 0 | 0 | 5,442 B | ✅ |
| `hand_end` | `sfx_hand_end.ogg` | 44100 | 1 | 333 ms | 400 | 67 | −3.45 | 0 | 0 | 5,066 B | ✅ |
| `match_end` | `sfx_match_end.ogg` | 44100 | 1 | 1555 ms | 1600 | 45 | −3.36 | 0 | 0 | 14,602 B | ✅ |

- **合计 98,006 B ≈ 95.7 KiB**（16 条），是「素材进仓库」可接受量级；不打包、不压缩成 zip。
- **起音（防爆音）**：全部样本在编码前施加 **6 ms 升余弦起音斜坡**（极短样本取 25% 长度上限：`countdown_tick` 3 ms、`ui_click` 6 ms），源素材本身起音 0.1–3.8 ms。实测「到 90% 峰值时间」：冲击类 **2.4–6.8 ms**，铃/乐句类 12.5–272 ms（源素材峰值靠后，非削波）。
- **不循环**：全部为一次性样本；`win_ron` / `win_tsumo` / `match_end` 的「长尾」是衰减，不是循环（OGG 无 loop 标记，`pygame.mixer.Sound` 默认 `loops=0`）。
- **解码验证**：16/16 用 `pygame 2.6.1（SDL 2.28.4）` 实测 `pygame.mixer.Sound()` 加载成功，`get_length()` 与 `soundfile` 读数一致。
- **启动预载实测**：16 条 `Sound()` 预载 **9.3–12.4 ms**（3 次取样），远低于 `F-AU-00d` 的 80 ms 预期。

### 6.2 命名规范

| 对象 | 格式 | 示例 | 备注 |
| --- | --- | --- | --- |
| 逻辑 id | `SFX_<ID>`（大写，见 §3 表的 id） | `SFX_WIN_RON` | 代码里引用逻辑 id，**不直接引用文件名**；映射表集中一处 |
| 交付文件 | `sfx_<id>.ogg`（小写，下划线） | `sfx_win_ron.ogg` | **`FEEL_SPEC` D1 既定，不得改** |
| 未来变体（需先改 D1） | `sfx_<id>_v<n>.ogg`（`n` 从 2 起） | `sfx_tile_discard_v2.ogg` | 预留位，v1 不使用 |
| 源素材留档 | `sfx_<id>.src.md`（可选的说明文本） | — | v1 不建；来源与配方已记录在本文 + `CREDITS.md` |

> 目录里**不放**原始整包（130+100+85+54 条），只放 16 条成品：授权归档在 `CREDITS.md`，复现配方在 §3.2/§6.3。

### 6.3 可复现构建配方（任何人可逐条重建）

统一处理链（对所有 16 条相同）：

1. **解码**：`soundfile.read(always_2d=True)` → `(L+R)/2` 降混单声道（44.1 kHz 原采样率，不重采样）。
2. **取段**：按 §3.2 的入点/时长截取；若该层短于要求时长则整条使用。
3. **叠层**（仅 4 条）：按 §3.2 的偏移把各层按增益相加进同一条缓冲。
4. **裁到上界**：`buffer[:cap]`，并在末尾施加 **12 ms 线性淡出**（仅当被裁）。
5. **去静音**：阈值 −55 dBFS（相对峰值），首端保留 2 ms。
6. **起音斜坡**：**6 ms 升余弦**（`f = min(6 ms, 25% 时长)`）。
7. **峰值归一化 + 迭代复测**：目标 **−3.4 dBFS**；编码后重新解码测峰，若 > −3.10 dBFS 则按 0.7 阻尼步长再压，最多 8 轮（Vorbis 编码器有 ~0.3–0.7 dB 过冲）。
8. **编码**：OGG Vorbis 单声道 44.1 kHz（`libsndfile` 默认质量，实测码率约 60–80 kbps）。

> 工具口径：处理用 `numpy` + `soundfile 0.14`（解码/编码），验证用项目自带 `pygame 2.6.1`。**处理脚本不随仓库提交**（避免引入第三方依赖到客户端运行环境）；配方已细到可手工复现，若主理人需要脚本归档，可另开一个 `tools/` 任务。

### 6.4 运行时约束（零联网 / 零收集）

- 音频文件随仓库分发；**运行时不下载、不联网校验、不上报**（`FEEL_SPEC` D4）。
- 缺文件 ⇒ 静音占位 + 一条 WARN，不崩溃、不阻塞启动（`F-AU-25` / D5）。
- 不引入音频中间件（无 FMOD/Wwise），只用 `pygame.mixer`。

---

## 七、接入方案（给主程序的最小实现依据）

> 本节只给「接哪、怎么接、边界在哪」，**不写客户端代码**（实现归主程序）。伪代码用于消除歧义，不是补丁。

### 7.1 混音器初始化（顺序是硬约束）

```python
# 必须在 pygame.init() 之前！pygame.init() 会自动 mixer.init()，
# 之后再 pre_init 不会改变缓冲区大小（当前 app.py:86 直接 pygame.init()）
pygame.mixer.pre_init(frequency=44100, size=-16, channels=1, buffer=512)
pygame.init()
try:
    pygame.mixer.init()                    # 已 init 时是幂等 no-op
    pygame.mixer.set_num_channels(16)
    pygame.mixer.set_reserved(3)           # 保护 Channel 0/1/2 不被 Sound.play() 抢走
except pygame.error as exc:                # 无声卡 / CI / SDL_AUDIODRIVER=dummy
    log.warning("混音器初始化失败（%s），本次运行静音", exc)
    audio_ok = False
```

| 参数 | 值 | 理由 |
| --- | --- | --- |
| `frequency` | **44100** | 与全部样本同源，**零重采样**（重采样会引入 1–2 ms 延迟且可能削峰） |
| `size` | **−16**（16 bit 有符号） | 与源素材解码位深一致 |
| `channels` | **1**（单声道） | 三段总线不需要立体声；省一半混音 CPU 与内存 |
| `buffer` | **512** 帧（≈ 11.6 ms/缓冲区） | 端到端延迟约 **20–30 ms**（双缓冲 + 混音），与 FEEL_SPEC 的 0/60/80/100/120 ms 触发偏移同量级，不会听出「慢半拍」；降到 256 可再省 6 ms，但低配机/虚拟机能听到爆音，**不建议**；1024 会到 ~46 ms，明显滞后 |
| 通道数 | **16** + `set_reserved(3)` | 三组各占一条固定通道；预留 13 条给未来变体/并发余量，且不参与自动分配 |

### 7.2 预加载（启动期一次，运行时零 IO）

```python
SFX_DIR = Path(__file__).parent / "assets" / "sfx"       # game/client/assets/sfx
SOUNDS: dict[str, pygame.mixer.Sound] = {}
for sid, base_vol in BASE_VOLUME.items():                # base_vol 见 §3.3
    p = SFX_DIR / f"sfx_{sid}.ogg"
    try:
        s = pygame.mixer.Sound(p)
        s.set_volume(base_vol)                            # 样本自身音量只设一次
        SOUNDS[sid] = s
    except (FileNotFoundError, pygame.error):
        log.warning("音效缺失或不可解码：%s（静音占位）", p)   # D5：不崩溃
```

- 实测预载 **9.3–12.4 ms / 16 条**，可放在 `GameApp.__init__` 的显示初始化之后、首次 `run()` 之前；**不得**在首帧渲染路径里做。
- 运行时只做 `channel.play(SOUNDS[sid])`，**不 decode、不读盘**。

### 7.3 播放（固定通道 + 优先级）

```python
CH_WIN, CH_ACTION, CH_UI = pygame.mixer.Channel(0), pygame.mixer.Channel(1), pygame.mixer.Channel(2)
GROUP_MULT = {"WIN": 1.00, "ACTION": 0.80, "UI": 0.55}
MASTER = 0.70

def play(sid: str, group: str, p0: bool = False):
    if not audio_ok or muted:                    # F-AU-25：静音仍照常走时机判定
        return
    if p0:                                       # F-AU-21：先静场再起播
        CH_ACTION.fadeout(30); CH_UI.fadeout(30)
        ch = CH_WIN
    else:
        ch = {"ACTION": CH_ACTION, "UI": CH_UI}[group]
        if ch.get_busy():
            ch.stop()                            # F-AU-22：立即抢占，不做交叉淡入
    ch.set_volume(GROUP_MULT[group] * MASTER)    # 与 Sound.set_volume 相乘
    ch.play(SOUNDS[sid])
```

**「和牌音不被打断」怎么保证（三层保险）**：

1. **专用通道**：`win_*` / `match_end` 只走 `Channel(0)`，ACTION/UI 的抢占逻辑永远碰不到它。
2. **不被自动分配偷走**：`set_reserved(3)` 之后，任何 `Sound.play()` 都不会分配 0/1/2 号通道 —— 因此**禁止**在音效层用 `SOUNDS[sid].play()`（它会挑「最久未用」的通道，可能正好是 Channel 0），必须 `Channel(idx).play(...)`。
3. **P0 先静场**：`Channel(0).play()` 之前对 Channel(1)/(2) 执行 `fadeout(30)`，保证和牌音不会与出牌/按钮音糊在一起。
   - 注意 `fadeout()` 是**异步**的（约 30 ms 内完成），不要 `fadeout()` 后立即 `stop()`，否则等于硬切（会有咔嗒声）。
   - `Channel(0)` 自身若在播（极端：场终覆盖和牌长尾），直接 `play()` 覆盖即可，`FEEL_SPEC` 未要求和牌之间交叉淡入。

**去重（F-AU-24）**：`last_played[(sid)] = (seq, mono_time)`；同 `seq` 且 `now - t < 0.2` 时丢弃。

### 7.4 静音开关与「音量持久化」

**先确认事实（已核对代码）**：

- `game/client/main.py` 的 CLI 只有 `--host / --port / --fullscreen / --log-level`；
- `game/client/` 全文**没有任何配置读写**（无 ini/json/toml 落盘，无 `Path.home()` 配置目录访问），A 批也没引入；
- 因此**项目当前不存在「客户端配置文件」这一层**。

**结论（v1 只做内存态，不自造存储层）**：

| 项 | 做法 |
| --- | --- |
| 静音开关 | 内存态布尔 `muted`；`M` 键切换（`FEEL_SPEC` F-G-09），切换时 `R_TOAST` 报「音效 已关闭/已开启」2.0 s |
| 静音的实现 | 置 `muted=True` 后**不播放**（并在切屏时 `fadeout(40)` 清场）；**不**改 `Sound.set_volume`（避免重开时音量错乱） |
| 全局一键关闭 | `--no-feel` 启动参数（`F-G-10`）= 音效 + 动画 + 两处提示一起关 |
| 持久化 | **不做**。理由：项目无配置层，为「音效开关」单独引入一套落盘（路径/格式/迁移/权限/测试）成本远大于收益，且会与既有 e2e（干净环境跑）冲突 |
| 若将来要做 | 由主程序在自己选的配置层里加一个 `audio.muted` 布尔 + `audio.master` 浮点即可；**本文不指定路径与格式**（避免第二个权威） |

**音量接口（供将来扩展，不用也不影响 v1）**：`MASTER` 与 `GROUP_MULT` 已是常量，任何设置面板只需要改这两个数 → 语义完整、无需改样本。

### 7.5 触发挂点（给主程序的接线清单）

| 事件源（现有代码位置） | 该播什么 |
| --- | --- |
| `game_state._apply_draw()`（`type=="draw"`，本家） | `tile_draw` → ACTION |
| `game_state._apply_discard_event()` | `tile_discard` → ACTION |
| `game_state._apply_action_event()`，`action in ("pong","chow","kong","ankan","kakan")` | 对应 `call_*` → ACTION |
| `game_state._apply_action_event()`，`action=="win"` | `"from_seat" in frame` ? `win_ron` : `win_tsumo` → **WIN（P0）** |
| `game_state._apply_game_end()`，`reason=="draw_exhausted"` | `draw_hand`（切页 60 ms 后） |
| `game_state._apply_game_end()`，`reason!="draw_exhausted"` | `hand_end`（切页 100 ms 后） |
| `game_state._apply_match_end()` | `match_end`（切页 120 ms 后，**P0**） |
| `game_state._apply_action_request()`，`window=="discard"` 首次 | `turn_start`（操作栏显示后 80 ms） |
| 听牌条 隐藏→显示（`tenpai{is_tenpai:true}` 跨越，待实现） | `tenpai_on` |
| `remaining_ms` 跨 3000/2000/1000 | `countdown_tick`（UI 组，默认关） |
| 按钮/房间行/结算按钮 `MOUSEBUTTONDOWN` 命中 | `ui_click` → UI |

> 全部挂点都在 `game_state` 的既有分发里，**不需要改协议、不需要服务端配合**；音频层只做「读状态已变化 → 播一次」，**不参与任何判定**（`FEEL_SPEC` R1）。

### 7.6 与既有测试/CI 的关系

- `SDL_AUDIODRIVER=dummy`（或无声卡）时 `pygame.mixer.init()` 可能抛 `pygame.error` → 必须 `try/except` 降级为静音，**不得**让 e2e 挂掉（现有 `app.py` 对 `set_mode` 失败已有同样的降级写法，照抄即可）。
- 建议给 `main.py` 加 `--mute`（等价 `muted=True`）方便 CI 稳定，但**属可选**，不在 FEEL_SPEC 强制项内。

---

## 八、风险与待办

| # | 风险 / 待办 | 影响 | 处置 |
| --- | --- | --- | --- |
| **R1** | **本方案未经人耳试听**：选材依据是客观声学特征（时长/谱质心/起音/主频走向/音高事件数），不是听感 | 可能出现「参数合规但听感不对」 | **必须做一次 20 分钟的人耳 A/B**：①`tile_discard` 连打 20 次是否烦；②`win_ron` vs `win_tsumo` 盲听能否分辨（FEEL_SPEC 硬要求）；③`call_pong` vs `call_chow` 轻重大小是否可感。三处任一不过，按 §3.2 换同族变体（候补已列出：`impactWood_light_000/001/002/004`、`jingles_STEEL03/08/11/16`） |
| **R2** | `tile_draw` 实测 **159 ms**，上界 160 ms，**只剩 1 ms 余量** | 任何二次处理（重采样/淡出加长）都会超界 | 若需再压，优先缩短第 2 层（`impactGeneric_light_000` 落手层 60→55 ms），而不是压摩擦层 |
| **R3** | `countdown_tick` 仅 **12 ms**，有效电平 −31.6 dBFS | 小喇叭上可能几乎听不见 | 若试听认为太轻，把源换成 `tick_004`（55 ms，但起音 47 ms 偏懒）或提高 `MASTER` 之外的单独 base（上限 1.00 已到）。**不要**拉长样本 |
| **R4** | `win_tsumo` 与 `match_end` 同属 `Steel` 族、时长接近（1256 / 1555 ms） | 长尾在记忆里可能混 | 二者**从不同屏发生**（牌桌 vs 场终页），实践中难混；若盲听混淆，换 `match_end` → `jingles_STEEL03`（1387 ms，8 个音高事件超 ≤4 限制，故须先确认）或 `SAX` 族 |
| **R5** | `FEEL_SPEC` F-AU-09/10 的触发条件写成 `win.tsumo`，该字段只在 `game_end` 里 | 按字面实现会导致和牌音**晚到一整拍**（`game_end` 在 `action_event` 之后） | 按 §4.2 第 4 条用 `action_event` 的 `from_seat` 判定；建议 FEEL_SPEC v1.4 修文案（`PROTOCOL` E7/E8 已明确要求音效在 `action_event` 触发） |
| **R6** | `tenpai_on` 需要客户端先落地听牌条；`PROTOCOL` v1.7 已定义 `tenpai` 帧（S16），但客户端**尚未消费** | 该 id 短期不播 | 样本已交付、口径已写死（§4.2 第 6 条，含 `tenpai_seq` 去重）；等听牌条实现后接线即可，无需返工资产 |
| **R7** | v1 每 id **单样本**（D1 冻结），出牌一局 20+ 次 | 重复疲劳 | §4.3：v1 靠音量层次 + 极短 + 去重缓解；v1.1 建议加 2 个变体（源素材现成，零新增授权） |
| **R8** | 「牌张碰撞音」本质是近似替代（无麻将牌实录） | 音色与真实麻将牌有差 | 已知取舍，已在 §1.3/§2.2 明示；若将来找到可核实许可的麻将实录，替换路径是「换 §3.2 的源文件 + 重跑配方」，映射表与代码不用改 |
| **R9** | 素材包体积随包体增长（+95.7 KiB） | 移动端包体 | 量级可忽略；若未来要压，OGG 质量可降到 q3（约 −25% 体积，听感损失在短样本上不可闻） |

---

## 附：交付自检清单

- [x] 16 条样本落地 `game/client/assets/sfx/sfx_<id>.ogg`（合计 98,006 B）
- [x] 逐条实测：44.1 kHz / 单声道 / 峰值 ≤ −3 dBFS / 首尾无静音 / 时长 ≤ 上界
- [x] 16/16 `pygame.mixer.Sound()` 解码通过；预载 9.3–12.4 ms
- [x] 每个源的许可证**从官方页面与包内 `License.txt` 原文核实**，全部 CC0 1.0，无署名义务
- [x] 授权归档 `game/client/assets/sfx/CREDITS.md`（含四包许可原文摘录 + 修改说明 + 未来 CC-BY 署名模板）
- [x] 排除项留痕：OGA「Thwack」（许可字段非标准）、Freesound（需登录不可核实）、Sonniss（体量/条款）、GPL/NC/ND（原则）
- [x] 未改任何客户端代码；未引入网络依赖
