# 可信发行元数据来源

WIN-03 阶段二第 1 步，2026-11-21。[部署校验](ENGINE_DEPLOYMENT.md)和[接收方核对](ENGINE_VERIFIED_PEER.md)要求预期程序路径与 SHA-256 来自调用方已信任的配置；本步确定这份配置由谁写、存在哪里、谁能改、接入方怎么读。实际输入路径不变，安装包仍只用进程内路径，TSF 尚未读取它。

## 存储位置与选择理由

`HKLM\SOFTWARE\Harukoto\EngineReleases`（64 位视图）。每个已安装版本一个 REG_MULTI_SZ 值，值名为版本目录名（安装器按文件哈希生成的 `Program Files\Meowpuut\IME\<版本>`）。

| 方案 | 结论 |
| --- | --- |
| HKLM 注册表 | 采用。只有管理员／SYSTEM 能写；普通用户、低完整性进程和 AppContainer（ALL APPLICATION PACKAGES）可读；单个值的写入是原子的，一个版本一个值，没有半写状态；多个版本并存、按版本增删都只动各自的值；不受 WOW64 重定向影响（读写都用 64 位视图） |
| 版本目录内清单文件 | 不采用作信任根。旧 TIP 只知道自己的目录，找不到新安装的版本（第 7 步换代需要）；文件与被核对的程序同目录，正是部署文档禁止的“候选同目录下的清单”；目录 ACL 依赖继承，难以统一核对 |
| 两者并用 | 不需要：程序文件本身已由 EngineDeployment 按批准哈希固定和核对，再加一份文件清单不增加信任，只多一个不一致来源 |

安装器不依赖继承，显式设置该键及父键 `SOFTWARE\Harukoto` 的描述符：所有者 Administrators；SYSTEM、Administrators 完全控制；Users、ALL APPLICATION PACKAGES 及 LPAC 的 registryRead 能力只读；不继承上级。真实 `HKLM\SOFTWARE` 含一条对对象本身生效的 CREATOR OWNER 完全控制，这个 SID 不在任何令牌中，读取方按“不授予任何人”处理。

## 格式

每个值固定 8 行、固定顺序，不允许多余或缺少的行：

```
format=harukoto-engine-release/1
id=<版本目录名，与值名相同>
sequence=<安装序号，十进制，1..2^53-1，无前导零>
protocol=<引擎协议版本，1..65535，对应 EngineProtocol.h 的 ProtocolVersion>
host=<绝对路径>\harukoto_engine_host.exe
host-sha256=<64 位小写十六进制>
owner=<绝对路径>\harukoto_engine_owner.exe
owner-sha256=<64 位小写十六进制>
```

- id 限 1–240 个 `[0-9A-Za-z._-]`，首字符为字母或数字。路径只接受 `X:\...` 形式，反斜杠分隔，拒绝相对路径、UNC、`\\?\`、`.`／`..`、空段、末尾点或空格、流名及控制字符，长度 ≤ 1024。两个程序名固定，必须在同一目录，且目录名等于 id。
- 上限：最多 8 个值（超过则整个来源无效）、单值 ≤ 8 KiB、≤ 16 行、值名 ≤ 240 字符。
- 首行为 `format=harukoto-engine-release/<其他数字>` 的值视为新版安装器写的未知格式，跳过（计为 unsupported），不影响其他值；首行不是该前缀则作为损坏处理。
- 多版本并存用于升级窗口。选择规则：协议号与接入方完全相同的条目中取 sequence 最大者；最大值并列视为异常，不批准任何一个。

## 写入方：安装器

`Windows/InstallSupport.ps1` 的 `Publish-HarukotoEngineRelease`／`Unpublish-HarukotoEngineRelease`，只在提权阶段调用。

- 安装／升级：`install-ime.ps1 -Register` 在 `Invoke-MeowpuutRegistration` 成功之后，若版本目录内两个引擎程序都存在，就从**已复制到 Program Files 的文件**计算哈希，写入 sequence＝现有最大值＋1 的条目，写后读回逐行比较。注册失败会在此之前返回并恢复旧注册，元数据保持原样。写入失败只警告、不回滚注册（引擎尚未被使用）；第 6 步接入 TSF 后再决定是否改为安装失败。
- 同时清理：去掉格式损坏或 host 程序已不存在的条目，其余按 sequence 只保留最新 4 个（含本次）。重新安装旧包会得到新的最大 sequence，因而成为选中版本，这就是回退方式。
- 卸载：`uninstall-ime.ps1 -Unregister` 在机器注册移除之后删除整个键，空的 `SOFTWARE\Harukoto` 一并删除。版本文件照旧保留，但不再有任何批准的引擎。
- 打包：`package-ime.ps1 -MSVC` 把 `harukoto_engine_host.exe`、`harukoto_engine_owner.exe` 放入 payload，安装器把它们当作可选文件复制进版本目录（计入版本目录名哈希）。输入服务不会启动它们。alpha.29 及更早的包没有这两个文件，升级后只打印“No engine programs”，键不创建。

## 读取方：接入方

`Windows/EngineRelease.h`（仅头文件，依赖 advapi32），命名空间 `harukoto::engine::release`：

```cpp
const auto snapshot = release::read();              // 默认 HKLM、要求保护
const auto approved = release::select(snapshot, harukoto::engine::ProtocolVersion);
if (!approved) { /* 留在进程内路径 */ }
auto host  = std::make_shared<EngineDeployment>(approved->host,  approved->hostSha256);
auto owner = std::make_shared<EngineDeployment>(approved->owner, approved->ownerSha256);
```

- `read()` 依次：以 KEY_QUERY_VALUE｜READ_CONTROL｜KEY_WOW64_64KEY 打开；检查该键及每个上级键（`SOFTWARE`、`SOFTWARE\Harukoto`）的所有者和 DACL；数量与大小上限；逐值校验类型、REG_MULTI_SZ 终止符和格式；对通过的条目再检查版本目录及其上级（不含卷根）不是重解析点、只有可信主体能写，防止在批准的程序旁边放 DLL 或替换目录。
- “可信主体”只有 SYSTEM、Administrators、TrustedInstaller。所有者必须是其中之一；任何生效的允许 ACE 若授予写入类权限（注册表：设置值、建子键、建链接、删除、改 DACL／所有者、GENERIC_WRITE／ALL；目录：建文件／子目录、写扩展属性／属性、删子项及同上通用位）而 SID 不在其中，即视为不受保护。仅继承 ACE 跳过；拒绝 ACE 不扩大权限，忽略；带条件或对象类型的允许 ACE 只要含写入位就拒绝；NULL DACL 拒绝。
- 结果 `Snapshot`：`Ready`（附有效条目及 rejected／unsupported 计数）、`Missing`（键不存在）、`Unprotected`（键或上级可被他人写）、`Invalid`（参数、枚举错误或条目过多）。只有 Ready 的有效条目可被 `select` 选中；其余情况一律不批准，接入方留在进程内路径，不从其他地方补值。单个损坏条目只排除它自己，其余管理员写入的条目仍可用。
- `Source{root, subkey, requireProtected}` 仅供测试注入 HKCU 子键；正式代码使用默认值。接入方不得读取 HKCU、环境变量、诊断映射、引擎自报值或候选目录中的文件作为批准值。
- 所有调用都是可能阻塞的系统调用，只能在服务／I/O 线程运行，不能放进 TSF 前台回调。每次发现或换代时重新读取，不缓存为长期信任。

## 威胁模型

- 能写：管理员、SYSTEM、TrustedInstaller。管理员本来就能替换输入法 DLL，不在防护范围内。
- 同一登录会话的普通进程（含恶意程序）：可以读该键；不能改键或在 Program Files 版本目录写文件；可以伪造公开诊断、创建同名管道或自报任意哈希，但这些都不是批准来源。它能做的是在自己的 HKCU 或临时目录放假清单——接入方不读这些位置。它可以终止或占用自己能访问的引擎进程，造成拒绝服务；结果是回退到进程内路径。
- 低完整性和 AppContainer 进程：按默认 ACL 可以读，能否读到以安装实测为准；LPAC 依赖安装器加入的 registryRead 能力项。读不到就是 Missing／Invalid，留在进程内路径。
- 不在范围内：管理员或内核级攻击者、离线改盘、Authenticode 签名与发行签名（当前包未签名，元数据信任来自 ACL 而不是签名）、旧版本程序本身的漏洞（被保留的旧版本只要哈希匹配仍可被批准，直到下次安装清理或卸载）。
- 本步不证明某个运行中的进程就是批准的引擎；那是第 2 步发现握手的工作，元数据只提供“应当是哪个文件”。

## 后续步骤如何使用

- 第 2 步发现握手：接入方先 `read()`／`select()` 得到批准路径与哈希，用 EngineDeployment 固定文件，再对握手中取得的真实进程对象调用 `verify`；公开控制对象、PID、管道名只作定位，不作身份。普通权限、管理员、沙箱三类宿主分别实测读取与握手。
- 第 3 步交接：竞争失败的一方用同一份批准条目核对已存在的引擎，不信任对方自报的路径或版本。
- 第 6 步接入 TSF：开关开启且读取结果为 Ready 并选中条目时才尝试引擎，其余情况（包括本机从未安装引擎程序）一律进程内路径；读取在后台线程完成。
- 第 7 步换代：新安装写入更高 sequence，旧 TIP 在组字边界重新读取，若协议号相同即可切到新引擎；协议号不同则提示重开应用。旧条目保留到被清理，供仍在使用旧引擎的连接继续核对。

## 验证

- `engine_release`（C++）：严格解析（行数／顺序／前缀、id 与值名、序号与协议边界、大写或长度错误的哈希、各类非法路径、程序名与目录关系、内嵌 NUL）、REG_MULTI_SZ 解码边界、SDDL 构造的描述符判定（含安装器实际使用的描述符、CREATOR OWNER、OWNER RIGHTS、条件 ACE、NULL／缺失 DACL）、真实对象（System32、Program Files、HKLM\SOFTWARE 为受保护；临时目录、HKCU\Software 不受保护；本机正式键只读检查）、HKCU 测试键上的升级窗口、损坏值单独排除、超大值、超长值名、未知格式跳过、并列不批准、超过 8 个值整体无效、要求保护时用户可写键为 Unprotected。
- `engine_release_install`（PowerShell）：安装器协议号与 EngineProtocol.h 一致、正式 SDDL 在 C++ 测试中被覆盖、安装／卸载调用顺序；在 HKCU 测试键上发布后由 C++ 读取器解析核对路径与哈希、显式描述符生效、升级并存、重装旧版成为选中、失败发布不改动已有条目、损坏与已删除版本被清理、数量上限、卸载删除及重复卸载。
- 未做：真实提权安装写 HKLM、低完整性／AppContainer／LPAC 进程实际读取、管理员权限宿主——均待安装实测。
