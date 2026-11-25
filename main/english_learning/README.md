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
- **版式是「背景 + 自适应高度内容容器」**：卡片本身只负责铺满屏幕和背景色，
  四行文字挂在一个 `LV_SIZE_CONTENT` 高的容器里，容器整体居中。
  再长的行只会把整块撑高，不会把最后一行挤出屏幕——这正是之前「显示不全」的成因
  （旧版把四行直接塞进全屏 flex 容器，内容一超就被上下裁掉）。
- 行距 16px：四行文字共 144px + 3 个行距 = 192px，屏幕 240px，上下各留 24px。
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
| 卡片自带字体（4 个子集） | ~90 KB |
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
