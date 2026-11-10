# 本地主题词库管理

适用：Windows 0.4.4-alpha.19。图形窗口与命令行均支持多包导入、独立启停、更新和上一版回退。管理工具需要 Python 3.10 或更新版本，图形窗口还需要 tkinter；安装器内置的 PowerShell 不包含 Python。日常输入不需要 Python。

## 图形窗口

安装后从开始菜单打开“晴词主题词库”，或在输入设置中点击“管理主题词库”。ZIP 用户双击 Dictionaries.cmd。首次启动选择本机 python.exe，入口会检查版本及 tkinter，成功后记住 Python 路径。不会自动下载 Python。

1. 点击“选择词库并预览”，选择主题包的 manifest.json。
2. 查看来源、许可、条目数和跨主题冲突，再点击“导入此预览”。预览后文件变化时拒绝导入，需重新预览。
3. 在主题列表选中该包，点击“启用”或“关闭”；有旧版时可以“回退上一版”。
4. alpha.18 起，修改会在实际输入时后台加载，并在当前组字结束后的新一段输入生效。若第一段还未变化，完成或按 Esc 取消，再开始输入。首次安装新版 DLL 仍需重开应用。

文件处理在后台执行，操作期间按钮暂时禁用；失败会显示原因并恢复可操作状态。来源文本仅供查看，不自动打开链接。开发者也可执行 `python Windows/dictionary-manager.pyw` 启动。

## 直接试用示例包

在仓库根目录或解压后的 ZIP 根目录执行。以下是 PowerShell 7 命令：

```powershell
python Windows/manage-japanese-packages.py preview Resources/DictionaryPackages/example/manifest.json
python Windows/manage-japanese-packages.py import Resources/DictionaryPackages/example/manifest.json
python Windows/manage-japanese-packages.py enable harukoto-example
python Windows/manage-japanese-packages.py list
```

主题后台加载后开始新一段输入，输入 kafeidian，可以分别选“喫茶店”或“きっさてん”。新主题导入后默认关闭，必须显式启用；已有主题更新保留启用状态。基础词库仍可提供相同词条，所以关闭主题不会保证每个同名候选都消失。

```powershell
python Windows/manage-japanese-packages.py disable harukoto-example
```

上述操作无需管理员权限。命令行全局参数 `--root C:/path/to/test-store` 可以指定隔离目录，适合开发和测试；这种目录不会自动被当前输入法加载。

## 创建自己的主题包

一个目录放两份 UTF-8 文件：manifest.json 和 words.txt。示例清单：

```json
{
  "format": 1,
  "id": "my-japanese-terms",
  "name": "我的日语主题",
  "version": "1.0.0",
  "source": "自行整理；在这里记录来源链接与核对日期",
  "license": "在这里填写作者许可或适用的使用条件",
  "words": "words.txt"
}
```

```text
# 我的日语主题
喫茶店 | きっさてん | pinyin: kafeidian | english: cafe
```

- ID 是更新和启停使用的稳定标识，以小写字母开头，只含小写字母、数字、短横线，最多 64 字符。
- version 为 1–64 个字母、数字、点、加号或短横线；工具不比较版本大小，同一 ID 的不同版本均可导入，同版本不同内容会拒绝。
- source 与 license 必须填写，但工具不能代替来源核对或授权判断。
- words 只能引用同目录的 .txt 文件，不接受外部路径。词表行格式沿用[现有自定义词表](JAPANESE_INPUT_ROADMAP.md)。
- 最多 128 个包；所有包当前与回退版本的编译记录合计最多 500000 条；状态与启用词表各最多 256 MB。

## 预览、冲突、更新与回退

preview 校验格式，输出清单、编译记录数和已导入主题包之间的冲突，不写入用户目录。相同触发词的不同表记／读音是 alternative，同表记同读音是 duplicate；报告也包括关闭的主题并显示其启用状态，最多列出 100 项。基础词库与旧 japanese-custom.tsv 暂不参与冲突报告。

冲突不会覆盖其他包。启用后不同词义共同参与候选排序，完全相同的编译行合并。多个同形候选的包来源尚未在候选窗标注，导入前请先看预览。

更新：修改同一 ID 的 version 和词表，再执行 import。当前版本会成为可回退版本，每个包保留一个逻辑回退版本。

```powershell
python Windows/manage-japanese-packages.py import C:/my-package/manifest.json
python Windows/manage-japanese-packages.py rollback my-japanese-terms
```

回退交换当前与上一版，保留启用状态，其他主题包及旧自定义词表不变；再次 rollback 可以切回。alpha.18 起，主题修改后台加载后在新一段输入生效，旧版仍需重开应用。

## 存储与失败处理

默认目录：`%LOCALAPPDATA%/Meowpuut/Dictionaries`。每次写入在随机快照目录生成 state.json 和 enabled.tsv，完成后原子替换 CURRENT 指针；C++ 输入服务仅在启动时加载指针指定的 enabled.tsv。导入失败或指针替换失败时继续使用旧指针。写操作持有操作系统文件锁，进程异常退出后由系统释放，不需要手动清除锁文件。

快照不可变，旧快照暂不自动清理，可能占用额外磁盘空间。该机制用于避免部分写入，不能替代断电恢复或文件系统备份。不要手动改写快照文件；备份时保存整个 Dictionaries 目录。缺失、超过限制或格式损坏的运行时词表会被忽略，基础中文输入仍能加载。

旧导入入口 Windows/import-japanese-dictionary.py 及 japanese-custom.tsv 继续单独工作。alpha.19 起个人表也支持后台加载后在新组字生效；主题管理不会覆盖它。个人导入是整表替换，`--clear` 发布空个人表，不影响主题包。意外删除／损坏文件会保留当前进程已加载的旧版，需恢复有效词表或显式清空；不是磁盘备份。卸载保持保留用户数据的行为。

## FF14 接入边界

当前随附 [FF14 职业与物品术语包 0.3.0](FF14_FULL_DICTIONARY.md)：57,672 条来源记录，共 113,463 组触发词，涵盖职业、道具、货币和制作采集。点击管理窗口“预览 FF14 词库”，导入后手动启用，后台加载后在新一段输入生效（alpha.18 起）。已定义[FF14 词条审核规范](FF14_TERMS.md)及校验／导出工具，记录词条 ID、分类、国服／日服名、玩家别名、版本与来源，只导出 verified 词条。已知专有名词辅助输入不等于任意游戏对话翻译。

alpha.9 起名称最长 128 字、触发最长 256 字母；每个源词表最多 32 MB、100000 行，编译后按触发展开。显式填写 `-` 表示没有可靠假名，只输出表记；不能用于活用词。省略汉字读音仍报错，不会自动猜读音。FF14 全量包必须在 alpha.9 或更新版本导入。
