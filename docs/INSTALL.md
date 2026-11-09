## 纸箱安装盘（0.4.1 起）

打开 `.dmg`，把纸箱里的 **Meowpuut** 拖到右侧「输入法」。这个捷径指向 `/Library/Input Methods`，不是「应用程序」。Finder 可能要求管理员验证；**更新时请使用「安裝或更新.pkg」**，Finder 不能覆盖正在使用的输入法。

复制完成后保存工作、注销再登录，再依下方步骤加入输入来源。更新不会移除本机选词记忆。安装盘内也有「安裝或更新.pkg」，与单独提供的 pkg 相同；两种方式选一种即可。若系统输入法文件夹尚不存在，请使用 pkg，它会建立文件夹。

安装盘与安装包尚未公证；纸箱只是 Finder 的安装界面，不会绕过 macOS 的验证。不要直接从 DMG 启动输入法，也不要停用 Gatekeeper。

# 安装喵扑 Meowpuut

目前测试包：0.4.3 Alpha 1，支持 macOS 27 以上的 Apple silicon Mac。

## 安装

1. 从 GitHub Release 下载 `Meowpuut-0.4.3-alpha.1-macos-arm64.pkg`。
2. 开启安装包，确认安装到系统磁碟。需要管理员验证；程序会放在 `/Library/Input Methods/Meowpuut.app`。
3. 保存工作后注销，再登录。更新旧版时也请这样做，避免旧的输入服务仍留在内存。
4. 系统设置 → 键盘 → 文字输入 → 编辑 → 加入 Meowpuut。名称也可能显示为「喵撲 Meowpuut」，可在繁体中文分类查找。
5. 切到 Meowpuut，按 Control＋Option＋2 使用拼音，输入 `nihao` 后按 1。

输入法没有出现在清单时，先确认安装位置与登录状态；不要同时保留用户目录和系统目录两份输入法。默认是注音，可按 Control＋Option＋1 切回。

## 签名状态

本测试包未使用 Developer ID Installer 签名，App 只有本机 ad-hoc 签名，尚未经 Apple 公证。因此从浏览器下载后，macOS 可能阻止开启。

请先核对来源和 Release 中的 SHA-256。若决定试用，可依 [Apple 的开启未验证 App 说明](https://support.apple.com/en-hk/102445)，在系统设置的「隐私权与安全性」处理该次开启。不要停用整台电脑的 Gatekeeper 或 SIP。若系统仍拒绝，请反馈完整提示；也可自行从源代码构建。

```sh
shasum -a 256 Meowpuut-0.4.3-alpha.1-macos-arm64.pkg
```

目前验证的是本机安装与输入；尚未完成另一台干净 Mac 的下载、公证与首次安装流程验证。

## 更新与移除

更新可直接安装新包，个人词库不会被覆盖；完成后注销再登录。

移除时先切换到 ABC，从键盘输入来源清单移除 Meowpuut，再通过 Finder 的「前往文件夹」开启 `/Library/Input Methods`，将 `Meowpuut.app` 移到垃圾桶并注销。个人数据会保留在 `~/Library/Application Support/Meowpuut/`；确定不再需要时才另行移除。
