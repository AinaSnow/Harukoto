# 引擎所有者的单实例竞争

WIN-03b，2026-11-10。`Windows/EngineInstance.h` 为开发用[独立所有者](ENGINE_OWNER.md)增加可选占位，避免同一登录身份、同一部署范围重复启动宿主。当前仍是可信启动方管理的实验进程树，没有安装、自动启动或接入 TSF；发行与本机安装保持 Windows 0.4.4-alpha.22、FF14 0.3.0。

## 使用与范围

```cpp
EngineProcess::Config config{hostExecutable, base, themes, personal};
config.dynamicEndpoints = true;
config.ownerExecutable = ownerExecutable;
config.ownerInstance = L"harukoto-main"; // 可信部署配置；升级前后保持一致。
EngineProcess process(config);
```

ownerInstance 仅允许 1～64 个小写 ASCII 字母、数字或连字符，必须同时配置 ownerExecutable；无效组合在创建子进程前拒绝。空值保持原有测试／直接启动行为，没有自动推导实例标识。不同标识允许并行，因此正式接入时应由部署配置统一选择，不能用每次随机值、版本号或程序路径规避竞争。

标识通过所有者的 `--instance` 传递。所有者在启动宿主之前原子创建命名 mutex，名称由 Local 会话命名空间、当前 Windows session、AuthenticationId、登录 SID 与部署标识组成。对象只作为存在占位，从不获取其 mutex 所有权、等待、ReleaseMutex 或根据 abandoned 状态接管。显式受保护 DACL 只向当前登录 SID 授予 SYNCHRONIZE／READ_CONTROL，原始句柄不可继承。

已有同名 mutex 时立即关闭本次取得的副本，以退出码 183（ERROR_ALREADY_EXISTS）拒绝；EngineProcess 将所有者启动阶段的这个结果记为 Failure::InstanceBusy，不发送 ready、不启动第二个宿主、不返回端点。名称被其他类型对象占用或打开权限不足时同样拒绝，记录为普通启动失败，不能解释为可连接的现有服务。没有等待重试或自动附着。

不同实际登录身份的对象名分开；本批自动测试在当前登录身份执行，没有验证另一个账户、会话切换或提权后的实际接入。

## 为什么宿主也保留占位

仅在所有者保存占位，会在所有者崩溃、Job 正在异步回收旧宿主期间提前放行。所有者因此通过内层 EngineProcess 的 ownerLease 借用字段，显式复制一份受限占位句柄到宿主继承列表，并用 `--owner-lease` 指定；该字段只允许直接动态宿主模式，不允许嵌套所有者或同时带 ownerInstance。借用只持续到启动完成，外层启动方和查询客户端不持有这份占位。

宿主验证继承句柄不与 ready／stop／控制句柄重合，且仅在完整动态控制模式接受；清除继承位后，固定保留这一份句柄直到操作系统实际终止进程。`ownerLease.release()` 是有意交给操作系统回收，不是在每次请求中泄漏句柄。不要改成栈析构时提前关闭，也不要复制给查询客户端或未来的子进程。启动参数尚未通过验证时可以按异常路径关闭，此时尚未读取词库或启动查询。

正常停止由所有者等待宿主退出；所有者崩溃或启动方放弃 Job 时，旧宿主的占位仍持续到它实际终止。最后一份占位句柄释放后，同标识的新启动才可以成功。仍保留已退出的进程对象、旧 EngineProcess 或旧 Job 不会单独占用实例标识。

## 信任与后续工作

这个对象只回答“当前能否创建”，不提供所有者 PID、管道、路径、版本或控制句柄，不能证明已有实例可信。查询仍使用私有交接得到的真实宿主对象认证；未添加 OpenProcess(publicPid)、公开映射恢复信任、对端自报身份或自动连接同名管道。

同登录身份的其他程序仍可能预先占名或保留打开的对象，造成拒绝启动；遇到已有对象不会强关句柄、删除公开记录或强行接管。DACL 与名称不是同用户恶意程序隔离方案。正式发现还需要独立验证部署身份／对象信任，明确竞争失败后的可信交接、启动方退出后的生命周期、旧版本退出确认及普通应用注册。本批也未修改直接启动宿主的默认契约，不能宣称所有入口已经强制全局单实例。

## 验证

- engine_instance 检查非法标识拒绝、受保护的单条登录 SID ACE、权限与不可继承属性；同名占位重复拒绝、不同标识并行、对象类型冲突和空 DACL 的访问拒绝。
- 真实继承子进程在启动方关闭原占位后继续阻止重用；分别正常退出和被强制终止后可重新创建，旧进程对象仍保留。此夹具确定性覆盖占位的跨进程寿命，不依赖宿主恰好处于某个短暂清理时间窗。
- engine_owner_process／full 在微型／完整基础词库及 FF14 临时夹具启动四个独立所有者竞争：恰有一个 Ready，三个明确 InstanceBusy；只有赢家创建宿主追踪文件。再验证同标识拒绝、不同标识并行、正常停止、所有者崩溃／宿主实际退出、坏词库启动后可重新启动。原三客户端查询／退出／接入和故障回归保留。
- 直接宿主、私有控制、认证管道、进程管理和静态进程间基准继续回归；基准指纹增加 EngineInstance.h，仍未测量所有者或占位模式的性能。

在仓库根目录使用 PowerShell 7：

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_instance|engine_process|engine_owner_process|engine_owner_full)$' --output-on-failure
```
