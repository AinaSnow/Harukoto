# 喵扑 Meowpuut 0.4.3-alpha.1 · Windows x64

## 直接下载安装

本分支开发进度、验证记录和接手任务见[开发状态](../docs/DEVELOPMENT_STATUS.md)。下方 GitHub 下载链接对应上游历史发布包，不包含本分支的新修复。当前开发版从源码构建，安装入口要求 PowerShell 7（`pwsh.exe`）；提权安装沿用同一个 PS7，WPF 编译仍使用 Windows 内置 .NET Framework 编译器。

从 [GitHub Releases](https://github.com/Krysta1ll/Meowpuut/releases/tag/v0.4.3-alpha.1) 下载 `Meowpuut-0.4.3-alpha.1-windows-x64.zip`，完整解压后双击 `Install.cmd`。Windows 会要求管理员权限，以注册系统输入法。安装后按 Win + 空格键，从繁体中文切换到 Meowpuut Pinyin。

此 Alpha 发布包未签名，Windows 应用程序控制政策可能阻止安装或启动玻璃渲染器。本版以拼音为主，注音尚未接入。安装、焦点切换及上屏保护均未在 Windows 实测；应用关闭、宿主拒绝输入或服务停止时，未提交的文字仍可能遗失。软件包内的 `BUILD-INFO.md` 列出构建来源与验证范围。

## 双猫玻璃候选窗预览

在仓库根目录用 PowerShell 构建并执行：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\Windows\build-preview.ps1
& .\.build\windows\MeowpuutGlassPreview.exe
```

预览使用 Windows 内置的 .NET Framework 与 WPF。双猫轮廓以 macOS 深色候选窗为基准，并沿用 iOS 的双猫造型。玻璃会即时采样候选窗附近的桌面，在内部柔化背景，沿猫耳、弧角和尾部轮廓的法线偏移背景，折射由边缘向内逐渐消退。轮廓反光和内侧细光带叠在暖色玻璃之上。采样画面仅在内存内处理。Windows 10 2004 起支持将预览排除在屏幕捕获之外，以避免重复采样自身，因此部分截图或录屏工具可能看不到预览窗。无法采样时回退为半透明玻璃，高对比模式下候选区使用系统色。

可点选候选，或按数字键 1–5、7 与左右方向键切换高亮。按 Esc 关闭，也可拖动玻璃区移动窗口。预览使用固定候选词。

## Windows 系统输入法（x64 Alpha）

### 候选字号与排版

候选按实际词宽排列，空间不足自动换行；长词和读音可在词块内部换行，不再强行压进一行。双猫区域显示组字状态，下方显示候选。默认正文为 18 个 WPF 逻辑像素（接近原版缩放后的中文字号），窗口宽度为 672；高 DPI 下由系统缩放。

在仓库根目录可调整字号或窗口宽度，无需管理员权限，下次按键更新候选时生效：

```powershell
.\Windows\set-candidate-appearance.ps1 -FontSize 22
.\Windows\set-candidate-appearance.ps1 -Width 800
.\Windows\set-candidate-appearance.ps1 -Reset
```

字号范围 14–32，宽度范围 480–1000；设置保存在当前用户的 `HKCU/Software/Meowpuut/CandidateWindow`。

图形设置入口：构建后执行 `.\.build\windows\MeowpuutGlassPreview.exe --settings`，或在新安装包中双击 `Settings.cmd`。使用新安装脚本安装后，开始菜单会提供 `Meowpuut Settings`。点击保存后生效；打开设置本身不会改变当前字号。安装包根目录也包含 `set-candidate-appearance.ps1`。

### 测试与打包

在 PowerShell 7 的仓库根目录执行：

```powershell
.\Windows\build-ime-msvc.ps1
python -m unittest discover -s Windows/tests -p test_japanese_dictionary.py
.\Windows\tests\InstallSupportTests.ps1
.\Windows\test-preview.ps1
.\Windows\install-ime.ps1 -VerifyOnly
.\Windows\package-ime.ps1 -MSVC -Version 0.4.3-japanese.1
```

`test-preview.ps1` 检查不同字号和宽度下的候选布局，并在 `.build/windows` 输出候选窗及设置页图片。打包脚本保留许可证，附带词表导入工具、设置入口、源提交和工作区状态；ZIP 和 SHA-256 位于 `.build/windows/dist`。LLVM-MinGW 构建应使用 `package-ime.ps1 -Compiler <编译器路径>` 打包。`-VerifyOnly` 只验证文件存在，不修改输入法注册。

### 更新失败与安装日志

普通安装和管理员注册分别在运行账户的 `%LOCALAPPDATA%/Meowpuut/Logs` 写入日志，文件名包含时间、阶段和随机标识。若 UAC 使用另一个管理员账户，注册日志位于该管理员账户下。`-VerifyOnly` 不创建日志，也不注册输入法。

安装会先复制到按文件内容区分的版本目录，并逐个校验 SHA-256，然后注册新 DLL。机器级注册失败或注册路径不符合预期时，会尝试重新注册安装前的旧 DLL；首次安装失败则尝试移除不完整注册。恢复失败会明确报错并保留版本文件以便诊断。原注册路径对应的 DLL 已丢失时，升级在注册前停止。

已有版本文件内容相同则复用，不覆盖被应用占用的文件；内容不同则停止。旧版本目录不会自动删除。注册阶段使用文件锁避免并发修改；这不代表完整安装过程具有事务隔离。用户语言配置、快捷方式或渲染器启动发生在机器注册之后，这些后续阶段失败时不会自动回滚注册，可保留日志并重试。注册恢复尚不等于通用卸载或完整系统状态还原。

`InstallSupportTests.ps1` 的 14 项检查使用模拟注册调用和临时文件，覆盖失败恢复、锁定文件与真实入口日志，不修改系统输入法。完整 UAC、TSF 和应用兼容性验收按[Windows 验收记录](../docs/WINDOWS_VALIDATION.md)执行。

开发版已加入第一阶段日语辅助输入：拼音出日文汉字、英语出片假名、候选上方显示假名读音及本地自定义词表导入。当前包含 41 组基础触发词，详见[日语辅助输入与分阶段计划](../docs/JAPANESE_INPUT_ROADMAP.md)。动词变形、敬语、完整日语整句和生词本尚未实现。

已安装 Visual Studio C++ 工具、Windows SDK、CMake 和 Python 3 时，也可使用 MSVC 构建并运行测试：

```powershell
.\Windows\build-ime-msvc.ps1
.\Windows\install-ime.ps1
```

CMake 不在 PATH 中时，可用 `-CMake C:/path/to/cmake.exe` 指定。MSVC 构建会生成 TSF DLL、词库和渲染器；测试保留断言，即使使用 Release 配置也会实际检查结果。

实际输入法候选窗使用半透明玻璃，不排除屏幕捕获，因此可在 Parsec、GameViewer 等远程或串流画面中显示。实时桌面采样和折射仅用于上方的独立预览。旧版渲染器会将候选窗排除在屏幕捕获之外，可能出现“数字键能选词，但远程画面看不到候选框”的情况；需要更新玻璃渲染器并重新启动。

在 Windows 10／11 x64 使用 Python 3 和 LLVM-MinGW 的 x64 编译器构建。默认构建产物未签名。安装时 Windows 会要求以系统管理员权限将输入服务安装至 Program Files 并注册 TSF。安装程序会在当前登录会话中启用输入法。

```powershell
.\Windows\build-ime.ps1 -Compiler C:\path\to\x86_64-w64-mingw32-clang++.exe
.\Windows\install-ime.ps1
```

在应用程序的文字区按 Win + 空格键，从繁体中文切换到 **Meowpuut Pinyin**。数字 1–7 直接选词；方向键和 Tab 切换高亮，Enter 确认，空格键输出原始拼音加空格。Page Up／Page Down 翻页，Esc 取消，Backspace 修改。

拼音默认输出简体；按 **Ctrl + 左 Alt + Shift + Q** 切换繁简，候选窗会显示「简」或「繁」。切换时保留正在输入的拼音。此 Alpha 的选择只在当前输入服务中有效，不会同步到其他已开启的应用程序，重新启动后恢复简体。右 Alt（AltGr）不会触发快捷键。

美式键盘布局下，直接输入逗号、句号、分号、斜线、方括号与反斜线会输出 `，。；？「」、`；组字中会先上屏候选。按住 Shift 时交给应用程序输入英文标点。

可混打词库收录的日语罗马字，例如 `wosukini` 的候选包含「我好き你」。同页有日语候选时，收合版保留其中一条，依原候选顺序显示。按 F4 展开查看同页其他候选。低置信中文和其余日语候选交替排列，猫爪只标记含日语的候选。选取带猫爪的整句会直接上屏；选取局部中文词会保留后续组字。
此版本使用喵扑的 C++ 中文引擎与实际词库，以 TSF 将文字送入应用程序。系统输入法会启动独立的双猫玻璃渲染器，显示即时拼音与候选词。支持 TSF 文字坐标的应用程序会把候选窗贴近插入点；其余应用程序沿用 Windows 提供的光标位置。渲染器会随用户登录启动。若 Windows 应用程序控制政策阻止执行渲染器，输入服务会使用原生候选窗。两种候选窗都可用键盘选词。已开启的应用程序需重新启动才能加载新版输入服务。此构建适用于 x64 桌面应用程序。

### 安装已签名的版本

先构建输入服务及玻璃渲染器，再以受信任的 Authenticode 发行者证书签名 `.build/windows/ime/MeowpuutTip.dll` 和 `.build/windows/MeowpuutGlassPreview.exe`。执行 `.\Windows\install-ime.ps1 -Signed`，安装程序会验证两个签名并直接安装已签名的文件。

## 可移植会话层

本节介绍跨平台 C++ 核心的会话接口；它本身不是可安装或注册的 Windows 输入法。每个输入上下文应独立持有一个 `meowpuut::Session`，并只在所属线程使用。字典以 `std::filesystem::path` 读取；Windows 调用端可以传 UTF-16 路径。快照复制 UTF-8 组字、候选与上屏字符串；`takeSnapshot()` 读取后清除待提交文字，避免重复上屏。

`Keyboard.h` 提供键盘事件转换：拼音数字选词、Shift 加大千声调键输入拼音声调，拼音组字时空格保留原文并加入空格；注音的数字留给声调而 Shift 加数字选词。翻页与编辑键在组字时交给会话。调用端需传入当次事件的虚拟键、键盘配置产生的 ASCII 字符及修饰键状态，忽略 key-up 和死键，并自行处理单按 Shift 切换中英。Ctrl、Alt、Windows 组合键保留给系统或应用程序。这个转换层不会安装键盘拦截器或注册 TSF。

拼音会话可在输入前调用 `registerJapaneseWords(path)`，从生成的 mimo 词库目录加载 `native-japanese.tsv`；它与 macOS、iOS 使用的日语读音来自同一份合并数据。未调用时保持纯中文核心行为。混合中文与日语的完整句和前半段通过跨平台 C 核心产生，选取前半段后会保留未转换的输入。快照的 `japaneseCandidates` 与候选一一对应：每页最多一个中日混合候选，排在该页中文之后；额外候选放下一页。第一页可能有七个中文及一个额外的中日候选，拼音可用数字 1–8 选取。后续 Windows UI 需按快照实际数量显示并标示这些候选。

生成词库后，可执行 `python3 scripts/export-windows-lexicon.py <生成词库目录> <新的输出目录>` 导出 Windows 使用的三份数据表与 `Licenses` 声明目录。日后发布 Windows 二进制时须一并附上该目录；此会话层不会自行产生 Windows 安装包。

同一次组字中，选择中文前段后，会话会暂存短期上下文以协助后段排序；整句完成、取消、删除至空或清除时会移除。选中含日语的混合前段会作为语境边界，清除之前的中文上下文，并保留尚未转换的读音。上下文只存在于各自的会话，不写入个人词库。

单次组字达到核心容量时，下一个有效按键会先将原始读音加入待提交文字，再开始新的组字；注音保留显示的注音符号，拼音保留原文。待提交内容必须由调用端通过 `takeSnapshot()` 依序上屏；连续按键之间未取快照也不会覆盖前一次结果。

TSF 的 [`OnTestKeyDown`](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfkeyeventsink-ontestkeydown) 是按键意向查询；`canHandleKey` 不改变组字、候选或待提交文字，可用于此查询。实际处理另调用 `handleKey`。两者之间应使用同一输入上下文的会话与模式；模式必须与会话加载的词库一致。核心按键接受规则有变动时，要同步更新查询并执行回归：测试逐一比对拼音、注音在空输入和组字状态下所有 ASCII 可打印键的查询结果与核心行为。这组转换层是 TSF 接入准备；它尚未连接上方的 TSF 原型。

在已安装 CMake、Visual Studio C++ 开发工具和 Windows SDK 的机器上，使用已生成的 mimo 字典目录：

```powershell
cmake -S Windows -B build/windows -A x64 -DMEOWPUUT_LEXICON_DIR="C:/path/to/lexicons"
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure
```

目录须包含 `native-pinyin.tsv`、`native-zhuyin.tsv` 和 `native-japanese.tsv`；它们由项目现有词库编译流程产生。`MEOWPUUT_LEXICON_DIR` 未指定时不注册集成测试。可用上方的 Windows 构建脚本产生词库，再指向其输出目录。

独立 TSF 原型见上方；它与此会话层尚未整合。会话层尚未接入 macOS 的 Swift 排序、个人词库、完整日语候选、中文标点和全部快捷键逻辑。可参阅 Microsoft 的 [TSF 文档](https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework)、[composition](https://learn.microsoft.com/en-us/windows/win32/tsf/compositions) 和 [文字服务注册](https://learn.microsoft.com/en-us/windows/win32/tsf/text-service-registration)。

会话层已在 macOS Clang 和 Windows MSVC x64 上编译并通过集成测试；完整 TSF 宿主生命周期仍需跨应用验证。

目前源代码包含焦点切换与待上屏文字的保护实验；仅通过跨平台队列测试与 Windows x64 交叉编译，尚未在 Windows 主机验证 TSF 生命周期、异步回调或应用程序兼容性。应用关闭、服务停用、宿主持续拒绝写入时，未完成文字仍可能遗失。

开发版已接入 TSF 字段移除通知，关闭字段时释放保存的会话；服务停用或字段关闭后，晚到的提交回调不再写入文字。正常焦点切换仍保留原字段的组字。这部分已通过队列测试、AddressSanitizer／UndefinedBehaviorSanitizer 及交叉编译，已包含在 0.4.3 Alpha 1 发行包中，尚未真机验证。


目前源代码也会在焦点变更时暂停该字段的待上屏队列；已调度但尚未写入的异步编辑会先保留文字，回到原字段并收到下一次可处理按键后再重试。选词已清空读音、但前段还在等待上屏时，紧接的数字与空格也排入同一队列，避免跑到前段之前。此保护不涵盖所有直接交给宿主的命令，例如 Enter、删除和移动光标；完整宿主编辑生命周期仍待 Windows 实测。
