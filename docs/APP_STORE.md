# App Store 准备记录

状态：开发中，尚未提交 App Store 或完成审核。iOS 真机开发签名成功不等于已具备发行签名或已通过审核。

## iOS

- 键盘扩充是系统支持的产品形式。Apple 的 [App Review Guidelines 4.4–4.4.1](https://developer.apple.com/app-store/review/guidelines/) 要求键盘在不授予完整取用及没有网络时仍可使用、可切换至下一个键盘，并限制键盘活动数据的用途。现有扩充设置 `RequestsOpenAccess=false`；应在真机上逐项验证，而不只检查设置值。
- 宿主与扩充使用同一 App Group 传递用户明确修改的设置；个人选词记录留在键盘自己的沙盒。须验证不开完整取用时的共享设置读取、切换回键盘后的更新与一次性清除请求，并确认更新与重装对用户数据的影响。
- [App Store Review 5.1.1](https://developer.apple.com/app-store/review/guidelines/) 要求 App 内可找到隐私政策，App Store Connect 亦须提供可用的隐私政策 URL 与准确数据使用声明。目前尚未在项目找到应用内政策链接。上架前应核对实际执行路径、用户可关闭与删除的记忆、第三方元件及系统诊断，再撰写并提供政策；不因数据仅保存在设备上而省略政策。
- 扩充已有 `PrivacyInfo.xcprivacy`；目前检查到的 iOS 开发包只有扩充内有该文件。需以 Release archive 与 Apple 的 [required-reason API 说明](https://developer.apple.com/documentation/bundleresources/app-privacy-configuration/nsprivacyaccessedapitypes/nsprivacyaccessedapitype) 逐项核对实际用途与文件放置；不要因加了宣告就假设审核通过。应分别检查宿主及扩充有无各自需要宣告的 API。
- 已生成的宿主 Info.plist 包含 `UILaunchScreen`；仍需在最终发行 archive 验证，详见 [TN3208](https://developer.apple.com/documentation/technotes/tn3208-preparing-your-apps-launch-screen-to-meet-app-store-requirements)。另须验证 iPhone／iPad 不同尺寸、横屏、VoiceOver、降低透明度、系统安全输入栏及多种 App。
- 个人开发签名可用于设备开发，但 [TestFlight 与 App Store 发行](https://developer.apple.com/programs/)需要合适的 Apple Developer Program 发行帐户与 App Store Connect 设置；目前不假设帐户已具备这些条件。

目前的已签名真机开发包可用以下命令检查键盘扩充类型、完整取用设置、隐私清单的三组用途及宿主／扩充的 App Group 是否一致：

```sh
python3 scripts/check-ios-privacy.py .build/iOS-device/Build/Products/Debug-iphoneos/Meowpuut.app
```

检查不会安装 App 或读取用户输入。加 `--release` 会拒绝仍启用 `get-task-allow` 的调试签名；本机开发包因此会按预期失败。这不是 App Store 自动验收，也不能证明没有其他需要宣告的 API、隐私政策已齐备、App Group 在真机可读，或最终 archive 符合上架要求。`CA92.1` 对应键盘自身的 UserDefaults；`35F9.1` 对应双击 Shift 的时间间隔。`C617.1` 的文件中继数据使用仍须连同最终二进制及依赖一起核对。

## macOS：商店整合需先做隔离验证

现有的独立发行版通过 PKG 把输入法安装至 `/Library/Input Methods/`，并可能需要管理员权限；这条安装流程**不能直接视为 Mac App Store 安装流程**。Apple 的 [App Review 2.4.5](https://developer.apple.com/app-store/review/guidelines/) 要求 Mac App Store 应用沙盒化、使用自含的单一 App bundle，并限制向共享位置安装与提权。[Apple 的沙盒说明](https://developer.apple.com/documentation/xcode/configuring-the-macos-app-sandbox)也将 App Sandbox 列为 Mac App Store 要求。现有 `scripts/build.sh` 使用本地 ad hoc 签名，且未建立商店发行用的沙盒 target。现有 macOS 构建包没有隐私清单，而程序使用 `UserDefaults`；商店目标应按实际用途加入并验证 required-reason 宣告。

本机 macOS 27 SDK 的 `Carbon.framework/Frameworks/HIToolbox.framework/Headers/TextInputSources.h` 对 `TISRegisterInputSource` 的注释指出，这个 API 注册 input method bundle 的位置为 `/Library/Input Methods/` 或 `~/Library/Input Methods/`。现有 `Sources/SystemInput.swift` 也只接受这两个目录。因此，单纯将现有 App 放到 `/Applications` 并沿用这条注册路径，没有得到目前 SDK 注释的支持。

这些事实**尚不能证明** InputMethodKit 类型的应用一律无法上架，也不能证明尚有可用的商店整合路径。Apple 的 [InputMethodKit 文档](https://developer.apple.com/documentation/inputmethodkit)说明文字服务框架，但未在此处提供本项目的 App Store 安装方案。

建议先在独立 macOS 测试帐户或虚拟机，以新的测试 Bundle ID 建立沙盒签名原型：检查在合法位置启动、注册、列举、注销再登录后是否仍可用，以及跨 App 组字、候选窗、更新和卸载行为。不得覆盖现有使用中的输入法、沿用其 Bundle ID，或把函数返回成功当作完成启用。对照结果向 Apple Developer Technical Support／App Review 询问受支持的架构，再决定商店版如何封装；独立发行版可以继续作为另一条产品路径。
