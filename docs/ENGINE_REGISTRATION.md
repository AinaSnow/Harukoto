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

默认 0 保持原启动接口。启动方创建指定数量的未命名控制映射／事件，以及每槽一个不可逆的手动复位撤销事件，通过显式继承列表交给所有者；所有者清除继承位后，不把这些通道传给宿主。CLI 的每个 `--registration` 对应一个严格的 section,request,response,revoked 四句柄组，最多八组，拒绝组内、组间及与六个启动句柄的重复值。旧三句柄启动参数不再接受，开发程序须配套构建。

takeRegistration 只在 Ready 后按顺序一次性取出预分配槽；未就绪或用完返回空。交接包含三个私有控制句柄、仅 SYNCHRONIZE 权限的非继承撤销事件，以及所有者、真实宿主两个非继承且仅 query／wait 权限的进程对象。id() 为此所有者内从 1 开始的槽编号，只用于可信启动方关联寿命，不是公开认证凭据。它不包含 stop、Job、启动方的上游控制或 PROCESS_DUP_HANDLE 权限。宿主对象仍来自原启动交接锚点，没有按公开 PID 重新打开进程。

可在当前服务线程使用返回对象；交给独立客户端时，必须在第一条请求之前，将 handles()、ownerHandle()、engineHandle()、revocationHandle() 的副本列入该客户端的显式继承列表，并交接 id()。客户端用导入构造函数接管三控制句柄，复制／约束两个可信进程对象，检查它们存活且属于当前登录身份；撤销事件再复制为仅等待权限，不能 SetEvent／ResetEvent。关闭启动方交接副本，不能共享同一槽给多个客户端，也不能使用后重建 Client 来重置序号。所有方法属于服务／I/O 线程，不能直接放进 TSF 前台回调。

## 撤销与客户端寿命

可信启动方创建独立客户端后，可用 bindRegistration(id, clientProcessHandle) 一次绑定 CreateProcess 返回且持续保留的真实进程对象。它保留不可继承的 query／wait 副本，检查对象类型和仍存活进程的当前登录身份；已退出对象直接使槽退役。禁止用公开 PID 再 OpenProcess 来恢复信任。未发出的编号、已撤销槽或重复绑定返回 false；错误对象或权限不足报认证错误。绑定是可选的，未绑定时没有客户端退出监测。

Ready 时 EngineProcess.waitHandles() 加入未退役槽的撤销事件与已绑定进程对象。调用方在事件唤醒后必须继续 poll；它观察客户端退出并触发该槽撤销，不依赖定时睡眠或其他输入。revokeRegistration(id) 可主动单向撤销已发出的槽，重复撤销返回 false。客户端 waitHandles() 包含仅等待的撤销事件；poll／request／take 都先观察撤销，因此 Idle、Pending、未领取 Ready 都进入 Failed／Closed，不能再发请求或领取旧回复。

所有者收到撤销后关闭该端口的映射／请求／回复句柄，移出等待集合。尚未转交的请求不再提交；已经在宿主执行的请求仍须消耗完内层回复，但丢弃结果，不通过已退役端口写回。损坏客户端请求也触发同一撤销路径，只关闭对应客户端。启动方保留的受限进程副本随退役关闭；槽编号和旧控制对象不复用，旧客户端即使继续写保留的映射／事件也不能重新激活端口。

这不强制结束客户端，不关闭已建立的查询连接，也不取消已到达宿主的 Allocate；由后者产生但未连接的 Listener 仍按 30 秒规则过期。只做进程级寿命绑定，进程仍存活但其逻辑输入上下文销毁时需要主动撤销。构造函数为原有手动包装保留不带撤销事件的形式；它没有这项通知能力，不能用于新槽的完整跨进程交接。

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

撤销事件或所有者、宿主任一实际退出都让注册通道进入 Failed／Closed，即使回复事件已触发，也撤销未领取结果；已经保留的首个失败不被覆盖。超时、坏回复或运行代次变化永久退役客户端包装；客户端只有等待权限，这些本地失败不会自行通知所有者，可信启动方须主动撤销或依靠绑定进程退出监测。不自动重放、重新注册或连接别的宿主。已有查询连接仍由自己的管道与 ClientGate 管理，客户端控制超时本身不清空其他客户端组字。

## 调度与资源边界

所有者将一个启动方端口和最多八个客户端端口轮转调度，每个端口最多一个请求，向宿主仍只有一个在途请求。每次选中后从下一端口开始下一轮，回复使用原端口的精确请求身份；不同客户端可以同时使用序号 1。等待时合并宿主回复、实际退出、stop 和可处理的请求事件；宿主请求在途期间不等待其他已触发的请求事件，避免忙循环。

客户端请求解码／序号错误只撤销该客户端端口，后续事件不再进入等待集合；该端口没有错误回复，客户端通过撤销事件进入 Closed。即使内层请求在途，所有者也等待其他客户端的撤销事件。启动方上游的损坏仍沿用整体失败契约；宿主控制故障仍让所有者停止宿主。stop 优先于调度，停止时撤销全部客户端端口。内层宿主控制期限仍为 5 秒，与客户端包含轮转等待的端到端期限独立。

注册槽和宿主查询端点是两种容量：注册槽在启动时固定分配，退出／撤销后回收所有者端口和启动方进程副本，但不复用槽，也不能追加；宿主端点仍受 connections 限制，查询断开后按原取消完成／Ticket 回收规则释放。客户端退出可能留下已分配但未连接的 Listener，仍按分配起算的 30 秒过期。没有无界队列、动态补充注册槽或实际应用数量不受限的承诺。

EngineProcess::status().claimed 只统计由启动方自身领取的查询端点，不包含各客户端直接申请数量；全局端点状态用任一健康注册通道的 Inspect 查看。registrationConfigured／registrationIssued／registrationBound／registrationRevoked 分别为配置槽数、已发出数、当前保留进程绑定数和已退役数；Bound 不是累计绑定次数，状态依赖 poll 观察，不是独立实时诊断服务。

## 验证与下一步

- engine_owner_process／full 继续原微型／全量基础词库＋FF14 回归，并额外启动三个独立客户端自行 Inspect／Allocate。前两个连接并行组字，第一个退出后第三个申请新端点；保留剩余组字、中文简繁、日语假名、FF14、部分选择及重复选择拒绝。
- 损坏第四个客户端请求后，通过撤销事件进入 Closed，其他客户端和启动方继续使用。另以八客户端槽和上游槽同时 Pending，连续四轮验证独立序号、回复代次、仅两个分配成功／六个 Full、Inspect 不分配和上游仍可领取。
- 测试专用宿主通过事件屏障固定首个请求在途，再撤销该槽及排队槽；释放后上游恰好为第二次请求，证明迟到回复被排空且排队 Allocate 未转发。旧映射继续写入不能重新激活，后续上游请求正常。
- 实际独立客户端的正常退出与强制退出触发受限进程对象唤醒；已退出对象绑定立即撤销，错误事件对象被拒绝，重复绑定／撤销和未发出编号被拒绝。未领取 Ready 撤销后无法领取，原查询组字／选词仍正常；新鲜剩余槽可用，旧槽不复用。独立客户端不能设置／重置撤销事件。
- 正常停止与所有者崩溃分别覆盖 Idle、Pending 及未领取 Ready 回复撤销；故障宿主在注册请求期间退出，通道失败。检查注册进程对象不能 TerminateProcess、预分配槽用尽及错误配置拒绝。原单实例竞争、直接宿主、认证管道与静态基准继续回归。

此接口依赖可信启动方预先交接能力。普通应用尚不能自行查找所有者、获得注册槽或验证部署身份；不是公开服务发现或正式多应用注册系统。可信发现、竞争失败后的交接、动态创建／补充注册槽、正式常驻生命周期和用户诊断入口仍待实现。原性能报告没有测这条注册调度路径，本批也未修改安装、用户词库或发行版本。

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_control|engine_process|engine_owner_process|engine_owner_full)$' --output-on-failure
```
