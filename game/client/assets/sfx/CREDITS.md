# 音效素材授权归档（CREDITS）

本目录 `game/client/assets/sfx/` 存放 B 批「手感表现」的 16 条音效样本（`sfx_<id>.ogg`）。
**全部素材均为 CC0 1.0（公有领域贡献），无署名义务、可商用、可修改、可再分发。**
下列署名是**自愿给出**的礼节性署名（CC0 不要求），同时也充当素材来源的可审查记录。

- 归档人：雷鸣远（音效总监）
- 归档日期：2026-10-22
- 授权权威文档：`game/docs/AUDIO_SPEC.md`（音色选择 / 许可审查 / 事件映射）
- 格式与路径权威文档：`game/docs/FEEL_SPEC.md` §5.3 D1/D2（`sfx_<id>.ogg` → `game/client/assets/sfx/`）
- 运行时依赖：**零联网**。所有样本已随仓库分发，运行时只读本地文件，不做任何联网校验、不请求任何授权服务。

---

## 1. 素材包清单（实际使用）

| 包名 | 版本 | 作者 | 来源页 | 许可证 | 许可证原文链接 | 本项目使用条数 |
| --- | --- | --- | --- | --- | --- | --- |
| Impact Sounds | 1.0（2019-12-19） | Kenney（Kenney Vleugels，kenney.nl） | https://kenney.nl/assets/impact-sounds | Creative Commons Zero (CC0) | https://creativecommons.org/publicdomain/zero/1.0/ | 10 |
| Interface Sounds | 1.0（2020-11-02） | Kenney（www.kenney.nl） | https://kenney.nl/assets/interface-sounds | Creative Commons Zero (CC0) | https://creativecommons.org/publicdomain/zero/1.0/ | 4 |
| Music Jingles | —（2014 / 2017 更新） | Kenney（Kenney Vleugels，kenney.nl） | https://kenney.nl/assets/music-jingles | Creative Commons Zero (CC0) | https://creativecommons.org/publicdomain/zero/1.0/ | 3 |
| Casino Audio | 1.1（2012 首发 / 2017 更新） | Kenney（Kenney Vleugels，kenney.nl） | https://kenney.nl/assets/casino-audio | Creative Commons Zero (CC0) | https://creativecommons.org/publicdomain/zero/1.0/ | 1（仅 `tile_draw` 的摩擦层） |

合计：**4 个包、18 条源文件**，产出 **16 条成品样本**（部分成品由 2 层叠加而成，见 AUDIO_SPEC §3）。

## 2. 授权义务

| 项目 | 结论 | 依据 |
| --- | --- | --- |
| 是否需要署名 | **不需要**（CC0 免除全部版权与邻接权主张） | 包内 `License.txt` 原文：「License (Creative Commons Zero, CC0)… Credit (Kenney or www.kenney.nl) would be nice but is not mandatory.」 |
| 是否可商用 | **可以** | 同上：「You may use these assets in personal and commercial projects.」 |
| 是否可修改 | **可以**（CC0 允许任意改作；本项目做了降混、裁剪、叠层、归一化，见 §3） | CC0 1.0 法条原文：https://creativecommons.org/publicdomain/zero/1.0/legalcode |
| 是否可再分发 | **可以**（素材随本仓库分发不构成违约） | CC0 1.0 法条原文（同上） |
| 是否要求「相同方式共享」 | **不要求**（CC0 无 copyleft） | CC0 1.0 法条原文（同上） |
| 商标限制 | Kenney 的**logo** 不得用于本项目品牌；**音频文件本身**无此限制 | Kenney 站点 FAQ / 包内 `License.txt` 未授予商标使用权 |

> 纪律说明：以上结论全部来自**官方页面与包内 `License.txt` 原文**（下载后逐字节核对的归档件），不采用素材站的二手转述。

### 2.1 各包 `License.txt` 原文（下载件逐字摘录）

**Impact Sounds（`License.txt`）**

```
Impact Sounds (1.0)

Created/distributed by Kenney (www.kenney.nl)
Creation date: 19-12-2019

------------------------------

License: (Creative Commons Zero, CC0)
http://creativecommons.org/publicdomain/zero/1.0/

This content is free to use in personal, educational and commercial projects.
Support us by crediting Kenney or www.kenney.nl (this is not mandatory)
```

**Interface Sounds（`License.txt`）**

```
Interface Sounds (1.0)

Created/distributed by Kenney (www.kenney.nl)
Creation date: 11-02-2020

------------------------------

License: (Creative Commons Zero, CC0)
http://creativecommons.org/publicdomain/zero/1.0/

This content is free to use in personal, educational and commercial projects.
Support us by crediting Kenney or www.kenney.nl (this is not mandatory)
```

**Music Jingles（`License.txt`）**

```
Music Jingles

by  Kenney Vleugels (Kenney.nl)

------------------------------

License (Creative Commons Zero, CC0)
http://creativecommons.org/publicdomain/zero/1.0/

You may use these assets in personal and commercial projects.
Credit (Kenney or www.kenney.nl) would be nice but is not mandatory.
```

**Casino Audio（`License.txt`）**

```
Casino Audio (1.1)

by  Kenney Vleugels (Kenney.nl)

------------------------------

License (Creative Commons Zero, CC0)
http://creativecommons.org/publicdomain/zero/1.0/

You may use these assets in personal and commercial projects.
Credit (Kenney or www.kenney.nl) would be nice but is not mandatory.
```

## 3. 本项目对素材所做的修改（CC0 允许）

全部 16 条成品经过同一套确定性处理，配方逐条记录在 `game/docs/AUDIO_SPEC.md` §6.3：

1. **立体声 → 单声道降混**（`(L+R)/2`），满足 `FEEL_SPEC` F-AU-00c「44.1 kHz 单声道」；
2. **裁剪**：按 §3 映射表的入点/出点截取，超出时长上界的一律裁到上界 + 12 ms 收尾淡出；
3. **叠层**（仅 `tile_draw` / `call_pong` / `call_chow` / `call_kakan` 4 条）：两段同族素材按毫秒级偏移与增益相加；
4. **首尾去静音** + 6 ms（极短样本 25% 长度）升余弦起音斜坡，防爆音；
5. **峰值归一化**至 **−3.0 ~ −3.5 dBFS**（编码后复测，Vorbis 过冲已迭代补偿）；
6. **重新编码**为 OGG Vorbis 单声道 44.1 kHz（`libsndfile` 默认质量）。

未做音高移调、未做变调变速、未做任何可识别原素材之外的音乐性改写。

## 4. 使用的源文件名 → 成品文件名

| 包 | 源文件（包内 `Audio/` 下） | 成品 |
| --- | --- | --- |
| Impact Sounds | `impactWood_light_003.ogg` | `sfx_tile_discard.ogg` |
| Impact Sounds | `impactGeneric_light_000.ogg` | `sfx_tile_draw.ogg`（第 2 层） |
| Impact Sounds | `impactWood_medium_000.ogg` + `impactWood_light_002.ogg` | `sfx_call_pong.ogg` |
| Impact Sounds | `impactGeneric_light_001.ogg` + `impactGeneric_light_004.ogg` | `sfx_call_chow.ogg` |
| Impact Sounds | `impactWood_heavy_003.ogg` | `sfx_call_kong.ogg` |
| Impact Sounds | `impactSoft_heavy_002.ogg` | `sfx_call_ankan.ogg` |
| Impact Sounds | `impactWood_medium_001.ogg` + `impactWood_heavy_000.ogg` | `sfx_call_kakan.ogg` |
| Impact Sounds | `impactSoft_heavy_004.ogg` | `sfx_draw_hand.ogg` |
| Impact Sounds | `impactWood_medium_002.ogg` | `sfx_hand_end.ogg` |
| Interface Sounds | `select_001.ogg` | `sfx_ui_click.ogg` |
| Interface Sounds | `glass_005.ogg` | `sfx_turn_start.ogg` |
| Interface Sounds | `glass_001.ogg` | `sfx_tenpai_on.ogg` |
| Interface Sounds | `tick_001.ogg` | `sfx_countdown_tick.ogg` |
| Music Jingles | `Steel jingles/jingles_STEEL13.ogg` | `sfx_win_ron.ogg` |
| Music Jingles | `Steel jingles/jingles_STEEL12.ogg` | `sfx_win_tsumo.ogg` |
| Music Jingles | `Steel jingles/jingles_STEEL07.ogg` | `sfx_match_end.ogg` |
| Casino Audio | `card-slide-2.ogg` | `sfx_tile_draw.ogg`（第 1 层） |

## 5. 建议的公开展示文本（游戏内「关于/致谢」页或 README，可选）

> 音效素材：Kenney（www.kenney.nl）— Impact Sounds / Interface Sounds / Music Jingles / Casino Audio，
> 均以 Creative Commons Zero (CC0 1.0) 发布，https://creativecommons.org/publicdomain/zero/1.0/
> 本项目对其做了降混单声道、裁剪、叠层与电平归一化处理。

> Sound effects: Kenney (www.kenney.nl) — Impact Sounds / Interface Sounds / Music Jingles / Casino Audio,
> released under Creative Commons Zero (CC0 1.0), https://creativecommons.org/publicdomain/zero/1.0/
> Modified for this project: mono downmix, trimming, layering, peak normalization.

当前客户端**没有**「关于页」（全仓库无 `关于`/`credits` 文案位）。因此本段文字只在仓库内归档，
不上界面；若后续新增关于页，直接粘贴上表即可。

## 6. 未来追加素材的纪律（CC-BY 等）

- **CC0 / Public Domain**：直接可用，署名自愿（仍按本文件格式登记）。
- **CC-BY**：可用，但**必须**能明确署名。登记时必须抄全作者名、作品名、来源 URL、许可证版本，并在
  「关于页/README」出现。署名模板：

  > 「<作品名>」by <作者>（<来源 URL>）， licensed under CC BY <版本>（<许可证 URL>），
  > 本项目做了 <修改说明>。

- **CC-BY-SA**：仅在确认「不改动素材本身」且能接受传染性条款时考虑；默认**排除**。
- **GPL / LGPL（音频素材）**、**CC-BY-NC\***、**CC-BY-ND\***、**「免费但禁止商用」的素材站条款**：
  **一律排除**。理由：GPL 的传染性会波及分发方式；NC 与本项目的商业发行路径冲突；ND 禁止本项目必需的
  裁剪/叠层（`FEEL_SPEC` D2 要求单声道裁剪归一化，本身就是改作）。
- **来源不明 / 许可字段非标准（如素材站自定义 license 标签）/ 需登录才能下载核实**：**一律排除**，
  宁可少不要错。

---

## 7. 语音合成资产（VO · 57 条）

> **本节独立成节，与 §1–§6 无关。** 下面的 57 条是**语音（VO）**，不是 CC0 素材，**不得并入 §1 的素材包清单**，也不适用 §2 的任何 CC0 结论。二者只在「都是本项目的音频资产」这一点上同属本归档文件。

| 项 | 内容 |
| --- | --- |
| 资产 | `vo_<id>.wav` × **57** 条（**WAVE / PCM 16-bit / 22050 Hz / 单声道**，不循环） |
| 存放位置 | `game/client/assets/vo/`（**不在本 `sfx/` 目录**；本文件是音频资产的统一归档入口） |
| 体积 | **2 930 868 B ≈ 2.80 MiB**（57 条 wav 合计；不含 tsv / manifest） |
| 文本真源 | `game/client/assets/vo/vo_lines.tsv`（57 行，`id<TAB>文本`，UTF-8 无 BOM） |
| 机器可读索引 | `game/client/assets/vo/vo_manifest.json`（逐条 `file / chars / ms / peak_dbfs / rms_dbfs`） |
| 生成方式 | **macOS 系统 TTS**：`/usr/bin/say`，音色 `Tingting`（`zh_CN`），语速 `-r 175` |
| 后处理 | `/usr/bin/afconvert -f WAVE -d LEI16@22050` 转码 → 本地 python3 **stdlib** 去 DC / 裁静音（−50 dBFS，前滚 5 ms，后滚 25 ms + 8 ms 淡出）/ 整句 RMS 归一化 −20.0 dBFS / 峰值钳 −3.5 dBFS |
| 生成日期 | **2026-10-23**（macOS 26.6.2） |
| 生成者 | 雷鸣远（音效总监） |
| 生成记录 | `game/docs/SETTLEMENT_BROADCAST_AUDIO.md` §8.4 / §8.4.1（参数、逐条实测值、串行管线） |

### 7.1 授权状态

- **来源**：macOS 系统语音合成（Apple 内置合成引擎 + `Tingting` 音色），**非第三方素材包**，未下载任何素材站文件。
- **当前状态**：**macOS 系统 TTS 合成，分发条款待核实，当前仅本地验收使用。**
- 也就是说：这 57 条**只用于本地 demo / 验收**，**未随任何对外发行包分发**。
- **待办（发行前必须关闭）**：由项目负责人核实 Apple 系统语音合成产出的分发条款，或改为自录音 / 已授权的 TTS 服务后全量重生成（重生成只需改音色参数，代码与字幕零改动）。
- **本节不对授权作法律判断**，只记录来源与当前使用范围，供负责人裁决。

### 7.2 与 CC0 素材的区别（防混淆）

| 维度 | §1 的 16 条 `sfx_*.ogg` | §7 的 57 条 `vo_*.wav` |
| --- | --- | --- |
| 性质 | 第三方 CC0 素材（经降混/裁剪/叠层） | 系统 TTS 合成语音 |
| 授权 | CC0 1.0，可商用、无署名义务 | **待核实**，当前仅本地验收 |
| 署名义务 | 无（本文件为礼节性归档） | 不适用（非 CC0） |
| 格式 | OGG Vorbis / 44.1 kHz / 单声道 | WAVE PCM / 22.05 kHz / 单声道 |
| 可否对外分发 | 可以 | **暂不可以，待核实** |
