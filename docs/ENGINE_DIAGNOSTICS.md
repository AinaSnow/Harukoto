# 实验引擎只读诊断

WIN-03b，2026-11-11。`EngineDiagnostics.h` 与 `harukoto_engine_diagnose.exe` 用于查看开发中独立引擎的元数据。它与已安装输入法的 [Diagnose.cmd](WINDOWS_DIAGNOSIS.md) 分开；当前 alpha.22 的 TSF 仍使用进程内核心，不能用此工具判断某个应用已完成核心换代。本工具尚未进入安装包。

## 开启与使用

可信启动方在原有 `EngineProcess::Config` 中设置 `diagnostics = true`。默认关闭，可用于直接宿主或独立所有者模式。发布方是持有这个 EngineProcess 的启动进程，观测目标是它管理的根进程：独立模式传所有者 PID，直接模式传宿主 PID。该 PID 可以由开发启动方显示；它只用于指定观察对象，不提供连接能力。

在仓库根目录的 PowerShell 7 中运行：

```powershell
cmake --build .build/windows/msvc --config Release
# 将 12345 换成启用了诊断的受管理根进程 PID。
& ./.build/windows/msvc/Release/harukoto_engine_diagnose.exe --process 12345
& ./.build/windows/msvc/Release/harukoto_engine_diagnose.exe --process 12345 --json
```

工具不自动枚举其他应用，不启动引擎，不申请 Allocate／Inspect，不领取控制回复，也不触发接入、停止、重启或重放。诊断不需要管理员权限，不能读取时返回明确状态；不要为取得状态去终止进程或重新注册输入法。

退出码：0 表示读到了与目标创建时间对应、发布进程仍可观察的快照；1 表示参数错误；2 表示没有可用观察或记录无效。0 不表示引擎健康、查询可用或已通过身份认证。

## 状态与字段

JSON `format=1`、`scope=experimental_engine`、`trusted_discovery=false`。顶层 `availability` 区分：

| 值 | 含义 |
| --- | --- |
| observed | 读到格式一致的快照，并观察到根进程及对应创建时间的发布进程尚未退出 |
| unavailable | 没有映射、未开启诊断、访问被拒绝或进程不可读；Win32 数字保留原因 |
| inconsistent | 尚未发布或三次有界采样都碰到写入，不等待或改写数据 |
| invalid | 版本、字段、保留字节、目标创建时间或时间范围无效 |
| process_exited | 根进程对象已退出；可能附带尚未被启动方轮询更新的旧快照 |
| publisher_unavailable | 发布进程已退出、PID 已复用或无法观察；附带内容仍只是旧快照 |

`snapshot=null` 表示没有可解析记录。取得有效记录后即使发现进程退出，也可保留记录供对照；必须同时看 availability，不能单独读取旧 `state=ready` 就报告正常。

快照只包含：

- 根进程、发布进程、查询宿主的 PID／创建 FILETIME，以及是否为独立所有者模式。尚未取得宿主对象时 host 为零。
- 进程状态 starting／ready／stopping／terminating／exited、固定失败类别、Win32 数字和可选退出码。
- `deployment_required`／`deployment_verified` 表示启动方是否配置[部署校验](ENGINE_DEPLOYMENT.md)、本次 Ready 前是否通过初始映像核对；可显示 deployment_identity 故障。这仍是自报观察，不是客户端可用的信任依据或内存完整性检查。v1 包 flags 增加两个已知位，旧读取器遇到开启策略的记录会拒绝，应配套构建。
- 端点配置上限、启动方累计领取数量。这个累计数不含其他客户端自行分配，也不是当前连接数。
- 私有 control／admission 的 disabled／idle／pending／ready／failed、固定失败类别和 Win32 数字。
- 接入槽的初始配置、累计交出／创建／撤销、当前有效和已绑定进程数量，口径见[动态接入](ENGINE_ADMISSION.md)。这些是启动方已观察的数据，不是所有者内部槽位的同步实时查询。
- `last_inspect`：启动方通过原控制接口自行领取的最后一份成功 Inspect 的代次、上限、监听／连接／待回收、任务计数及观察时间。尚未领取时为 null。客户端自己的 Inspect 不进入此缓存；Allocate 也不刷新它。各计数可能早已改变。

`published_tick_ms` 是最近发布的 GetTickCount64；`polled_tick_ms` 是启动方最近调用 poll 的时间，尚未轮询时为零／`poll_age_ms=null`。两者有各自的 age_ms，Inspect 另有独立年龄。数字时间只作本机观察，不与引擎 steady_clock 期限混用。正常调用 poll 会重新发布，即使状态不变；绑定／撤销等操作也会发布，但不伪造一次新 poll。空闲等待不会主动刷新，冻结的 I/O 线程也可能保留 ready，不能把进程存在或旧记录当作健康心跳。

## 格式、权限与寿命

每个启用诊断的 EngineProcess 创建一个 264 字节分页文件映射：8 字节序号／保留头，以及 256 字节 HKDS v1 固定包。字段按固定宽度小端编码，尾部保留字节必须为零；读取先校验版本、枚举、计数范围、字段关系及时间。写入使用奇偶序号，读取最多尝试三次，并检查复制前后序号，不返回写入中的半份记录。

名称为 `Local\Harukoto.EngineDiagnostic.v1.<根PID>.<根创建FILETIME>`。创建时拒绝已存在对象，绝不接管其内容；映射创建／发布失败只在 `status().diagnosticsWin32` 记录错误并停止发布，不终止引擎或改变原控制／查询结果。对象不继承给子进程，受保护 DACL 只给当前登录 SID 的 SECTION_MAP_READ／READ_CONTROL；工具以 FILE_MAP_READ 打开和映射，不授予写入能力。与原私有能力通道一样，不宣称隔离拥有同一用户权限的恶意进程或管理员。

映射中没有管道名称、原始句柄、组字／输入、候选正文、文档路径或个人词库内容，不落地诊断状态文件。关闭 EngineProcess 后发布方释放映射，其他读者若仍持有对象可以暂时保留旧数据；根进程退出状态及创建时间检查防止将这种残留当成新实例。

**诊断名称、PID、创建时间以及记录自报都不提供可信发现。** 工具按 PID 打开受限进程对象仅用于查看；查询客户端仍须经原可信启动链取得保留进程对象和私有能力，不能据诊断字段自动附着、复制句柄或执行身份认证。

## 验证与剩余工作

`engine_diagnostics` 覆盖固定包、未知版本／枚举／保留值、错误创建时间、发布进程身份变化、只读权限、对象冲突、奇数序号、跨进程命令行及缺失状态。独立发布进程连续切换关联字段，读取只返回一致快照或明确的写入中；冻结发布进程的输入等待后，连续读取只增加年龄，不改变原发布时间。命令行分别测试文本和 JSON、非法参数及实际进程退出。

`engine_owner_process`／`engine_owner_full` 在真实所有者／宿主上开启诊断，检查启动／Ready、绑定／动态补充／撤销、真实中文选词、未领取 Inspect 不被读取器消费、领取后缓存容量、正常停止／崩溃、未 poll 的退出与读者持有旧映射。关闭诊断时不创建映射。基准源码指纹包含该头文件，但既有静态基准默认不开诊断，不代表发布开销已量化。

后续仍须可信发现与竞争失败交接、正式常驻生命周期、分层用户界面和所有者／调度性能测量；然后进行 TSF、组字边界换代与回退。本批 Windows 0.4.4-alpha.22、FF14 0.3.0 的安装和发行基线保持。
