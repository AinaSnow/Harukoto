<p align="center">
  <img src="assets/brand/MeowpuutMark.png" width="160" alt="米米莫和莫莫">
</p>

<h1 align="center">喵扑 Meowpuut</h1>
<p align="center">mimimo。</p>
<p align="center">
  <a href="https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/Meowpuut-0.4.3-alpha.1-macos-arm64.dmg">下载 macOS 测试版</a> ·
  <a href="https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/Meowpuut-0.4.3-alpha.1-windows-x64.zip">Windows</a> ·
  <a href="https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/Meowpuut-0.4.3-alpha.1-android.apk">Android</a> ·
  <a href="docs/INSTALL.md">安装说明</a> ·
  <a href="https://github.com/Krysta1ll/Meowpuut/issues">反馈问题</a> ·
  <a href="LICENSE">MIT</a>
</p>

喵扑是一款从日常中文输入出发的开源输入法。拼音和注音各有自己的按键规则，繁简共用一份词库；偶尔想打日文单字，可以直接输入罗马音，不必为几个字来回切换输入法。

目前是公开测试版。遇到不顺手的地方，欢迎提供按键顺序和预期文字。

本维护分支以 Windows 为优先，代码托管于 [Gitea](https://git.hare.fun/NemophilaHare/input)。参与开发请先读[开发状态与接手入口](docs/DEVELOPMENT_STATUS.md)、[开发变更记录](docs/DEVELOPMENT_CHANGELOG.md)和[日语分阶段路线图](docs/JAPANESE_INPUT_ROADMAP.md)。下方 GitHub 下载链接为上游历史发布版本，不包含本分支新增修复。

## 把小猫带回家

| 平台 | 下载 | 版本 |
| --- | --- | --- |
| macOS · Apple silicon | [纸箱安装盘](https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/Meowpuut-0.4.3-alpha.1-macos-arm64.dmg) · [安装包](https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/Meowpuut-0.4.3-alpha.1-macos-arm64.pkg) · [SHA-256](https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/SHA256SUMS.txt) | 0.4.3 Alpha 1，macOS 27+ |
| Windows · x64 | [下载 ZIP](https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/Meowpuut-0.4.3-alpha.1-windows-x64.zip) · [SHA-256](https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/SHA256SUMS.txt) | 0.4.3 Alpha 1，Windows 10／11 |
| Android | [下载 APK](https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/Meowpuut-0.4.3-alpha.1-android.apk) · [SHA-256](https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/SHA256SUMS.txt) | 0.4.3 Alpha 1，Android 7+ |
| iOS／iPadOS | [源代码与构建说明](iOS/README.md) | 尚无公开 TestFlight 或 IPA |

Windows 与 Android 仍是 Alpha，功能与 macOS 不完全相同；Windows 目前以拼音为主，新版焦点及上屏保护未经 Windows 实测，未提交的文字仍可能遗失。Windows 与 Android 尚未完成真机验收。macOS 安装包尚未公证，Windows 软件包未签名；安装前请读各平台说明并核对 SHA-256。Intel Mac 与较旧的 macOS 尚未验证。

**Android 旧版更新：** 0.4.3 改用新的发行签名，无法直接覆盖 0.1.0。请先记下设置，再卸载旧版、安装新版；卸载会清除本机设置。全新安装不受影响。

## 0.4.3：句子还没打完，也能先选前半段

`woxihuannid` 打到最后的 `d`，前面的「我喜歡你」仍可选；选完只留下 `d`，接着打下一个字。注音也保留前段与尚未完成的音节。拼音后面接 `kirei` 或 `udon` 时，中文前段与日语候选一起参与分段，不必先把中文送出去。

这版重新核对 mimo 的单字读音、繁简字形和来源词频。简体拼音用简体来源的词频排序，繁体输出保留自己的权重；已选前段的短期上下文也会参与后段排序。词库与三个公开客户端在同一版收齐。具体修复与限制见[版本更新](docs/RELEASE_NOTES.md)。

## 今天想打什么？

以下画面为 macOS 0.4.2 的候选界面预览。

打 `sushi` 的时候，「蘇軾」和「寿司」都可以在候选里找到。猫爪提醒你那是日语，想吃寿司还是想聊苏轼，交给你选。

![输入 sushi 时的候选窗界面预览](assets/screenshots/sushi-current.png)

想说「我好き你」？在拼音模式接着打 `wosukini` 就好。也可以先选长句的前半段：`woxihuannid` 选「我喜歡你」之后，剩下的 `d` 会留在组字中。

![中日混打「我好き你」的候选窗界面预览](assets/screenshots/mixed-current.png)

习惯注音的话，`su3cl3` 就是「你好」。拼音和注音各有一套按键，不用猜现在该按哪种声调。

![注音输入「你好」的候选窗界面预览](assets/screenshots/zhuyin-current.png)

## 米米莫、莫莫，和候选窗

左边是米米莫，右边是莫莫。猫耳构成玻璃的边界，尾巴沿著候选列收起，两张脸留在左右两端。角色和窗口一起变长、收合；文字仍是中间最重要的部分。

「双猫玻璃」之外，也有「简洁玻璃」。两种皮肤共用深色、浅色与跟随系统的选择，内置玫瑰奶茶、海盐蓝、鼠尾草绿、雾紫、琥珀蜜、石墨六组配色，玻璃强度亦可调整。两者都依实际字宽安排候选，短词可以并排，长句展开阅读。缩放与展开有轻柔过渡，也会遵守系统的减少动态效果设置。

## 选字，先照顾中文

- **拼音与注音分开。** 拼音用数字直接选词；注音保留数字声调，用 Shift＋数字选词，不让两套规则互相抢按键。
- **日语留在旁边。** 拼音输入时，完整罗马音可产生假名或日语词汇候选；有日语候选时，收合列保留一个，以倾斜的小猫爪标示。展开后可按候选排序继续浏览。
- **英文留得住。** 拼音组字时，空格或 Enter 选取目前高亮的候选，数字可直接选词；Shift＋Enter 输出原始字母。单个字母仍可选英文，但中文优先。已识别的英文单字也可以成为首选。
- **越用越熟悉你的用词。** 记住确认过的选词、使用次数，以及短时间相邻选词的关系。反复使用的短语可进入本机个人词库，无须逐个收藏。
- **表情跟著词义走。** 「开心」「谢谢」「晚安」等词可以带出 emoji 和颜文字，作为文字之外的选择。

日语目前是罗马音转假名加小型单字词表，不是完整的日语整句汉字转换器。能转成假名，也不代表那串字一定是有意义的日语。

## 先试几个字（macOS）

| 模式 | 输入 | 可以选到 |
| --- | --- | --- |
| 注音 | `su3cl3` | 你好 |
| 注音 | `aul `（末尾空格） | 喵 |
| 拼音 | `nihao` | 你好 |
| 拼音 | `miao` | 喵 |
| 拼音 | `youhua` | 優化／优化 |
| 拼音 | `d` | 的、到、都等中文，亦保留原字母 |
| 拼音 | `sushi` | 中文同音词，以及日语备选 |
| 拼音 | `emoji` | 英文原文 |

| 操作 | 快捷键 |
| --- | --- |
| 注音／拼音／日语模式 | Control＋Option＋1／2／3 |
| 拼音选词 | 1–7 |
| 注音选词 | Shift＋1–7 |
| 确认候选 | Enter；拼音也可按空格 |
| 保留原始字母 | Shift＋Enter |
| 展开／收合或取消 | ↓／Esc |
| 翻页 | Option＋←／→，或 Page Up／Page Down |
| 拼音声调（可选） | Shift＋空格／6／3／4／7 → 一／二／三／四／轻声 |
| 中英切换 | 拼音单按 Shift；注音保留 Shift＋Space |
| 标点选单 | 反引号 |
| 中文模式输入英文斜线 `/` | Option＋斜线 |
| 本地短语模组 | Option＋Space |

拼音直接按标点键输入中文标点；Shift＋逗号、句号、斜线可输入英文标点。注音的声韵键保留原功能。繁简与皮肤可在输入法选单切换。

## mimo 1.0：一份繁简词库，两套输入规则

中文解码、断词与排序由喵扑的 C++17 核心完成，没有 Rime 执行时依赖。中文数据采用 McBopomofo 与 Pinyin Pro 的 MIT 词表，再套用喵扑的读音、字形与排序修订；日语词条也包含附带 BSD 授权的 UniDic 精选数据。

合并词库名为 **mimo 1.0**。读音和简体相同、繁体不同的字词各自保留，例如「志／誌」和「一周／一週」；简体输出会将相同候选去重。两份来源的频率尺度以共同词条校准，简体来源的表达优先，本地修订最后生效。每笔数据保有繁简字形，切换输出不需要加载另一份词库。

约 40 万组词条读音保存在完整索引，常用部分供核心断词；这不是 40 万个互不重复的词，也不是即时联网更新。固定版本、校验值与授权见 [THIRD_PARTY.md](THIRD_PARTY.md)。合并策略与已知限制见[引擎说明](docs/ENGINE.md)。

## 数据留在本机

macOS 和 iOS 的输入、排序与个人词库不需要网络。开始一段中文组字时，使用目前输入字段光标前最多四个连续中文字辅助排序，遇到标点、空白或其他文字便停止；这段前文只留在本次组字，不写入个人词库，也不扫描聊天记录或整份文件。个人词库学习的是你确认过的候选，可在设置中停用或清除；停用记忆后仍可使用当前语境排序。Windows、Android Alpha 的功能请参考各自的版本说明。

macOS 数据位于 `~/Library/Application Support/Meowpuut/`。iOS 键盘使用自己的沙盒；目前没有跨设备同步。

## 从源代码构建

macOS 需要 macOS 27 SDK 的完整 Xcode、Python 3 和命令行工具。构建脚本默认使用 `/Applications/Xcode.app`；其他位置可设置 `MEOWPUUT_XCODE_DIR`。

```sh
git clone https://github.com/Krysta1ll/Meowpuut.git
cd Meowpuut
bash scripts/build.sh
.build/Meowpuut.app/Contents/MacOS/Meowpuut --smoke
bash scripts/package-macos.sh
```

词库快照随源代码保存，正常构建不用下载外部引擎或词库。测试使用独立偏好设置，不修改日常选词记忆。安装包输出至 `.build/releases/`。

C++ 核心可以单独构建：

```sh
cmake -S Core -B .build/core
cmake --build .build/core
```

Windows 的构建、安装与会话层测试见 [Windows/README.md](Windows/README.md)。iOS 另需 XcodeGen，见 [iOS/README.md](iOS/README.md)。

输入体验的修正、测试方式与尚未完成的部分，见[体验检查记录](docs/INPUT_EXPERIENCE_AUDIT.md)。

## 一起把选词做好

最有用的反馈往往只有几行：使用哪个模式、按了哪些键、想打什么、目前出现什么。请删掉截图中的私人内容，再附上系统与应用版本。

词库修订放在 `Resources/Lexicon/`，不要直接修改上游快照。欢迎修读音、调排序、补日常词汇和改善跨应用兼容性。提交方式见 [CONTRIBUTING.md](CONTRIBUTING.md)。

原创代码与随附项目素材采 [MIT](LICENSE)；外部词库保留各自授权。喵扑仍在测试阶段，长句断词、繁简歧义字形、日语单字覆盖和不同应用中的候选定位，都还有需要打磨的地方。
