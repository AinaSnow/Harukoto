# 受控客户端接入与独立端点申请

WIN-03b，2026-11-10。`Windows/EngineRegistration.h` 为开发用[独立所有者](ENGINE_OWNER.md)提供每客户端独立的私有接入通道。可信启动方预分配能力并交接后，客户端自行 Allocate／Inspect，不再由启动方逐次代申请查询端点。当前未安装、自动启动或接入 TSF；Windows 仍为 0.4.4-alpha.22、FF14 0.3.0。

## 配置与交接

```cpp
EngineProcess::Config config{hostExecutable, base, themes, personal};
config.dynamicEndpoints = true;
config.ownerExecutable = ownerExecutable;
config.ownerInstance = L"harukoto-main";
config.ownerRegistrations = 4; // 0～8；只允许独立所有者模式。
EngineProcess process(config);
// 合并 process.waitHandles()／deadline()，poll 至 Ready。
auto registration = process.takeRegistration(pipe::now());
```

默认 0 保持原启动接口。启动方创建指定数量的未命名控制映射／事件，通过显式继承列表交给所有者；所有者清除继承位后，不把这些通道传给宿主。CLI 的每个 `--registration` 对应一个严格的 section,request,response 三句柄组，最多八组，拒绝组内、组间及与六个启动句柄的重复值。

takeRegistration 只在 Ready 后按顺序一次性取出预分配槽；未就绪或用完返回空。交接包含三个私有控制句柄，以及所有者、真实宿主两个非继承且仅 query／wait 权限的进程对象。它不包含 stop、Job、启动方的上游控制或 PROCESS_DUP_HANDLE 权限。宿主对象仍来自原启动交接锚点，没有按公开 PID 重新打开进程。

可在当前服务线程使用返回对象；交给独立客户端时，必须在第一条请求之前，将 handles()、ownerHandle()、engineHandle() 的副本列入该客户端的显式继承列表，并关闭启动方副本。客户端用导入构造函数接管三控制句柄，复制／约束两个可信进程对象，检查它们存活且属于当前登录身份。不能共享同一槽给多个客户端，也不能使用后重建 Client 来重置序号。所有方法属于服务／I/O 线程，不能直接放进 TSF 前台回调。

## 客户端请求

```cpp
registration->request(control::Operation::Allocate, pipe::now());
// 合并 registration->waitHandles() 和 deadline()，唤醒／到期后 poll。
auto result = registration->take(pipe::now());
if (result && result->endpoint) {
    auto channel = pipe::Channel::connect(result->endpoint->name,
                                         result->endpoint->process.get());
    // 继续原查询 Hello／ClientGate 握手，然后处理输入和选择。
}
```

request 支持 Allocate／Inspect，默认期限 5 秒，沿用控制层的范围与精确到期规则；Pending 或未领取 Ready 回复占用本客户端槽，不接受新请求。take 至多领取一次，Full／Backend 也是正式结果；Allocate 成功附上宿主 query／wait 副本，Inspect 不创建端点。运行代次、操作和请求序号按[私有控制格式](ENGINE_CONTROL.md)校验，既有查询协议与 Kind 不变。

所有者、宿主任一实际退出都让注册通道进入 Failed／Closed，即使回复事件已触发，也撤销未领取结果。超时、坏回复或运行代次变化永久退役客户端通道；不自动重放、重新注册或连接别的宿主。已有查询连接仍由自己的管道与 ClientGate 管理，客户端控制超时本身不清空其他客户端组字。

## 调度与资源边界

所有者将一个启动方端口和最多八个客户端端口轮转调度，每个端口最多一个请求，向宿主仍只有一个在途请求。每次选中后从下一端口开始下一轮，回复使用原端口的精确请求身份；不同客户端可以同时使用序号 1。等待时合并宿主回复、实际退出、stop 和可处理的请求事件；宿主请求在途期间不等待其他已触发的请求事件，避免忙循环。

客户端请求解码／序号错误只停用该客户端端口，后续事件不再进入等待集合；该端口没有错误回复，客户端按原期限失败。启动方上游的损坏仍沿用整体失败契约；宿主控制故障仍让所有者停止宿主。stop 优先于调度，停止后不再处理注册请求。内层宿主控制期限仍为 5 秒，与客户端包含轮转等待的端到端期限独立。

注册槽和宿主查询端点是两种容量：本批注册槽在启动时固定分配，客户端退出、超时或坏请求后不复用，也不能追加；宿主端点仍受 connections 限制，查询断开后按原取消完成／Ticket 回收规则释放。客户端退出可能留下已分配但未连接的 Listener，仍按分配起算的 30 秒过期。没有无界队列、自动回收注册槽或实际应用数量不受限的承诺。

EngineProcess::status().claimed 只统计由启动方自身领取的查询端点，不包含各客户端直接申请数量；全局端点状态用任一健康注册通道的 Inspect 查看。

## 验证与下一步

- engine_owner_process／full 继续原微型／全量基础词库＋FF14 回归，并额外启动三个独立客户端自行 Inspect／Allocate。前两个连接并行组字，第一个退出后第三个申请新端点；保留剩余组字、中文简繁、日语假名、FF14、部分选择及重复选择拒绝。
- 损坏第四个客户端请求后，其他客户端和启动方继续使用；坏端口按期限失败。另以八客户端槽和上游槽同时 Pending，连续四轮验证独立序号、回复代次、仅两个分配成功／六个 Full、Inspect 不分配和上游仍可领取。
- 正常停止与所有者崩溃分别覆盖 Idle、Pending 及未领取 Ready 回复撤销；故障宿主在注册请求期间退出，通道失败。检查注册进程对象不能 TerminateProcess、预分配槽用尽及错误配置拒绝。原单实例竞争、直接宿主、认证管道与静态基准继续回归。

此接口依赖可信启动方预先交接能力。普通应用尚不能自行查找所有者、获得注册槽或验证部署身份；不是公开服务发现或正式多应用注册系统。可信发现、竞争失败后的交接、动态注册／退出回收、正式常驻生命周期和用户诊断入口仍待实现。原性能报告没有测这条注册调度路径，本批也未修改安装、用户词库或发行版本。

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_control|engine_process|engine_owner_process|engine_owner_full)$' --output-on-failure
```
