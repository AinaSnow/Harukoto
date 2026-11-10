# 实验引擎部署身份校验

WIN-03b，2026-11-11。`Windows/EngineDeployment.h` 为可信发现补充部署身份基础：固定预期程序文件、核对调用方保留的真实进程映像。已接入 EngineProcess 的直接宿主及独立所有者启动链，尚未实现普通应用发现、竞争失败后的附着、正式常驻或 TSF 接入。Windows 0.4.4-alpha.22、FF14 0.3.0 安装与发行保持。

## 可信配置

```cpp
EngineProcess::Config config{hostExecutable, base, themes, personal};
config.dynamicEndpoints = true;
config.ownerExecutable = ownerExecutable;
config.ownerInstance = L"harukoto-main";
config.hostSha256 = trustedManifest.hostSha256;
config.ownerSha256 = trustedManifest.ownerSha256;
EngineProcess process(config);
// 在 I/O 线程合并等待并 poll；Ready 后再交出客户端能力。
```

预期 SHA-256 必须由调用方已经信任的构建／发行元数据提供。不能从待连接进程、公开诊断、候选程序同目录下未经验证的清单取值，也不能现读任意候选文件的哈希再把它当成批准依据。当前没有新增发行清单签名、系统安装目录信任或版本选择器；测试从固定构建夹具取得预期值，仅验证机制。

两个字段默认空，保留原开发启动行为。直接宿主只设置 hostSha256；独立所有者只要启用校验，就必须同时设置两项。格式为 64 位十六进制，接受大小写并规范为小写；缺一项、非法字符、非绝对路径或内嵌 NUL 均拒绝。ownerSha256 不能用于没有 ownerExecutable 的配置。两条路径实际指向同一个文件对象，包括不同硬链接名称时，拒绝递归所有者配置。

## 固定文件和路径

每个程序以 GENERIC_READ／FILE_SHARE_READ 打开，不共享写入或删除，拒绝最终文件为目录或重解析点。通过文件句柄取得实际卷和路径，按卷 GUID 从根到父目录逐个持有目录对象：允许同级文件的正常读写，拒绝目录删除／改名，拒绝路径上的重解析目录。完成目录固定后重新打开解析路径，要求卷序列号与 128 位文件 ID 一致，避免获取目录句柄之前的路径替换进入启动链。

哈希从保留的同一个文件对象按 64 KiB 块计算，不整包读入内存；有效文件大小为 1 字节至 512 MiB。需要支持卷 GUID 路径与 FileIdInfo 的本地文件系统，无法取得这些信息时拒绝，不退化为仅比较名称。远程共享不在此接口范围内。

启动使用从已固定文件取得的规范 DOS 路径，实际身份核对使用原生映像路径和文件 ID。DOS 路径只是定位参数，不是信任来源。开启校验时显式使用 Windows 系统目录作为工作目录，基础／主题／个人表等仍通过绝对路径提供。

文件及目录句柄至少保持到 EngineProcess 销毁，包括它已经观察到 Exited 的阶段。[客户端独立校验](ENGINE_VERIFIED_PEER.md)使注册、端点与查询连接共享只读部署保护；启动方销毁后，最后一个引用释放才解除固定。更新应写入新的版本目录并在合适边界切换；不能覆盖仍被固定的程序或重命名其父目录。同级文件创建不受目录固定影响。构造失败会释放已经取得的文件／目录句柄，不留下公开占位或自动重试任务。

## 核对实际进程

直接模式在挂起创建真实宿主、加入自有 Job 后，用 CreateProcess 返回并保留的进程对象核对原生映像路径，并重新取得进程映像路径对应的文件 ID。两者都须与固定文件对应，进程还须存活；成功后才恢复主线程。不是打开一个公开 PID 再接受其自报路径。

独立模式先按相同顺序校验所有者。可信启动方将规范宿主路径和预期宿主哈希作为 `--host`／`--host-sha256` 配置交给已核对的所有者；内层 EngineProcess 在恢复宿主线程前再次固定并核对宿主。外层收到 ready 后，原私有启动映射仍只提供真实所有者对象中的受限宿主对象；外层再核对该对象的映像，成功后才保留 engineHandle 并进入 Ready。

初始文件不匹配时抛出固定 Authentication 错误，哈希不符为 ERROR_INVALID_IMAGE_HASH（577），尚未创建查询进程。挂起进程检查失败会终止本次实际创建的进程，不执行其主线程；私有交接中的宿主映像不匹配记 Failure::DeploymentIdentity 并终止自有 Job，保留 Win32 原因，不保存错误宿主到 engineHandle，不交出端点或接入能力。已有 InstanceBusy 仍是明确拒绝，不据此附着其他实例。

`status().deploymentRequired` 表示是否配置了校验；`deploymentVerified` 只在全部初始映像核对成功并进入 Ready 后为 true。此值在停止／退出后仍保留，描述本次启动的历史事实，不是持续完整性监控或进程健康。只读诊断同步这两项和 deployment_identity 失败类别；记录仍是非可信观察，不能被其他客户端拿来代替独立核对或能力交接。开发中的 v1 诊断包 flags 增加两个已知位，未知位仍拒绝；旧读取器遇到启用校验的记录会拒绝，开发程序应配套构建。

## 验证与边界

- `engine_deployment` 在临时复制的真实 EXE 上检查大小写哈希、坏格式、缺失配对、空文件／目录拒绝、同内容不同文件和硬链接身份；持有期间写入、删除、程序／父目录改名失败，同级文件可创建，释放后可以改名和删除。
- 使用真实挂起子进程验证直接启动、同对象成功、其他文件／错误对象／已退出对象拒绝，以及默认未校验模式保持。故障所有者发布一份格式正确的私有锚点，但指向错误宿主，外层必须以 DeploymentIdentity 拒绝，诊断为 required=true／verified=false。
- `engine_owner_process`／`engine_owner_full` 使用可信夹具哈希开启双程序校验，覆盖三个独立客户端动态接入及实际简繁／假名／FF14／部分选词回归；另核对状态／诊断、同标识竞争拒绝、动态申请／撤销和正常退出。
- 原管道认证、控制／接入、诊断和静态基准继续回归；基准指纹增加 EngineDeployment.h 与复用的 BaseDictionaryFiles.h。静态基准默认不开部署校验，不代表文件固定／双层哈希或新增所有者成本已测量。

这项检查只覆盖批准的 EXE 文件与初始映像对象，不是进程内存逐字节验证、所有 DLL 的白名单、Authenticode 签名或同用户恶意进程／管理员隔离。`EngineDeployment::verify` 接受调用方已保留的真实进程对象，本身不取得客户端能力、检查注册授权或提供公开发现。可信部署元数据的来源、发现握手、竞争失败后的可信交接与正式常驻生命周期仍须继续实现；原 Job、撤销、精确期限及无重放规则保持。

在仓库根目录用 PowerShell 7 复现：

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_deployment|engine_process|engine_diagnostics|engine_owner_process|engine_owner_full)$' --output-on-failure
```
