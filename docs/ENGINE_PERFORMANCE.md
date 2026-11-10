# 实验引擎逐键性能测量

WIN-03b／CN-02，2026-11-10。新增 harukoto_engine_benchmark 与 scripts/benchmark-engine.py，测量[Windows 合并查询](ENGINE_WINDOWS.md)每一次实际按键的核心重建、SQLite 打开、候选合并和[单工作线程](ENGINE_EXECUTOR.md)往返，不把此前“中间按键返回空候选”的功能测试当逐键性能。

## 运行方法

使用已配置完整词库的 MSVC Release 构建，然后通过 Python 运行。整个过程在临时目录制作基础快照并导入 FF14，不改安装、注册或用户词库：

```powershell
python -X utf8 scripts/benchmark-engine.py --reader .build/windows/msvc/Release/harukoto_engine_benchmark.exe --lexicon .build/windows/ime --runs 3 --output .build/windows/engine-performance.json
```

先基础场景、再 FF14 全量开启；每个场景先 direct、再 executor，各为独立进程。direct 在服务端所属线程直接计算，executor 使用一个工作线程和一个槽位；均逐个发键并等待该键完整回复，不模拟快速连续输入、多连接竞争或管道 I/O。每次请求都经过 ServerConnection 接受／完成校验，executor 另含提交、线程唤醒、查询包复验和结果领取。

固定样本为 nihao、xinkule、riben、eat、nihaoz；FF14 场景增加 tejihuifuyao。每轮基础 26 键、FF14 38 键，三轮四组共 384 个逐键样本、66 次实际选择。每段输入最后验证“你好／辛苦了／日本／食べる／特级恢复药”，nihaoz 只消费 nihao、保留 z；重复 Select 必须返回原回复而不再消费。选词和重复请求验证在计时区间之外。

## 记录与解释

查询分阶段计时是 WindowsDictionary.query 的可选参数；常规查询不读这些阶段时钟，也不改变候选计算和顺序。失败查询不输出成功样本。字段均为毫秒：

| 字段 | 包含内容 |
| --- | --- |
| setup_ms | 输入检查、创建独立核心会话、设置简繁与片段上下文 |
| replay_ms | 从空会话逐字恢复完整输入；每次 mpn_key 可能重新解码前缀 |
| sqlite_open_ms | 加载系统 SQLite、只读打开数据库、检测表结构与准备查询 |
| merge_ms | 首轮正式 Windows 候选合并，含 SQLite 查词与来源处理 |
| expansion_ms | 至多三轮核心候选扩展与正式追加合并 |
| conversion_ms | 整理候选 ID／正文／注释、按页截取及完整包校验 |
| cleanup_ms | queryImpl 返回后的剩余耗时，主要为局部对象／SQLite／核心会话释放，也含计时包装开销 |
| total_ms | 完整查询调用，含清理 |
| request_ms | 从服务端接受当前 Key 前到完成回复后，含查询和相应直调／后台调度 |

cleanup_ms 是总时长减去各显式阶段后的余量，并非单独挂钩析构采样。其余阶段时钟也有开销；不应用微小差值判断优化。request_ms 不包括外部键盘事件、真实 IPC、TSF 写锁、宿主插入、候选窗口绘制。

每组记录全部原始样本及中位／最大／合计。不同长度和输入类型混在组内，组中位数只用于总览；应同时按 case、key 和 run 查原始数据。少量轮次不报告 P95／P99。load_ms 单独记录完整词库准备，包含清单／SHA256／SQLite 完整性校验和合并复制，不混入逐键请求。

报告保存 Git 提交与 source_dirty、相关源码和测量程序 SHA256、基础四文件大小／SHA256、FF14 清单和词表哈希、系统版本。结束复核程序、源码和词库未改变。只使用固定测试文本，不记录用户输入、机器名、用户路径或安装状态。报告脚本严格拒绝缺键、重复样本、缺选择验证、非有限／负数时间和阶段合计不一致；失败不写新的成功报告。

## 验证和后续

engine_benchmark 使用微型基础词库配合真实 FF14 主题，跑四个进程／128 次实际逐键查询和 22 次选择验证，同时检查源文件不变、哈希、汇总与坏报告拒绝。初次测试发现 Windows 文本写入将固定 LF 容器头转为 CRLF；测试制作器已显式保持 LF，修复后通过。完整构建与 engine_executor、engine_windows_full、engine_benchmark 三项相关测试通过；未为本批重复运行全部 38 项。

工具和正确性检查先落地，随后从干净源码提交运行完整词库基准并保存报告。根据重建／SQLite／合并／扩展各阶段占比决定下一次优化，不能先假定 SQLite 连接池收益。当前未调整解码预算或缓存，不宣称延迟、内存已有改善；冷缓存、并发会话、真实应用和多进程共享收益仍需单独测量。
