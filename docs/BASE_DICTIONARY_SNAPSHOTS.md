# 基础词库版本快照与热更新

适用 Windows 0.4.4-alpha.20。工具需要 Python 3.11 或更高版本，仅使用标准库。通用 TSF 输入服务读取当前用户的 %LOCALAPPDATA%/Meowpuut/BaseDictionaries/STATE.json；所有加载新版接入层的应用均使用同一机制，各自加载和切换。工具输出 runtime_activation_confirmed: false，表示工具未确认各应用的实际生效状态，不表示发布不会生效。首次升级接入层仍需重开输入应用。

## 整包内容

四个文件必须来自同一次已完成的构建，创建期间停止写入构建目录：

| 文件 | 用途 |
| --- | --- |
| native-pinyin.tsv | 中文原生拼音与组词 |
| language-evidence.sqlite | 中文查询与语言证据 |
| japanese.tsv | 基础日语触发词 |
| japanese-conjugations.bin | 日语活用表 |

create 只复制上述四项并生成 base-dictionary.json；主题、个人词表、DLL 和渲染器不在其中。清单包含 format=1、engine_abi=1、最多 128 字符的语义版本号和每项文件的字节数／SHA256；重复 JSON 字段被拒绝。当前 ABI 号是数据加载协议约定，不是 DLL 二进制 ABI，也未接入运行时协商。

verify 检查精确文件集合、大小上限（每项 512 MiB）、哈希及已知容器头。它不全面解析词条或执行 SQLite 完整性检查；哈希不能证明来源可信，也不能证明四个文件在语义上相互配套。创建快照不替代构建和原生回归测试。

## 操作示例

在仓库根目录使用 PowerShell 7。路径均显式指定，开发验证推荐独立目录；目标快照必须不存在。版本号表示词库构建版本，不自动改变安装器版本。以下 alpha19 是示例词库版本，新版引擎可加载兼容格式的旧快照。

```powershell
python -X utf8 Windows/manage-base-dictionary.py create .build/windows/ime .build/base-snapshots/alpha19 --version 0.4.4-alpha.19
python -X utf8 Windows/manage-base-dictionary.py verify .build/base-snapshots/alpha19
python -X utf8 Windows/manage-base-dictionary.py stage .build/base-snapshots/alpha19 --store .build/base-store
python -X utf8 Windows/manage-base-dictionary.py status --store .build/base-store
```

下次完成词库构建后，用新的目标路径和版本执行 create，再 stage 到同一个 store。暂存过两个不同快照后才能回退：

```powershell
python -X utf8 Windows/manage-base-dictionary.py rollback --store .build/base-store
```

store 以清单规范化 JSON 的 SHA256 命名子目录；STATE.json 的 current 和 previous 指向当前暂存与上一快照。重复 stage 同一内容不改变 previous；rollback 交换两者，再次执行可切回。相同文件但不同版本号会获得不同标识。status 检查两个指针指向的文件，不仅打印版本号。

## 失败与恢复边界

- 先复制到临时目录并重新校验，再发布完整目录，最后原子替换 STATE.json。复制失败或校验失败保留旧指针。指针替换失败可能留下一个未被引用的完整目录，重试可复用；工具暂不自动清理旧快照。
- 每个 store 使用独占 .write.lock 阻止多个工具同时写入。正常错误会释放自己的锁，不移除其他调用的锁。进程强制终止可能留下锁和临时目录：先确认没有管理命令运行，再人工备份并移除该 store 中的残留锁，重新执行 status。不得按时间猜测锁已失效并自动删除。
- rollback 是两个完整有效版本间的切换。current、previous 或 STATE.json 损坏时拒绝写入；不会猜测可用版本或自动修复。可用 verify 检查保留的独立快照，再 stage 到一个新的开发 store，原损坏目录保留供诊断。
- 文件 flush／fsync 后才替换指针，但不承诺所有文件系统下断电事务安全。此工具用于受信任、本地、独占管理的开发目录；不提供远程来源签名验证或抵御目录联接／恶意并发修改的安全边界。

## 原生整包加载检查

Windows CMake 构建新增 harukoto_base_dictionary_check.exe。它使用与输入法相同的中文核心、查询接口、日语和活用解析器，将四份数据加载为一个 Snapshot 对象；所有检查成功才返回。可在创建或暂存后单独运行，不修改系统注册：

~~~powershell
python -X utf8 Windows/manage-base-dictionary.py verify .build/base-snapshots/alpha19 --native-loader .build/windows/msvc/Release/harukoto_base_dictionary_check.exe
& .build/windows/msvc/Release/harukoto_base_dictionary_check.exe --store .build/base-store
~~~

原生检查器重新校验清单、规范化内容标识、四文件大小与 SHA256，加载原生拼音、逐行验证基础日语、加载活用表并执行 SQLite schema／integrity_check。基础日语允许三列普通记录及六列显式触发记录，拒绝被宽松解析器跳过的坏行、非法别名和 Unicode。它不验证自然语言翻译准确率或跨文件词条语义一致性。

SQLite 必须是关闭写入并使用普通回滚日志格式的独立数据库；拒绝 WAL 格式及 -wal／-shm／-journal 边车文件。活用表原生上限为 4 MiB，严格于通用存储层的 512 MiB。原生加载期间持有清单与成员文件的只读句柄，拒绝写入、删除或替换；多个只读加载器可同时使用。对象销毁时先关闭 SQLite，再释放文件句柄。不要对已加载快照原地修补，应发布新目录。

检查器的 --store 读取 current、检查目录与清单标识，并在准备完成后复查指针；中途变更会失败，调用者可重试。独立检查器不会后台自动重试、启动监控、修改 STATE.json 或激活 TSF。检查器结果 native_loaded=true 只代表原生加载通过，runtime_activated 仍为 false；管理工具外层则使用 runtime_activation_confirmed 字段。

Snapshot 与 SQLite 查询语句只供单个消费线程使用；后台准备完成后通过原子完成标记交接给 TSF 线程，不能同时查询。主题／个人表与新基础表在同一后台任务中合并，并和 Snapshot 同步发布。原生核心会话可独立保留其词库，但使用日语、活用和 SQLite 引用的代码仍须持有 Snapshot，不能只留裸引用。

## 发布到正在使用的输入法

先完成 create 和原生 verify，再将已验证快照发布到当前用户的运行时仓库。以下命令会影响所有已加载 alpha.20 的输入应用，不限定 QQ，也不需要管理员权限：

~~~powershell
$baseStore = Join-Path $env:LOCALAPPDATA 'Meowpuut/BaseDictionaries'
python -X utf8 Windows/manage-base-dictionary.py stage .build/base-snapshots/alpha19 --store $baseStore
python -X utf8 Windows/manage-base-dictionary.py status --store $baseStore
# 发布过两个有效版本后可回退
python -X utf8 Windows/manage-base-dictionary.py rollback --store $baseStore
~~~

输入服务在实际可处理按键时至多每秒启动一次后台检查，空闲时不持续轮询。正在组字、活用面板开启或任一挂起会话还有引擎未取走的已选文字时不切换；完成或取消后，在下一次实际按键前发布准备好的版本。若第一段仍是旧候选，完成或取消后再输入。各应用独立生效，不保证同一时刻切换，也不保证发布后的第一个按键立即使用新版。

切换前先创建全部活动／挂起上下文的替代会话，保留各自繁简模式及片段上下文；全部成功后才交换。已经进入 TSF 上屏队列的文字、队列状态和原字段归属均保持。基础、主题或个人表任一失败都保留当前整套结果，后续新组字提示“词库更新失败，沿用旧版”，之后继续重试。有效空个人表仍表示显式清空；删除基础 STATE.json 不表示恢复出厂词库，已加载实例会保留内存旧版并报更新失败。

启动时先使用安装目录内的基础词库，之后按上述流程读取用户仓库；已发布的用户基础快照会覆盖安装包基础词库，因此后续升级安装包也应检查用户仓库版本。重启后遇到坏用户快照会继续用安装目录版本，内存旧版不是跨进程备份。通过运行时仓库更新无需替换宿主 DLL；引擎代码和 TSF 接入层更新仍需重开应用。

MSVC 安装包／ZIP 根目录附带 harukoto_base_dictionary_check.exe。下一个试用包起（DICT-03）包内不再附带 Windows/manage-base-dictionary.py 与 scripts/base_dictionary.py，发布基础快照请在源码仓库执行上述命令；alpha.34 及更早的 ZIP 仍附带。工具没有图形入口。安装器不会自动发布或覆盖用户基础快照。

## 验证与下一步

存储测试：Windows/tests/test_base_dictionary.py。覆盖重复暂存、回退、缺失／同大小损坏、格式和版本拒绝、复制期间变化、指针替换失败、写锁和坏状态保留、CLI 退出码。

生产测试：Windows/tests/test_base_dictionary_production.py，由 CTest 传入全量词库目录、原生集成测试与候选探针。创建两个版本、暂存和回退后执行 SQLite integrity_check、原生加载与选词回归、简繁候选检查及坏更新拒绝。小型存储夹具不代表真实词库语义验证。

原生加载器与开发检查入口已实现；test_base_dictionary_native.py 覆盖 Python／C++ 内容标识一致、格式拒绝、有效哈希下的坏内容、SQLite schema／WAL、只读文件占用释放及不同代次并存，生产测试也通过新 Snapshot 对象查询简繁候选。

alpha.20 已接入通用 TSF 后台加载和空组字边界切换。service_dictionary_reload 使用两个独立进程运行生产服务代码，覆盖输入中更新、活动／挂起已选文字、队列身份、繁简保持、失焦、主题／个人表一致性、坏更新恢复与回退；service_dictionary_reload_full 使用真实全量词库。自动测试不替代记事本、浏览器、VS Code、QQ、FF14 和远程环境的实际 TSF 投递验收。下一步补各应用生效版本诊断、性能测量及独立引擎方案。
