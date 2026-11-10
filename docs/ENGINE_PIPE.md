# Windows 认证本地管道

WIN-03b，2026-11-10。`Windows/EnginePipe.h` 提供实验传输层，真实句柄与子进程测试在 `Windows/tests/EnginePipeTests.cpp`。本批完成管道连接认证、异步分帧和取消回收，尚未连接 ServerConnection、Executor、真实词库查询或 TSF；子进程只做固定消息往返。发行仍为 0.4.4-alpha.22，没有生成或安装新包。

## 端点与信任来源

每个 Listener 使用 BCrypt 生成的 256 位随机名称，格式为 `\\.\pipe\Harukoto.Engine.v1.<64 位小写十六进制>`。客户端和监听器都严格检查本地名称格式；服务端使用 `FILE_FLAG_FIRST_PIPE_INSTANCE` 拒绝同名抢占，`PIPE_REJECT_REMOTE_CLIENTS` 拒绝远程客户端。名称不是认证凭据，不写输入内容或用户身份到名称中。

管道使用非继承句柄与受保护 DACL，唯一允许 ACE 指向当前进程令牌的登录 SID。客户端权限明确为 `FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE`，不使用包含 `FILE_CREATE_PIPE_INSTANCE` 的通用写权限。一个端点只有一个管道实例；连接结束后不复用该 Listener。多应用共用引擎需要后续宿主分配端点并限制连接总数，本批没有实现服务发现或多客户端接入服务。

客户端 `Channel::connect(name, expectedServer)` 必须接收可信启动流程保留的进程 HANDLE，例如 `CreateProcessW` 返回的 `hProcess`；句柄须允许查询进程令牌、读取进程 ID 和等待。函数复制并保留进程内核对象，检查仍存活，比较进程令牌，再用 `GetNamedPipeServerProcessId` 确认实际服务端属于该对象，并复查存活。禁止将管道、注册表或其他不可信来源给出的 PID 重新打开后当作可信身份。连接失败直接返回错误，没有自动等待、重试或转向其他进程。

身份同时比较用户 SID、登录 SID、AuthenticationId 和 Windows session。只接受当前桌面登录，缺登录 SID 时拒绝；不宣称支持服务账户、AppContainer 或跨登录会话代理。进程对象匹配确认的是可信调用方选择的那个进程，不是可执行文件签名验证；同用户恶意代码注入、进程启动路径的信任和版本选择仍需宿主管理。

客户端以 `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION` 打开，服务端只能查询身份。服务端首次收到的完整有效帧必须是空载荷 Hello；读取后先 `ImpersonateNamedPipeClient`，查询实际连接令牌并与预存身份精确比较，恢复线程身份后才交出该帧。验证前服务端不能发送业务回复。失败不向业务层交付帧；恢复身份失败时终止进程，避免在线程仍模拟客户端时继续执行。认证后仍须由 ServerConnection 检查握手和每项业务，不把身份数字当凭据。

创建监听器、连接或执行服务端认证时拒绝已有线程模拟身份；拒绝不会撤销调用者原来的模拟状态。传输对象必须由专用 I/O 所属线程串行使用，不可复制或移动，不跨线程直接操作。

## 有界分帧与读写

管道使用字节模式和 OVERLAPPED。每个 Channel 仅有一个读操作、一个待领取帧和一个写操作，不维护额外发送队列。`beginRead` 在已有读操作或未领取帧时拒绝；`beginWrite` 在已有写操作或服务端未认证时拒绝。调用方须限制自身排队，返回 false 不代表可以丢弃按键。

先收满 56 字节头，再用[封装解码器](ENGINE_PROTOCOL.md)检查魔数、版本、类型、保留位、身份及 64 KiB 长度上限，通过后才扩展正文缓冲。坏头直接关闭，不扫描魔数恢复；不按未经检查的长度分配。头和正文可分多次到达，每次只读取当前帧剩余字节，粘连的下一帧留在管道里，领取当前帧并再次 `beginRead` 后才读取。

`beginWrite` 使用同一封装校验和长度上限；无效待发帧抛出异常，调用方处理后才可继续。传输只校验封装，Key、候选、选择与 Error 的完整类型化载荷仍由[消息层](ENGINE_MESSAGES.md)验证，客户端响应归属仍由 ClientGate 检查。认证和分帧都不授予上屏权限。

`poll(time)` 不等待 I/O，每个读／写方向每次最多处理 8 个完成片段；未完成时交回事件循环。`readEvent()`／`writeEvent()` 只在相应操作尚待回收时返回事件，Listener 提供连接事件。调用方要在等待前主动 poll，等待这些事件与最近截止时间，醒来后再次 poll；不能仅等待事件而忽略绝对 deadline，也不能依赖反复等待已经领取的事件推进下一帧。执行器完成通知如何并入 Windows 事件循环仍待宿主实现。

## 超时、断连与资源生命期

now／deadline 由调用方传入同一单调时钟；`pipe::now()` 与 Executor 默认实现相同，都是 `steady_clock` 毫秒数。使用自定义 Executor Clock 时，管道和服务端也必须统一时基，不能混入墙上时间或不同纪元的绝对毫秒数。

监听、每次读和每次写都有固定截止时间；到期即失败，部分头或正文到来不会续期。本层不设置正式产品超时值。连接超时和 I/O 失败关闭通道，撤销未领取帧。关闭后不能开始新读写或取出旧结果；不会自动重放 Key／Select。

`CancelIoEx` 只是请求取消。close 保留管道句柄、缓冲、OVERLAPPED 和事件，直到 `drain()` 确认操作完成后才释放句柄；取消与正常完成竞争也按完成结果回收。drain 仅用于关闭后的操作，活连接调用不会误消费完成字节。析构请求取消并等待所有尚未回收的操作结束，故只能在 I/O 所属线程收尾，不能放在 TSF 前台线程等待。

传输关闭后，未来宿主还必须断开对应 ServerConnection／ClientGate、撤销执行器旧任务、清掉所属连接的待发缓存，保持已确认 PendingDelivery 文字的原上下文归属。本批未连接这些对象，不能据此宣称已实现完整断连恢复或引擎热更新。

## 已验证与下一步

MSVC Release 新目标构建通过，相关 5 项 CTest（engine_pipe、engine_protocol、engine_messages、engine_server、engine_executor）通过。当前注册测试共 39 项，本批未重复其余既有测试。保留测试中 `/UNDEBUG` 覆盖 Release `NDEBUG` 的既有编译提示。

新增 engine_pipe 使用真实 Windows 管道和进程，覆盖：

- 当前登录双向握手、最大 64 KiB 正文、单读／写容量、待领取帧不能覆盖和重复领取拒绝。
- 头／体碎片、粘连帧、坏魔数／版本／类型／保留位、超长声明、非 Hello 首帧和非空 Hello。
- 监听超时、部分头不续期、连接断端、关闭撤销未领取帧、同时挂起读写的取消回收，以及 20 次挂起读取析构。
- 真实 DACL 的保护位、唯一登录 SID 与精确权限；AccessCheck 验证当前令牌可读写而不能创建管道实例。匿名客户端认证失败，所属线程身份恢复；已有模拟身份时拒绝且保持原模拟状态。
- 非法／远程名称拒绝、随机名称不重复、同名监听碰撞；真实子进程使用只继承 ready event 的句柄列表启动，正确保留进程对象完成握手和最大帧往返，错误进程对象及已退出对象拒绝。
- 身份四项分别变化的比较拒绝；这只是字段验证，没有创建其他用户或登录会话。

远程拒绝依靠名称校验和系统管道标志，本机没有发起跨机器攻击测试；不同用户、不同真实登录、UAC 完整性级别组合、系统资源耗尽及真实应用 TSF 仍待验证。此次进程间往返只证明传输路径，未测真实词库跨进程候选或延迟、总内存收益。

复现（仓库根目录，PowerShell 7；已有 MSVC CMake 构建目录）：

```powershell
cmake --build .build/windows/msvc --config Release --target harukoto_engine_pipe_test
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_pipe|engine_protocol|engine_messages|engine_server|engine_executor)$' --output-on-failure
```

接着实现实验查询宿主：可信启动与端点交接、ServerConnection 和 Executor 的同线程调度、固定词库快照的真实跨进程查询、连接／任务总容量及退出回收。先验证简繁、日语、FF14、部分选择、取消和错误关闭，再做多应用共享内存与逐键延迟对照；最后接入 TSF、版本切换和更新回退。
