# 英语单词学习（本地扩展应用）

在官方 `78/xiaozhi-esp32` 上加的第二个扩展应用（第一个是 `ble_arrival` 门口迎宾）。
**不依赖服务端做教学逻辑**：意图识别、词表、发音全在设备本地跑，服务端只负责把
ASR 识别出的文字推过来。

## 1. 交互剧本

```
用户：「开始学习英语单词」
设备：「好的，开始学习英语单词。请问学习第几单元？」
用户：「第三单元」
设备：屏幕显示第 3 单元第 1 个词卡 → 读单词 → 读拼写 → 读汉语意思
用户：「下一个」
设备：下一个词（同样三段）
...
设备：「本单元单词学习结束。」
```

支持的说法（意图解析器 `intent_parser.cc`）：

| 用户说 | 意图 | 设备行为 |
|---|---|---|
| 开始学习英语单词 / 学英语 | `kStart` | 播欢迎语 + 反问第几单元 |
| 第三单元 / 3单元 / unit3 / 第3个单元 | `kSelectUnit` | 开始该单元 |
| 下一个 / 下一个单词 | `kNext` | 推进到下一个词 |
| 重复一遍 / 再读一次 | `kRepeat` | 「好的，再读一遍」+ 重播当前词三段 |
| 结束学习 / 不学了 | `kExit` | 退出教学，恢复普通对话 |
| 有哪些单元 | `kListUnits` | 教学中播提示语；空闲时交给 MCP 工具 |

识别不出��话时：

- **选单元阶段** → 播「没听清楚，请再说一遍」，留在教学里
- **学习阶段且不是提问** → 同上
- **学习阶段且像在提问**（含「什么/怎么/为什么/哪/吗/?/谁/多少」）→ 退出教学，把这一轮交回服务端

## 2. 屏幕

全屏单词卡（`LcdDisplay::ShowStudyCard`），四行：

```
Unit 3    5 / 18        <- 30px 自带拉丁字体
afternoon                <- 40px 自带拉丁字体
a-f-t-e-r-n-o-o-n        <- 26px 自带拉丁字体
下午                      <- 48px 自带中文字体
```

- **卡片自带字体**，不依赖 assets 分区的通用字体。`font_noto_sans_common_20_4`
  是给聊天界面用的 20px 小字，字形还要靠服务端按需推送；放在单词卡上既小又
  不保证齐全，汉字会缺字形（显示不全，甚至整行空白）。
  自带字体由 `tools/gen_study_fonts.py` 按词表裁剪 Noto Sans / Noto Sans SC 生成，
  字符集 100% 覆盖（110 条词用到的 186 个汉字 + 44 个拉丁字符）。
- **字号由实测字宽决定**，不是拍脑袋：脚本用 fontTools 量出每行最长文本的像素宽度，
  超过可用宽度（320 - 左右各留 8 = 304px）就直接报错退出。
  实测：标题 210.6px / 单词 290.6px / 拼读 289.4px / 汉语 192.0px。
- **配色写死成白底黑字**（`lv_color_hex(0xFFFFFF)` 背景 + `0x000000` 文字 +
  `text_opa` 显式设 `LV_OPA_COVER`），**不跟随主题**：深色主题下背景会变成黑色，
  整屏看着就像黑屏。
- **四行用绝对定位**（y = 20 / 69 / 128 / 171），**不用 flex + `LV_SIZE_CONTENT`**：
  flex 容器的自动高度要等子对象布局完才算得准，算不准时整块内容会被裁掉，
  表现就是「屏上一个字都没有」。各字体的行高是固定的（30/40/26/48px 对应
  31/41/25/47px），直接算好偏移即可，不依赖任何自动布局。
- 文本一律 `LV_LABEL_LONG_MODE_CLIP`（单行，超宽就裁）：所有文本实测都能单行放下，
  折行只会把行高撑大。
- 拼写串最长 27 字符（`o-f-f-i-c-e w-o-r-k-e-r`），26px 下实测 289.4px，单行放得下。
- 不显示音标：保留在 `pep4a_words.json` 里备用。

## 3. 发音

全部是**预生成的离线音频**，编进固件（`EMBED_FILES`），不依赖服务端 TTS：

| 资源名 | 内容 | 数量 |
|---|---|---|
| `en_u{u}_w{n}.ogg` | 单词读音 | 110 |
| `en_u{u}_w{n}_spell.ogg` | 逐字母拼读 | 110 |
| `zh_u{u}_w{n}.ogg` | 汉语意思 | 110 |
| `prompt_*.ogg` | 7 条提示语 | 7 |

- 格式：Ogg-Opus，mono，24000Hz 输入，8kbps 码率，20ms 一帧
  （满足 `main/audio/demuxer/ogg_demuxer.cc` 的全部约束）
- 总体积 **598 KB**

### 3.1 播报编排

一次只往播放队列里推**一段**音频，靠 `on_playback_drained` 回调推进下一段
（`Application::Run()` 里的 `MAIN_EVENT_PLAYBACK_DRAINED` 分支）。
这样能精确知道「这一段放完了」，不会把两段连播误判成一段。

音频服务每推入一批数据就把 `playback_drained_notified_` 置回 false，队列排空时
只回调一次，所以「推一段 → 回调一次」是一一对应的。

### 3.2 本地播报必须在 Speaking 态

Speaking 态下 `application.cc` 会 `EnableVoiceProcessing(false)`，麦克风不会录到
设备自己的声音。所以本地播报前先切到 Speaking：

```cpp
if (GetDeviceState() != kDeviceStateSpeaking) {
    pending_local_asset_ = asset_name;
    SetDeviceState(kDeviceStateSpeaking);   // 状态切换是异步的
    return;                                  // 真正播放放到 HandleStateChangedEvent 尾部
}
```

必须等状态切换**生效之后**再推音频：`kDeviceStateSpeaking` 的处理里会
`ResetDecoder()`，而 `ResetDecoder()` 会清空播放队列——先推后切的话音频会被抹掉。

### 3.3 屏蔽服务端插话

教学期间服务端仍然会照常生成回答（官方服务端在 `stt` 之后就会调 LLM）。
设备端做两道拦截：

1. `OnIncomingJson` 里 `tts` / `llm` 消息直接丢弃（`local_learning_active_` 为真时）
2. `OnIncomingAudio` 里服务端的音频包直接丢弃

本地播报完后 `local_learning_active_` 置回 false，服务端恢复正常。

## 4. 符号命名的坑（重要）

`EMBED_FILES` 会给每个被嵌入的文件生成一对链接符号：

```
_binary_<路径>_start / _binary_<路径>_end
```

`<路径>` 怎么取，不同 IDF 版本/调用方式并不一致：可能只取文件名，可能相对组件
目录，也可能相对工程根目录。猜错就是链接期 `undefined reference`。

**本模块的做法**：三种取法**全部声明成弱符号**（`__attribute__((weak))`），
运行时挑第一个能解析出来的。这样无论构建机上的 IDF 用哪种规则都能工作，
也不会因为符号缺失导致构建失败。

```c
// english_audio_table.h（自动生成）
extern const char en_u3_w05_ogg_c0_start[] __asm__("_binary_en_u3_w05_ogg_start")
    __attribute__((weak));
extern const char en_u3_w05_ogg_c1_start[] __asm__("_binary_assets_english_en_u3_w05_ogg_start")
    __attribute__((weak));
extern const char en_u3_w05_ogg_c2_start[] __asm__("_binary_main_assets_english_en_u3_w05_ogg_start")
    __attribute__((weak));
```

注意 C++ 侧的标识符必须**每个候选都不同**（否则是重复声明），链接器实际看的
是 `__asm__` 里给的名字。

开机时会打一行自检日志，能看出用的是哪个候选、有多少音频可用：

```
I (1234) EnglishAudio: 337/337 audio assets resolved via candidate 0, 598.0 KB total
```

## 5. 容量

| 项 | 大小 |
|---|---|
| 音频（337 段） | 574 KB |
| 词表头文件 `word_table.h` | 12 KB |
| 音频索引表（flash 常量） | ~15 KB |
| 卡片自带字体（4 个子集） | ~203 KB |
| 代码 | ~40 KB |
| **合计** | **约 0.75 MB** |

app 分区上限 `0x3f0000` = 4,128,768 B（3.94 MiB），官方固件 app 约 2.95 MB，
**余量约 1.1 MB**。所以放得下，但余量不宽裕，需要 CI 实际构建确认。

放不下时的降级阶梯：

1. 音频码率 8k → 6k（省约 25%）
2. 只编前 3 个单元（省约 50%）
3. `CONFIG_ENGLISH_LEARNING_ENABLED=n` 关掉整个功能（省 100%）

**所以这个功能是 Kconfig 开关，默认关**：不打开的板子不占任何 flash，
行为与官方固件完全一致。

## 6. 开关与变体

```kconfig
config ENGLISH_LEARNING_ENABLED
    bool "Enable English word learning (英语单词学习)"
    default n
```

本仓库的变体（`main/boards/lckfb/szpi-esp32s3/config.json`）：

| 变体 | 开关 |
|---|---|
| `lichuang-dev` | 官方原版 |
| `lichuang-dev-ble` | `CONFIG_BLE_ARRIVAL_ENABLED=y` |
| `lichuang-dev-english` | `CONFIG_ENGLISH_LEARNING_ENABLED=y` |

BLE 迎宾和英语学习**分成两个变体**，因为两者都要抢 app 分区空间，合在一起能不能
放下要等 CI 实测。先各编各的，确认都不超限之后再考虑合并。

## 7. 数据与再生成

| 文件 | 作用 |
|---|---|
| `main/english_learning/pep4a_words.json` | **唯一真相**：6 个单元 110 个词条 |
| `main/assets/english/*.ogg` | 预生成的离线发音（337 个） |
| `main/english_learning/word_table.h` | 自动生成（构建时），已在 `.gitignore` |
| `main/english_learning/english_audio_table.h` | 自动生成（构建时），已在 `.gitignore` |

构建时 CMake 会自动重跑两个生成脚本（`scripts/gen_english_word_table.py`、
`scripts/gen_english_audio_index.py`），所以头文件不会和数据脱节。

想换教材/加单词：

1. 改 `pep4a_words.json`
2. 重新生成音频（需要 edge-tts + ffmpeg，见 `english-learning/README.md`）
3. 重新构建（两个头文件会自动更新）

## 8. 主机侧测试

```powershell
$root='D:\程序\08-utility-tools\xiaozhi-esp32-doubao'
$gxx="$root\tools\w64devkit\w64devkit\bin\g++.exe"
& $gxx -std=c++17 -Wall -Wextra -Werror -O2 `
  "-I$root\src\xiaozhi-esp32\main\english_learning" `
  "-I$root\tools\host-syntax\stubs" `
  "$root\src\xiaozhi-esp32\test\english_learning\english_learning_test.cc" `
  "$root\src\xiaozhi-esp32\test\english_learning\fake_audio.S" `
  "$root\src\xiaozhi-esp32\main\english_learning\english_learning.cc" `
  "$root\src\xiaozhi-esp32\main\english_learning\english_audio.cc" `
  "$root\src\xiaozhi-esp32\main\english_learning\intent_parser.cc" `
  -o "$root\tools\english_learning_test.exe"
& "$root\tools\english_learning_test.exe"
```

当前：**断言 139 条，失败 0 条**。

`fake_audio.S` 照抄了 ESP-IDF `EMBED_FILES` 生成的汇编布局（`_start` 紧挨着
`_end`），用来在主机侧复现「符号绑上了」这件事。用两个独立的 C 数组来伪造是不行的
——链接器会把它们先后顺序打乱，`end - start` 变成负数。

测试覆盖：

- 音频查表：候选 0 命中、候选 1 命中、三个候选都没定义的取不到、不存在的名字取不到
- 意图解析回归（43 条原有断言）
- 完整剧本：开始 → 问单元 → 选单元 → 逐词（含重复）→ 最后一个词 → 结束语
- 退出教学能恢复服务端播报
- 学习中途改问问题会把这一轮交回服务端

## 9. 上板验证（2026-11-25）

固件由 GitHub Actions 编出（fork `fangbb-coder/xiaozhi-esp32`，分支 `feature/english-learning`），
app 镜像 **3,678,336 B**，分区上限 4,128,768 B，**余量 450,432 B**。

### 9.1 开机自检

```
I (220) Board: UUID=... SKU=lichuang-dev-english
I (1220) MCP: Add tool: self.english_learning.list_units [user]
I (1240) EnglishLearning: hooks installed (ready=1)
I (1270) EnglishAudio: 337/337 audio assets resolved via candidate 0, 598.0 KB total
```

`candidate 0` 命中，说明这台 IDF 的 `EMBED_FILES` 符号名就是「只取文件名」；
但三候选弱符号方案本身不依赖这个结论，猜错也能跑。

### 9.2 完整剧本实测

```
I (172860) Application: >> 我要学习英语单词。
I (172860) EnglishLearning: text='我要学习英语单词。' intent=start unit=0 state=0
I (172860) EnglishLearning: state -> 1
I (172870) Application: server speech suppressed
I (172870) EnglishLearning: play prompt_start.ogg (step 1/2)
I (176000) EnglishLearning: play prompt_ask_unit.ogg (step 2/2)

I (51850) Application: >> 下一个。
I (51860) EnglishLearning: text='下一个。' intent=next unit=0 state=2
I (51860) EnglishLearning: play en_u4_w03.ogg (step 1/3)
I (53740) EnglishLearning: play en_u4_w03_spell.ogg (step 2/3)
I (56300) EnglishLearning: play zh_u4_w03.ogg (step 3/3)

I (65750) Application: >> 你。
I (67160) EnglishLearning: text='你。' intent=none unit=0 state=2
I (67160) EnglishLearning: play prompt_confused.ogg (step 1/1)

I (110300) Application: >> 退出。
I (110300) EnglishLearning: text='退出。' intent=exit unit=0 state=2
I (110300) EnglishLearning: state -> 0
```

- 选单元后逐词播放「英文 → 逐字母拼写 → 汉语意思」三段；
- 听不清 → 播 `prompt_confused.ogg`，**留在教学态**而不是退出；
- 「退出」→ 状态归零，服务端恢复正常；
- 唤醒词「你好小智」被 ASR 听成普通语句 → `intent=none`，安静忽略；
- 全程 **0 条 E 级日志，无 panic**。

> 单元 4 实测到第 8 个词时手动说了「退出」提前结束，所以「本单元单词学习结束。」
> 这句在板上没听到；该路径有主机侧单测覆盖（133 断言）。

### 9.3 烧录注意：只刷 app 槽

`merged-binary.bin` 里打包了 assets 分区（0x800000，约 1.78 MB），整包刷会冲掉
自定义唤醒词。正确做法是只把 app 镜像写进 ota_0 (0x20000) / ota_1 (0x410000)，
nvs、assets、bootloader、分区表全部保留。脚本见项目目录 `scripts/flash-app-only.ps1`。

## 10. 第二轮修复（2026-11-25）

### 10.1 「汉语意思显示有问题」的排查结论

用户反馈：有的汉语显示不全、有的意思不对不对应、有的完全没有。

**数据侧全部排除**：

| 检查项 | 结果 |
|---|---|
| `pep4a_words.json` 110 条 zh | 全部非空，且与英文一一对应（逐条核对） |
| `word_table.h` 110 条条目 | zh 全非空 |
| 音频资源名 vs 屏幕词表 | 同一个 `idx+1` 编号，不存在错位 |
| 字体字符集覆盖 | 110/110 全覆盖 |

**结论：问题在渲染层**，两个原因叠加：

1. **版式**：旧版把四行文字直接塞进全屏 flex 容器，容器高度固定、内容居中。
   内容总高一旦超过屏幕，超出部分就被上下裁掉——表现就是「最后一行（汉语）被切一半」
   或「整行看不见」。
2. **字体**：只能用 assets 分区里的 `font_noto_sans_common_20_4`（20px 通用字体，
   字形靠服务端按需推送）。中文既小，字形也不保证当下齐全，缺的地方就是空白或
   豆腐块——表现就是「有的完全没有」「看起来不对应」。

修复方式：卡片自带字体 + 自适应高度居中容器（见 §2）。

> 补充（第二轮）：用 esptool 读了板上 0x800000 的分区目录才确认——
> **设备上的 assets 分区里一个字体文件都没有**（23 个条目：srmodels + 20 个 gif 表情
> + index.json），index.json 也没有 `text_font` 条目，主题只能回退到内置的
> 纯拉丁字体。所以缺的不只是单词卡，**整个界面**（状态栏、聊天字幕、服务端回答）
> 当时都只能显示拉丁字符。整机的问题后来用混合 assets 包一并修掉了，
> 见项目 README §14.12。

### 10.2 顺带做的调整

- 拼读改小写：`a-f-t-e-r-n-o-o-n`（**音频保持大写字母拼读**，TTS 读大写字母是
  标准教学方式，不动音频可以避免引入读音风险）
- 中文释义精简成孩子向的 1~4 个字（`Ms`→女士、`there`→在那里、`over`→在上方 …），
  110 条中文音频全部重新合成
- 字号整体放大：标题 30px、单词 40px、拼读 26px、汉语 48px

### 10.3 教学结束回到「等待唤醒」

原来单元结束 / 用户说「退出」都是 `ResumeListening()`，设备留在连续对话态，
必须等用户主动开口。新增 `hooks.set_idle`：

- 单元播完 → 播 `prompt_unit_done.ogg` → `set_idle("本单元单词学习结束")`
- 用户说「退出」→ 播 `prompt_exit.ogg` → `set_idle("已结束英语学习")`
- `set_idle` 的实现：收起单词卡 → `SetDeviceState(kDeviceStateIdle)`
  （这个状态会 `EnableWakeWordDetection(true)` + `EnableVoiceProcessing(false)`，
  也就是「等唤醒词」）→ 再把收尾提示显示回来

> 收尾提示必须**等状态切换生效之后**再显示：待机状态的处理函数会
> `ClearChatMessages()`，提前显示会被擦掉。所以用 `Schedule()` 把显示动作排到
> 主循环里、状态切换之后执行（主循环里 `MAIN_EVENT_STATE_CHANGED` 先于
> `MAIN_EVENT_SCHEDULE` 处理，顺序有保证）。

> **用户改问别的问题时不能进待机**：那一轮要交回服务端回答，设备必须留在
> 聆听态，否则服务端的音频会被丢掉。所以 `Exit()` 分成两种：
> `Exit(true)` 回待机、`Exit(false)` 交回服务端。

### 10.4 「背单词时屏幕全黑，一个字都没有」

2026-11-26 用户反馈：进入背单词后屏幕全黑，没有任何字符，其它功能正常。

**排查过程**：

| 怀疑 | 结论 |
|---|---|
| 配色：深色主题下卡片背景变黑 | 成立，但只解释「黑」，解释不了「一个字符都没有」 |
| 版式：flex + `LV_SIZE_CONTENT` 高度算不准，整块内容被裁 | **最可能**：四个标签一起消失，正好等于「一个字符都没有」 |
| 自带字体没生效 | 上一版英文单词与拼读是能显示的（用户当时只抱怨汉字），说明拉丁字体没问题 |

**改动**：配色写死成白底黑字；四行改成绝对定位，彻底不用 flex 自动高度；
`LV_LABEL_LONG_WRAP` 换成 `LV_LABEL_LONG_MODE_CLIP`。

**顺带记一笔 LVGL 9.5 的三个坑**（都是编译器/头文件查出来的，不是猜的）：

| 写法 | 正确写法 |
|---|---|
| `lv_obj_set_style_scrollbar_mode` | `lv_obj_set_scrollbar_mode` |
| `LV_LABEL_LONG_CLIP` | `LV_LABEL_LONG_MODE_CLIP`（v9.5 枚举名带 `_MODE_`） |
| `LV_COORD_MAX` | 换成字面量 `10000`（量字宽用不上这个宏） |

**加了两处自检日志**，下次再出问题不用猜：

* 建卡片时量三个自带字体的字宽（`lv_text_get_size`）：
  `study font latin40 / latin26 / cjk48` —— 量出 0 就是字体数据有问题。
* 显示卡片时量实际文本的宽高与 y 坐标：
  `study text: word WxH at y=N` / `study text: zh WxH at y=N` —— 和设计值对不上就是布局问题。

### 10.5 真正的根因：plain 位图 vs 压缩位图（2026-11-26）

10.4 的两条改动（白底黑字 + 绝对定位）刷进去之后，用户反馈：
**屏幕不再全黑、有点亮了，但仍然一个字符都没有**。说明 10.4 猜错了方向。

查 LVGL v9.5.0 的 `src/font/fmt_txt/lv_font_fmt_txt.c` 源码，有这么一段：

```c
    if(fdsc->bitmap_format == LV_FONT_FMT_TXT_PLAIN) {
        /* 解到 draw_buf 里 */
        return draw_buf;
    }
    else {
#if LV_USE_FONT_COMPRESSED
        decompress(...);
        return draw_buf;
#else
        LV_LOG_WARN("Compressed fonts is used but LV_USE_FONT_COMPRESSED is not enabled in lv_conf.h");
        return NULL;      // <- 位图取不到
#endif
    }
```

而本板 `main/sdkconfig.defaults` 第 55 行写着 `CONFIG_LV_USE_FONT_COMPRESSED=n`，
`tools/gen_study_fonts.py` 生成的却是**压缩位图**（`bitmap_format = 1`）。
两边一撞，`lv_font_get_bitmap_fmt_txt()` 直接返回 NULL：
**背景照画、字一个都不画**，和症状完全吻合。

对照物就在仓库里：能正常显示的官方字体 `main/boards/electron-bot/otto_icon_font.c`
写的是 `.bitmap_format = 0`（plain）。

| | 之前 | 现在 |
|---|---|---|
| `bitmap_format` | 1（压缩） | 0（plain） |
| 生成参数 | lv_font_conv 默认（压缩） | `--no-compress --no-prefilter --no-kerning` |
| 字符集选择 | `--symbols` | `-r <码点区间>`（与上游 `compact_ranges` 同算法） |
| 位图能否取到 | 返回 NULL | 能取到 |

**为什么前几轮没查出来**：`lv_text_get_size()` 只读 glyph_dsc 里的 `box_w/box_h`，
压缩字体的描述符本身是好的，所以自检日志照样能量出正常字宽 ——
**字宽正常但位图取不到**，量宽度这条自检路线天生看不出这个问题。

**对齐上游**：上游 `78/xiaozhi-fonts/scripts/build.py` 调 lv_font_conv 用的是
`--no-compress --no-prefilter --force-fast-kern-format` + `-r <ranges>`。
本轮照抄，kerning 换成 `--no-kerning`：单词卡用不到字距微调，
而 186 个汉字两两配对能出上万对 kerning，纯属白占 flash。

**生成脚本加硬断言**：产物里 `bitmap_format` 不是 0 就直接失败退出，
不再依赖人肉记住命令参数。

**开机自检**：开机 2.5 秒后自动渲染一帧样例单词卡（`afternoon` / `a-f-t-e-r-n-o-o-n` / `下午`），
1.5 秒后收回。日志会打：

```
study selftest: latin40 'afternoon' 178x41
study selftest: latin26 289x25
study selftest: cjk48 '下午' 96x47
```

字体要是还是压缩的，LVGL 会打
`Compressed fonts is used but LV_USE_FONT_COMPRESSED is not enabled` —— 一眼就能看出来。

> **体积别看 .c 源文件大小**：字体 .c 是十六进制文本，`font_study_cjk_48.c` 看着 1 MB，
> 编译进 flash 的只有位图本体。四个字体实测合计约 **203 KB**（位图 196 KB + 描述符 7.4 KB）。
> 判断占用要看 CI 报的 app image 大小，不是 .c 文件大小。

### 10.6 LVGL v9 定时器的三个坑（2026-11-26）

开机自检要延时回调，连着踩了三次，都是 CI 编译报错才暴露的：

| 坑 | 现象 | 正确写法 |
|---|---|---|
| `lv_timer.h` 不在 `lvgl.h` 里 | `invalid use of incomplete type lv_timer_t` | 自己 `#include <src/misc/lv_timer.h>` |
| `lv_timer_t` 在公开头里只是前置声明（完整定义在 private 头） | 访问 `timer->user_data` 报 incomplete type | 用 captureless lambda + 文件级静态指针传上下文 |
| 捕获 `this` 的 lambda **不能**转成函数指针 | `cannot convert <lambda> to lv_timer_cb_t` | 只有 captureless lambda 能做这种转换（标准 [expr.prim.lambda]） |

另外 `lv_timer_create` 默认 `repeat_count = -1`，也就是**无限重复**，不是一次性。
要一次性就 `lv_timer_set_repeat_count(timer, 1)`：回调跑完 repeat_count 归零，
LVGL 会自动回收这个 timer（`auto_delete` 默认 true），不用自己 `lv_timer_delete`，
也就避开了「在回调里删自己」的悬垂风险。

> 本机没有 ESP-IDF 工具链，编译只能走 CI，每轮往返 6 分钟。
> 后来的改法先用 w64devkit g++ 写了个**最小样例**在本地验证语法
> （`tools/tmp/lv_timer_probe.cc`，模拟 LVGL 的公开接口），
> 确认能编过才推上去，省掉一轮 CI 往返。

### 10.7 上板实测（2026-11-26）

刷入 CI run #45737551030（sha `2573264`，app 镜像 3,618,240 B，余量 510 KB）后的开机日志：

```
I (3290) LcdDisplay: study selftest: latin40 'afternoon' 188x41
I (3290) LcdDisplay: study selftest: latin26 187x25
I (3290) LcdDisplay: study selftest: cjk48 '下午' 96x47
```

| 检查项 | 结果 |
|---|---|
| `Compressed fonts is used ...` 告警 | **0 条**（字体是 plain 位图，位图取得到） |
| 三个自带字体的字宽 | 188 / 187 / 96 px，全部非零 |
| E 级错误日志 | 0 条 |
| 唤醒词模型 | `wn9_nihaoxiaozhi_tts`（你好小智，未变） |
| assets 分区 | 1792 KB（混合包还在） |
| MCP 工具 | `self.english_learning.list_units [user]` 已注册 |
| 状态机 | `activating -> idle` |

开机 2.5 秒后会自动闪一帧样例单词卡，1.5 秒后收回。

> 项目级的完整排查方法（四层排查法 / 硬约束表 / 迭代法）见 `xiaozhi-esp32-doubao/README.md` §14.13；
> 结构化经验条目见 `xiaozhi-esp32-doubao/.learnings/`。
