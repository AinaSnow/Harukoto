# 参与晴词

修一个常用字的排序，和写一个新功能一样有价值。

本维护分支优先推进 Windows。开始前查看[开发状态](docs/DEVELOPMENT_STATUS.md)和[开发变更记录](docs/DEVELOPMENT_CHANGELOG.md)，在 [Gitea](https://git.hare.fun/NemophilaHare/input) 的 issue／PR 中写明认领范围。每次功能提交同步更新进度、使用说明和验证结果；区分源码完成、本机验证与发布安装。Windows 构建／测试命令见 [Windows/README.md](Windows/README.md)，脚本使用 PowerShell 7。

反馈输入问题时请包含：

- Windows／Android 版本、晴词版本与发生问题的应用。
- 注音或拼音模式、繁体或简体输出。
- 原始按键、预期文字、实际候选及选取后结果。
- 如有截图，先遮掉私人内容。

词库新增与修订请放在 Resources/Lexicon/chinese.tsv；日语在 japanese.tsv，表情关联在 associations.json。请提供读音与使用例，勿复制没有明确授权的商业词库。修改后运行 Windows/build-ime-msvc.ps1 和相关 Python 测试，为排序问题留下能重现的测试。

Core 是 C++17 中文核心；Windows 包含 TSF 输入服务、WPF 候选窗与可移植会话层；Android 保留平台键盘接入。macOS 和 iOS 源码已移除。请勿更改升级兼容标识，参阅 docs/BRAND.md。

提交请说清楚「什么输入原先失败，现在变成什么」。一个 PR 解决一组相关问题即可。贡献的原创内容依项目 MIT 授权提供；第三方数据仍须附上来源和兼容授权。
