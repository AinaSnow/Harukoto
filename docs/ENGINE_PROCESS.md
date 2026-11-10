# 独立引擎的受控启动与端点领取

最新进度：[受控独立所有者](ENGINE_OWNER.md)已支持独立进程监督、可信宿主对象交接和三个真实客户端的退出／重新接入，并加入[可选单实例竞争](ENGINE_INSTANCE.md)及[受控客户端接入](ENGINE_REGISTRATION.md)。可选 ownerExecutable 要求开启[动态端点](ENGINE_CONTROL.md)，ownerInstance 指定稳定部署范围，ownerLease 仅供所有者向直接动态宿主借用传递占位；ownerRegistrations 预分配 0～8 个独立客户端通道，Ready 后用 takeRegistration 一次交接。默认直接模式及[多进程竞争测量](ENGINE_IPC_PERFORMANCE.md)保持原契约。正式常驻生命周期、可信发现／动态应用注册和 TSF 仍待接入；以下启动批次的验证记录按历史范围阅读。

WIN-03b，2026-11-10。`Windows/EngineProcess.h` 将原先真实宿主测试内的启动逻辑提取为可复用模块，管理一个自己创建的引擎进程、1～8 个随机单次连接端点和退出状态。真实微型／全量词库测试已改用该模块；宿主业务仍见[查询宿主](ENGINE_HOST.md)。发行基线保持 Windows 0.4.4-alpha.22、FF14 0.3.0，不安装或自动启动新程序。

启动所有权及基础退出监督已完成，后续增补了可选动态模式和受控独立所有者。没有跨应用服务发现、签名／版本选择、正式常驻服务、自动重启、快照换代或 TSF 接入。不能把某个应用各自创建一个 EngineProcess 当成多应用共享引擎。

## 可信配置与启动

调用者提供 Config：可执行文件与基础词库的绝对路径、可选主题／个人表绝对路径、连接数、启动和停止宽限。连接默认为 2；启动 30 秒、停止 5 秒，时长范围均为 1～300,000 毫秒，是开发值。拒绝相对路径、空必填路径、内嵌 NUL、超长命令行、非法数量或时限。缺省个人表不读取用户安装数据。

绝对路径只约束传参，不等于文件可信验证。可信部署目录的权限、可执行文件与词库身份、签名、版本选择和路径替换竞争仍由后续部署层解决；模块不能接收管道对端自报的可执行路径后直接启动。启动前拒绝当前线程已有模拟令牌。

直接调用 `CreateProcessW`，明确指定可执行文件，按 Windows argv 规则引用每个参数，不经过 shell；工作目录固定为可执行文件所在目录，环境沿用调用方。CREATE_NO_WINDOW 隐藏启动，CREATE_SUSPENDED 保证主线程先不运行。Job 使用 KILL_ON_JOB_CLOSE，成功加入后才 ResumeThread；分配 Job 或恢复线程失败时终止自己创建的挂起子进程。不搜索 PID，也不结束用户的其他输入法或应用。

父进程控制事件和保留的进程／Job 句柄均不可继承。静态模式仅为 ready／stop 创建短期可继承副本，通过 STARTUPINFOEX 的显式 HANDLE_LIST 传递；动态模式另加未命名映射和两个控制事件的副本。子进程创建后关闭父方的短期副本，真实宿主接管后再清除子方控制句柄的继承标记。列表中不包含其他继承句柄，避免沿用调用方原有可继承事件。

指定 ownerExecutable 时先创建独立所有者，再由它按同一规则创建动态宿主；外层继承列表额外携带一次性宿主对象交接映射。外层 ready 必须等待宿主实际 ready 和可信对象复制完成；失败记 ControlIo，按原 Job 路径终止。根进程与查询对象的区分、嵌套所有权和固定句柄槽规则见[独立所有者](ENGINE_OWNER.md)。

构造包含进程创建、文件路径处理等系统调用；连接包含 CreateFile 和令牌检查。这些调用不能保证前台延迟。所有方法只允许在同一服务／I/O 线程使用，不得直接放进 TSF 前台回调。

## 状态、等待与截止时间

| 状态 | 意义 | 等待句柄／截止时间 |
| --- | --- | --- |
| Starting | 已创建子进程，尚未观察到 ready | 进程、ready／启动绝对期限 |
| Ready | 观察到 ready，允许领取端点 | 进程／无管理器期限 |
| Stopping | 已发送 stop，禁止新连接 | 进程／停止绝对期限 |
| Terminating | 已向自己的 Job 请求终止 | 进程／无管理器期限，等待实际退出 |
| Exited | 已观察到进程退出并读取退出码 | 空等待集合／无期限 |

`poll(time)` 只以零等待检查事件和进程。进程退出优先于 ready；两者都已触发时，不会暴露端点。仍存活时先判断绝对期限，再判断 ready；到达启动期限即过期，不因看到 ready 续期。时间统一使用 `pipe::now()` 的单调毫秒时钟，不能混用墙钟。

外层事件循环合并 `waitHandles()`、其他通道／完成事件，取非零 `deadline()` 和业务期限中最近者作为等待上限。每次唤醒或到期调用 poll；Ready／Stopping 不再等待已经触发的 ready，避免忙循环。模块没有后台定时器，调用者若停止 poll，超时不会自行执行。Terminating 仍须等待真实进程对象触发，不能把发出终止请求当成已经退出。

动态模式在 Ready 有 Pending 控制请求时另加入回复事件和控制期限；超时仅让控制槽进入 Failed，不自动终止已就绪宿主，原查询连接可继续使用。停止和实际退出均撤销未领取控制回复，固定失败原因保留。接口、单槽规则、容量与信任边界见[私有控制说明](ENGINE_CONTROL.md#调用和等待)。

预分配注册槽可在交接后用 bindRegistration(id, trustedProcessHandle) 一次绑定真实客户端进程对象，或用 revokeRegistration(id) 主动撤销。Ready 时 waitHandles 包含未退役槽的撤销事件及已绑定进程对象；唤醒后 poll 观察退出并触发撤销，同时关闭自己的受限进程副本。所有者回收对应端口，迟到回复丢弃，客户端尚未领取的结果失效；现有查询连接继续按原契约运行。槽不复用／补充，编号仅在此所有者有效。status 的 registrationConfigured／Issued／Bound／Revoked 统计配置、已发出、当前保留绑定、已退役数，需要持续 poll 才刷新；完整权限与交接步骤见[注册寿命说明](ENGINE_REGISTRATION.md#撤销与客户端寿命)。

`requestStop(time)` 在 Starting／Ready 发送 stop，进入停止宽限；重复调用不延长期限。超时通过 TerminateJobObject 仅终止自己的子进程树。析构不等待退出，关闭自己的 Job 作为放弃所有权时的终止保护。正常释放应先 requestStop、继续等待并 poll 至 Exited，再销毁；只有观察退出才能断言词库文件保护已释放。即使后端永久阻塞导致 Host 的 join 不返回，独立拥有者仍可到期终止宿主。

模块不自动重启，停止期间迟到的 ready 不恢复 Ready；不重放 Key／Select，也不改变 PendingDelivery 对已确认文字和原 TSF context 的归属。正式升级／回退还需设计组字边界、未确认选择、旧连接寿命与旧版保留。

## 端点与诊断接口

Config 新增 `threads`（1～16，默认 2）与可选 `trace` 绝对路径，传给开发宿主的 `--threads`／`--trace`；空路径关闭计时。相对路径、内嵌 NUL 或非法线程数在启动前拒绝。追踪文件存在时真实宿主拒绝启动，管理器观察为 StartupExit，不覆盖文件；计时说明见[进程间测量](ENGINE_IPC_PERFORMANCE.md#可选服务计时与混合负载)。

静态模式的 `takeEndpoint(time)` 仅在 Ready 领取下一个端点，返回随机名称及一份非继承、仅 PROCESS_QUERY_LIMITED_INFORMATION／SYNCHRONIZE 权限的保留进程句柄；领取即消费，即使之后没有连接或交接失败也不归还。可信启动方可明确复制到自己创建的客户端句柄列表，不得只传 PID 后重新打开。`claim(time)` 复用该接口，并在当前 I/O 线程用返回的进程 HANDLE 调用 Channel::connect。Channel 再复制该内核对象、核验进程令牌和实际管道服务端进程。模块不从数字 PID 重新打开进程作为信任依据。动态模式改用 requestControl／takeControl，成功分配同样附带只读进程对象，旧 takeEndpoint／claim 返回空。

一个随机端点只尝试一次，即使连接抛出 pipe::Error 也计为已领取，不等待管道、不重试、不复用名称。未 Ready、停止中、已退出或端点用尽返回空指针。连接失败由调用方处理，不影响其他尚未领取端点；不得自动把未确认按键转给其他连接。返回的 Channel 仍由 I/O 线程负责 Hello／ClientGate、候选快照、取消和关闭回收，claim 成功不是业务握手完成。

`status()` 返回固定元数据：状态、Failure、必要的 Win32 数字、可选退出码、已领取／配置端点数量。Failure 区分 StartupExit、UnexpectedExit、StartupTimeout、StopTimeout、ControlIo、InstanceBusy；最后一项用于独立所有者启动阶段的重复实例退出码 183，不自动附着。超时原因保留到真实退出，不被终止退出码覆盖。其余未观察 ready 即退出，即使退出码 0 也算 StartupExit；已就绪后的正常退出码 0 允许作为宿主自行收尾。构造／系统检查失败抛出固定异常，不能伪造 Ready。当前没有用户可见的持久诊断入口，不记录输入、候选或异常详情。

`processHandle()` 是受管理根进程的借用句柄，用于退出监督；直接模式是宿主，独立所有者模式是所有者进程。查询认证使用 `engineHandle()`：直接模式与根相同，独立模式在 Ready 前为空，之后是固定的只读宿主对象。成功分配返回的 Endpoint 自动选用这个查询对象，不能把所有者对象当成查询服务端。借用句柄不得关闭或修改；跨越模块寿命须自行 DuplicateHandle，它们不能替代文件签名校验。模块不可复制／移动，不能跨线程并发调用。

## 验证与接手

新增 engine_process 使用真正的子进程和相同控制参数契约，覆盖参数中的中文、空格、引号、尾反斜杠，无关可继承事件隔离、父方控制／进程句柄不可继承、连接失败后端点消耗、就绪前拒绝领取、ready／退出同时触发、启动期限（含 ready 已触发但在期限才观察）、启动中停止、重复停止不续期、停止不响应、崩溃分类与放弃所有者后的 Job 回收。超时测试向 poll 传入受控到期时间，不等待实际 30 秒或宣称精确墙钟计时验证。故障子进程不读取真实词库或个人数据。

engine_host_process／full 使用新模块领取两个真实连接，继续完整逐键查询、分页、部分／重复选择与直调字节对照，并验证端点用尽、stop、崩溃后状态和拒绝领取，以及退出后的文件保护释放。测试连接仍来自同一客户端测试进程，没有据此声称已完成多应用服务发现或内存收益评估。

本批完整 MSVC Release 构建通过，4 项相关测试通过；最后修改后分别复测受影响项：engine_process 约 0.26 秒、engine_host_process 6.41 秒、engine_host_full 14.36 秒，未改变的 engine_host 为本批前次 0.08 秒。当前共注册 43 项，未重复其余既有测试。仅保留既有 /UNDEBUG 覆盖 Release NDEBUG 的编译提示。

仓库根目录在 PowerShell 7 中运行：

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_process|engine_host|engine_host_process|engine_host_full)$' --output-on-failure
```

一次性端点交接、多客户端竞争／总私有量、动态端点和私有诊断已完成；[受控独立所有者](ENGINE_OWNER.md)新增三个独立客户端、可信对象交接及故障回归。最新可选实例竞争和验证见[单实例说明](ENGINE_INSTANCE.md)及[开发状态](DEVELOPMENT_STATUS.md)。下一步明确正式常驻生命周期、可信服务发现／应用注册；不得从公开 PID 自认证。实验根进程仍属于可信启动方的 Job，不能直接作为正式常驻服务；版本更新、组字边界换代／回退和 TSF 继续按 WIN-03 推进。
