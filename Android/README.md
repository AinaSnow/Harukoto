# 喵扑 Android 测试版

Android 7.0（API 24）以上的系统键盘。拼音、注音、候选排序和繁简输出共用喵扑的 C++17 核心及 mimo 词库。也可用罗马音输入日语假名和词表中的日文单字。键盘离线运作，没有网络权限。

界面沿用 iOS 的键位、试打页、候选展开、双猫／简洁皮肤、深浅色与十组色系。玻璃外观由 Android 的渐层、描边和透明度绘制。选词记忆和完整的 iOS 混合语言排序仍待移植。

## 安装

[直接下载已签名的 APK](https://github.com/Krysta1ll/Meowpuut/releases/download/v0.4.3-alpha.1/Meowpuut-0.4.3-alpha.1-android.apk)（[发布信息与校验档](https://github.com/Krysta1ll/Meowpuut/releases/tag/v0.4.3-alpha.1)），安装后开启「喵扑」。在「键盘设置」启用并选取喵扑，回到试打页即可输入。

0.4.3-alpha.1 使用新的发行签名。若已装过 Android 0.1.0，请先记下自己的键盘设置，再卸载旧版、安装新版；卸载会清除喵扑的本机设置，不能直接覆盖更新。第一次安装不受影响。

## 0.4.3-alpha.1

这版更新 mimo 词库与共用输入核心，补回单字读音，修正长句分段后剩余读音的处理。拼音输入到下一个音节时，可以保留前面的分段候选，不必从第一个字重选。也修正换字段、移动光标和长按时的待送文字处理。

这是公开测试版，尚未完成 Android 真机验收。

## 构建

准备 JDK 17、Gradle 8.9、Android SDK Platform 35、Build Tools 35.0.0、NDK 27.2.12479018、CMake 3.22.1 和 Python 3。将 `ANDROID_HOME` 指向 Android SDK。词库使用随源代码附带的固定转换数据，不需要安装 `uconv`。

在保存库根目录执行：

```sh
python3 scripts/compile-native-lexicon.py Android/Generated
gradle -p Android assembleDebug
```

APK 位于 `Android/build/outputs/apk/debug/`。安装后打开「喵扑」，从「键盘设置」进入系统设置，启用并选取喵扑键盘。

## 制作签名安装包

使用 JDK 17，设置 `ANDROID_HOME` 与 `GRADLE`（Gradle 执行档路径），再提供以下本机环境变数：

- `MEOWPUUT_ANDROID_KEYSTORE`：原有发行金钥文件。
- `MEOWPUUT_ANDROID_KEY_ALIAS`：金钥别名。
- `MEOWPUUT_ANDROID_STORE_PASS_FILE`：只含 keystore 密码的本机文件。
- `MEOWPUUT_ANDROID_KEY_PASS_FILE`：金钥密码文件；相同时可省略。

密码档与金钥应保存在保存库外，限制为帐号本人可读。执行 `Android/build-release.sh` 后，签名 APK 会放在 `.build/releases/<版本>/`。脚本会重建词库、编译 release、检查签名与 16 KB ZIP 对齐，最后列出 SHA-256。更新既有安装必须使用相同签名金钥。

## Windows 模拟器

在 Android SDK 中安装 Android Emulator、Platform Tools 和 `system-images;android-35;default;x86_64`，再从保存库根目录执行：

```powershell
pwsh -File Android/start-test.ps1
```

脚本建立或启动 `Meowpuut_API_35`，安装最新构建的 APK，并在该模拟器启用喵扑和开启试打页。若未设置 `ANDROID_HOME`，脚本使用本机的 `.build/android-tools/sdk`。

## 试打

「键盘设置 → 按键高度」可选标准、加高或特高。键盘依目前窗口可用高度收紧，空间足够时保留所选高度；横向与分割窗口不再被固定的 312 dp 最低高度撑大。导航列与屏幕缺口留在安全区外，展开候选时维持键盘原高度。按键列与候选条的纵向点击范围至少 44 dp；极矮窗口仍可能需要系统全屏幕编辑区。这些尺寸经几何测试，尚待 Android 真机确认。

- 新安装默认使用简体拼音。切换注音后，依序点 `ㄋ ㄧ ˇ ㄏ ㄠ ˇ`，选「你好」。注音的空格键在组字时可标一声或选词。
- 长按底列的 ABC／语言键，直接选择注音、日语或拼音。拼音输入 `nihao` 可按空格提交首选「你好」。前缀候选旁会显示对应拼音，选取后可继续转换剩余读音。
- 长按 ABC／语言键并从选单切换繁简，用拼音输入 `youhua`，选「优化」或「优化」。
- 日语输入 `sushi` 可选「寿司」、平假名或片假名。拼音模式识别词库中的日语片段后，中文可完整解码时优先显示中文候选，手动指定日语片段后则以指定写法为首选。含日语的候选会加上猫爪标记。收合时最多显示一项，其他日语候选会和低置信中文候选交错出现在展开清单。长按该候选可选其他写法，只替换日语片段，整句会留在组字区。
- 点候选条右侧箭头展开，展开后可翻页。长按空格滑动可移动光标，长按退格可连续删除。
- 密码、网址、电邮及数字字段使用直接输入。

## 核心冒烟测试

生成词库后，使用桌面端的 C++17 编译器执行：

```sh
c++ -std=c++17 -ICore Core/MeowpuutCore.cpp Android/smoke.cpp -o Android/Generated/smoke
Android/Generated/smoke
```


## 上屏失败重试

目前源代码会保留已选取、但应用程序暂时拒绝接收的文字，显示「文字尚未送出，点此重试」。点提示或再次按键会先重试该次选词；成功前不写入剩余读音，也不执行旧候选的点击。若重试前光标已移动，会暂停自动重试；只有点选「光标已移动，点此插入待送文字」才插入目前位置。文字只属于目前编辑字段；离开字段后不会送到另一个应用程序。此保护尚未在 Android 真机验证；应用关闭或持续拒绝写入仍可能让未送出的文字遗失。

可用 JDK 独立检查重试、顺序与字段隔离：

```sh
javac -d /tmp/meowpuut-delivery Android/src/main/java/app/meowpuut/android/PendingCommit.java Android/tests/PendingCommitTest.java
java -ea -cp /tmp/meowpuut-delivery app.meowpuut.android.PendingCommitTest
```


版面几何可在桌面 JDK 检查，涵盖 192 组窗口高度、安全区、拼音／注音列数与高度偏好：

```sh
javac -d /tmp/meowpuut-layout Android/src/main/java/app/meowpuut/android/KeyboardLayoutMetrics.java Android/tests/KeyboardLayoutMetricsTest.java
java -ea -cp /tmp/meowpuut-layout app.meowpuut.android.KeyboardLayoutMetricsTest
```


## 编辑位置与长按

在应用程序中清空、粘贴或移动光标时，键盘会结束旧组字，避免下次按键带回已删除的读音。上屏造成的延迟光标反馈会与已接受的写入逐一比对。长按删除与空格滑动在隐藏、换字段、重新布局或视图移除时取消；空格上的第二根手指也会取消该次手势。

```sh
javac -d /tmp/meowpuut-selection Android/src/main/java/app/meowpuut/android/EditorSelection.java Android/tests/EditorSelectionTest.java
java -ea -cp /tmp/meowpuut-selection app.meowpuut.android.EditorSelectionTest
```

光标协议已通过桌面模型测试，手势生命周期已编译检查；仍需在 Android 应用中验证真实事件顺序。
