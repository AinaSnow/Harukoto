# 运行中创建和补充客户端接入槽

WIN-03b，2026-11-11。`Windows/EngineAdmission.h` 提供可信启动方到[独立所有者](ENGINE_OWNER.md)的私有接入通道。它在运行中创建新的[客户端接入能力](ENGINE_REGISTRATION.md)，退出或主动撤销后可继续补充，每个所有者最多同时保留 8 个有效槽。每次使用新的内核对象和递增编号，不复用旧客户端的能力。本接口仍是 C++ 开发接口，没有安装、注册、自动启动或接入 TSF；Windows 0.4.4-alpha.22、FF14 0.3.0 保持。

## 调用方式

```cpp
EngineProcess::Config config{hostExecutable, base, themes, personal};
config.dynamicEndpoints = true;
config.ownerExecutable = ownerExecutable;
config.dynamicRegistrations = true; // 默认关闭，只允许独立所有者模式。
config.ownerRegistrations = 0;      // 仍可预分配 0～8 个，与动态槽共用上限。
EngineProcess process(config);
// 合并 waitHandles()／deadline()，poll 至 Ready。
process.requestRegistration(pipe::now());
// 持续等待／poll；status().admissionState 到 Ready 后只领取一次。
auto result = process.takeRegistrationResult(pipe::now());
if (result && result->code == admission::Code::Ok) {
    auto registration = std::move(result->registration);
    // 按 ENGINE_REGISTRATION.md 显式交接给独立客户端，
    // 再用 bindRegistration(registrationId, trustedClientProcess) 绑定寿命。
}
```

takeRegistration 继续只领取启动时预分配的槽；动态槽通过 requestRegistration／takeRegistrationResult 领取。动态通道和查询端点 Allocate／Inspect 的控制通道相互独立，Pending 或尚未领取的 Ready 占用动态通道，不能再提交。返回 Ok／Full／Closed；只有 Ok 携带 EngineRegistration，Full 表示 8 个位置均在用，Closed 表示已撤销。Full 后可由调用方在容量释放后明确提交另一份新申请，不自动排队或重试。

新编号在此所有者寿命内单调递增，失败申请也消耗编号，不要求连续；耗尽 uint64 编号后拒绝新申请，不回绕。旧编号不再绑定或撤销新客户端。预分配槽尚未交出时也占容量。动态槽可像原预分配槽一样自行 Allocate／Inspect、绑定进程寿命、主动撤销及导出受限句柄；不会交出启动方的动态接入通道、stop、Job 或进程复制权限。

## 私有交接与句柄所有权

可信启动方创建未命名的 256 字节映射和两个手动复位事件，经显式继承列表交给自己挂起创建并加入 Job 的所有者。CLI 可选 `--admission-section`／`--admission-request`／`--admission-response` 必须全部提供且不与其他启动／注册句柄重复；临时副本在恢复线程前关闭，所有者接管后清除继承标记。这些句柄不交给查询宿主或普通客户端。

每次申请时，启动方创建一组新的客户端控制映射、请求／回复事件及撤销事件。它使用持续保留的所有者 CreateProcess 对象作为 DuplicateHandle 的目标，把四个非继承副本复制进该实际进程，再将四个目标进程内句柄值和新编号写入私有接入请求。没有公开 PID 查找，也不从文件、注册表或普通应用的自报接受这些数字。

发布请求之前，Transfer 守卫独占新复制的目标句柄；复制中途或发布失败，会关闭已经复制的部分。请求事件成功触发即转移所有权，此后只有所有者接管并关闭目标副本。启动方即使超时也不能按旧数字使用 DUPLICATE_CLOSE_SOURCE 回收，因为所有者可能已关闭并复用了那个数字槽。源码中的 release() 是所有权转移，不能改成超时后再次关闭远端值。

所有者读取申请后用 RAII 接管四个副本，判定容量前再次观察撤销，避免沿用收到申请前的旧容量状态。Full／Closed 当场释放，Ok 安装到空闲位置；撤销后关闭控制端口，新的申请可以使用同一个内部位置，但使用全新的对象与编号。已有宿主请求只保留原位置、注册编号与请求副本；返回时必须仍匹配该编号才可写回，因此替换位置不会收到旧回复。动态接入事件在宿主请求在途时也参与等待，不依赖查询完成来补槽。

## 格式、期限与失败

格式使用小端整数、`HKRG` 魔数、版本 1，剩余字节必须为零：

| 字节偏移 | 请求 | 回复 |
| --- | --- | --- |
| 0／4／6 | 魔数 4 字节／版本 uint16／类型 1 uint16 | 相同，类型 2 |
| 8／16 | uint64 序号／注册编号 | 精确匹配请求序号／编号 |
| 24 | 四个 uint64 目标句柄：section、request、response、revoked | uint32 结果：1 Ok、2 Full、3 Closed |
| 56～255／28～255 | 请求剩余部分为零 | 回复剩余部分为零 |

序号严格递增一，编号严格增加；拒绝零、非法句柄范围、重复句柄、与本接入通道别名、坏类型／版本／保留字节。接入层不携带输入或候选，原查询／Allocate／Inspect 格式不变。这是可信监督通道，损坏的申请按所有者整体失败处理，不开放给任意应用。

requestRegistration 默认期限 5 秒，可设 1～300,000 毫秒，使用 pipe::now()。EngineProcess.waitHandles 合并接入回复事件，deadline 取进程管理、普通控制与接入三者最近期限。精确到期先于回复，超时或坏回复永久停用本接入通道，保留首个失败，不重放也不自行重建；原所有者控制与查询连接仍可继续使用。

停止、实际所有者退出或接入失败会触发尚未交出槽的撤销事件，Pending／未领取 Ready 都不能再领取能力。若已复制的副本尚未被所有者读取，它们至多是一份在途申请的四个句柄，随后由所有者消费／拒绝或在进程实际退出时由系统回收。接入通道失败后禁止继续申请，不能积累更多悬挂转移。所有者按原路径停止并等待宿主；启动方仍持有 kill-on-close Job，这不是脱离启动方的常驻服务。

## 容量与状态口径

所有者最多 8 个有效客户端槽；启动方最多保留 8 个有效槽加 1 份单槽申请，退役记录在下一次申请／领取时清除，只保留累计计数。旧客户端持有的受限事件和映射可以继续存在，但不能重新激活端口；不强制结束应用或已有查询连接。宿主查询端点容量及未连接 Listener 的 30 秒过期仍独立管理。

| status 字段 | 含义 |
| --- | --- |
| registrationConfigured | 启动时预分配数量，不含动态申请 |
| registrationCreated | 累计创建编号数，含失败或未交出的申请 |
| registrationIssued | 累计成功交给调用方的槽数 |
| registrationActive | 启动方当前保留的未退役已接纳槽，含未发出的预分配槽；不含尚未领取的动态申请 |
| registrationBound | 当前保留的客户端进程对象数 |
| registrationRevoked | 累计在启动方退役的槽／申请，包括 Full、超时或停止时未交出的申请 |
| admissionState／admissionFailure／admissionWin32 | 私有动态接入通道状态、首个错误及系统错误 |

这些值随 poll 刷新；不能把 Issued、Revoked、Active 简单相减，也不是独立实时诊断服务。对照查询端点容量仍用健康通道的 Inspect。

## 验证与剩余范围

engine_admission 验证严格格式、序号／编号关联、单槽 Pending／Ready、精确期限、坏回复／首错，以及发布前远端副本回收、部分复制失败和发布后由接收方关闭。engine_owner_process／full 使用微型／全量基础词库＋FF14，覆盖预分配与动态槽混合、8 槽满后 Full、连续 24 轮撤销补充、旧句柄隔离与所有者句柄数不增长；原客户端保留组字并继续选日语候选。

真实独立客户端分别通过动态创建的能力自行请求查询端点，覆盖正常退出后第三个客户端接入、简繁／假名／FF14、部分选词和重复选择拒绝。事件屏障固定旧请求在途，在它返回前撤销并替换对应位置，新客户端仅接收第二次查询结果。另覆盖动态槽进程绑定退出、接入 Pending／Ready 时停止和崩溃、精确超时后原控制仍可用。

可信发现、竞争失败后的交接、正式常驻生命周期、面向用户的只读诊断、TSF 和版本换代仍待完成。普通应用不能自行获取接入通道。既有性能报告测的是静态直接宿主，不代表本路径的补槽／调度成本；基准源码指纹加入 EngineAdmission.h 以保持可追溯性。

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_admission|engine_process|engine_owner_process|engine_owner_full)$' --output-on-failure
```
