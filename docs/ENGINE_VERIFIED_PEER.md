# 客户端独立核对部署身份

WIN-03b，2026-11-11。`Windows/EngineVerifiedPeer.h` 将[部署文件固定与实际映像核对](ENGINE_DEPLOYMENT.md)用于客户端接收能力的边界。接收方根据自己信任的预期部署，检查私有交接的真实进程对象；查询连接自己保留文件保护。它不查找公共 PID，不实现公开发现或竞争失败后的自动附着。安装与发行保持 Windows 0.4.4-alpha.22、FF14 0.3.0。

## 接收可信进程对象

```cpp
auto approvedHost = std::make_shared<EngineDeployment>(
    trustedMetadata.hostPath, trustedMetadata.hostSha256);
EngineVerifiedPeer peer(approvedHost, privatelyHandedOffHostProcess);
auto channel = peer.connect(candidatePipeName);
// 在原 I/O 线程继续 Hello／ClientGate 握手；然后处理输入和选择。
```

预期路径和哈希必须来自接收方已经信任的配置。进程参数必须是可信创建或私有交接时保留的真实对象；不能用 `OpenProcess(公开PID)` 代替，再把文件校验称为完整发现认证。管道名只是定位提示，即使格式正确也不提供身份。

构造时拒绝空部署策略、线程模拟令牌、错误对象、已退出进程、不同登录身份以及映像不匹配。内部只复制不可继承的 PROCESS_QUERY_LIMITED_INFORMATION／SYNCHRONIZE 副本，不持有 terminate、duplicate、Job 或所有者控制权限。

connect 先核对保留对象的实际映像与存活，再通过原 Channel 绑定同一个对象和操作系统返回的管道服务进程身份，最后再次核对存活与映像。检查失败时关闭本次连接，不发送 Hello、输入或控制请求。无等待管道、循环重试、发现其他实例或请求重放；原查询协议、Kind 和截止时间规则保持。

EngineDeployment 首次构造时固定并计算文件哈希；连接时复核实际映像和文件身份，不重复整文件哈希。这些仍是可能阻塞的系统调用，只能在服务／I/O 线程运行，不能放进 TSF 前台回调。

## 接收注册能力

```cpp
EngineRegistration::DeploymentPolicy policy{
    std::make_shared<EngineDeployment>(trustedMetadata.ownerPath,
                                       trustedMetadata.ownerSha256),
    std::make_shared<EngineDeployment>(trustedMetadata.hostPath,
                                       trustedMetadata.hostSha256)
};
EngineRegistration registration(std::move(privateControlHandles),
    privatelyHandedOffOwner, privatelyHandedOffHost,
    privatelyHandedOffRevocation, registrationId, std::move(policy));

registration.request(control::Operation::Allocate, pipe::now());
// 合并等待并 poll，领取结果后：
auto result = registration.take(pipe::now());
if (result && result->endpoint) {
    auto channel = result->endpoint->connect();
    // 继续原查询握手。
}
```

策略默认为空，保持原开发调用。启用时 owner／engine 必须同时提供，而且是不同的实际文件；依次核对两个保留进程对象。只给一半、同文件或任一映像不符都在第一条注册请求之前拒绝，失败构造不发起 Allocate／Inspect，不修改新控制槽中的请求内容。`deploymentVerified()` 只表示初次接收时完成双程序核对，进程退出或请求失败后的历史值仍为 true，不是持续健康检查。

EngineProcess 启用 hostSha256／ownerSha256 后，预分配及动态返回的 EngineRegistration 自动保留这对策略；静态及动态 Endpoint 也携带宿主策略。`claim()` 和 `Endpoint::connect()` 复用校验路径。跨进程传递 C++ 指针没有意义；新进程必须从自己的可信配置构造策略，并单独接收私有控制与进程对象，真实测试客户端按此方式执行。

Endpoint::connect 是携带策略的连接入口；手动拆出 name／process 再调用旧 Channel::connect 只执行原进程对象认证，不会替调用者保留部署策略。没有配置策略时，新方法沿用原连接行为。

## 文件保护的寿命

部署保护由只读共享对象持有。注册、未连接端点、VerifiedPeer 以及已连接 Channel 可分别保留它；释放启动方或注册对象，不会让存续的连接丢掉保护。Channel 在取消并回收原管道 I/O、关闭管道和服务进程副本后才释放部署对象。进程实际退出也不会提前解除文件／父目录固定，最后一个引用释放后才允许更新对应目录。

共享引用只延长文件保护，不延长进程执行寿命。原启动方关闭 Job 仍会回收所有者与宿主，不因此变为正式常驻服务。注册撤销、超时、实际进程退出与已有查询连接仍遵循[受控接入](ENGINE_REGISTRATION.md)的原规则。

## 验证与尚未完成的部分

- `engine_verified_peer` 使用临时复制的真实进程，覆盖同内容不同文件拒绝、错误对象、空／单边策略、owner／engine 任一不匹配、同文件配对、线程模拟拒绝、受限且不可继承的进程副本，以及正确映像不能认证另一个进程创建的管道。
- 真实管道完成 Hello／HelloAck 与有载荷往返；错误导入不发布控制请求，成功注册只领取一次结果，默认无策略连接继续工作。宿主退出后不能重连；注册与进程管理对象释放后，两条连接继续固定目录，分别销毁直到最后一份保护释放才可改名和删除。
- `engine_owner_process`／`engine_owner_full` 的部署策略用例让三个独立客户端自己核对双程序，再申请端点并完成简繁、假名、FF14、部分与重复选择回归；父方另检查预分配与动态策略传播、撤销和正常退出。

这项能力验证批准的 EXE 和保留对象，仍不证明任意公开控制映射属于那个进程。控制／撤销对象必须经原可信私有链交接；批准值来源见[可信发行元数据](ENGINE_RELEASE.md)（ACL 保护的 HKLM 键，未签名）；没有公开发现挑战握手、竞争失败接手、正式常驻或 TSF 接入。未验证不同账户／会话实机矩阵，也不提供同用户恶意程序、所有 DLL 或进程内存完整性隔离。原静态性能报告不包含新接收与连接校验成本。

在仓库根目录使用 PowerShell 7：

```powershell
cmake --build .build/windows/msvc --config Release
ctest --test-dir .build/windows/msvc -C Release -R '^(engine_verified_peer|engine_deployment|engine_pipe|engine_process|engine_owner_process|engine_owner_full)$' --output-on-failure
```
