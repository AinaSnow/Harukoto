# mimo 固定繁简转换数据

四个平台编译 mimo 时使用相同的转换快照；一般构建只需要 Python，不需要 ICU、Foundation 或网络。快照以 `t2s`／`s2t`、原文和结果为一笔 TSV，gzip 保存。程序会核对 `SOURCE.json` 的压缩档、解压数据与人工修订 SHA-256；格式、校验值错误或来源中出现未收录的转换键时会停止构建，不会改用系统转换器。

快照依固定上游词库的实际转换需求产生，并不是完整的繁简转换词典。产生时使用 ICU 78.2 的 `Traditional-Simplified` 正向与反向，数据来自 ICU 所附的 CLDR `Hans_Hant.txt`；该文件保留 Apple Inc.、Unicode Inc. 及其他贡献者的版权声明。`ICU-LICENSE.txt` 为上游 Unicode License V3 原文，原始规则、版本、URL 和 SHA-256 见 `SOURCE.json`。`reviewed-overrides.tsv` 另列喵扑审核过的 11 笔字形，不修改原始 ICU 规则。

## 更新来源时

1. 在更新后的来源上构建 ICU **78.2**，确认上游来源与授权；普通用户不需要此步。以 pkg-config 可用的环境为例：

   ```sh
   c++ -std=c++17 -O2 scripts/FreezeConversions.cpp $(pkg-config --cflags --libs icu-i18n) -o /tmp/meowpuut-freeze-78
   /tmp/meowpuut-freeze-78 --version
   ```

   输出必须是 `ICU 78.2`。请勿以系统其他 ICU 版本或其他方向的转换替代。
2. 以不存在的文件名产生候选快照。工具不会覆盖已审核数据，也不需网络：

   ```sh
   python3 scripts/update-normalization-snapshot.py --converter /tmp/meowpuut-freeze-78 --output /tmp/mimo-next.tsv.gz
   python3 scripts/compare-normalization.py DataSources/MimoNormalization/conversions-icu78.2.tsv.gz /tmp/mimo-next.tsv.gz > /tmp/mimo-conversion-changes.tsv
   ```

3. 逐项检查增减和变更，重新审核可能需要修改的 `reviewed-overrides.tsv`，保留 ICU 的原始版权及授权。更新快照后同步修改 `SOURCE.json` 中文件名、压缩及解压数据 SHA-256 与生成器版本数据。不同 zlib 版本的压缩结果可能不同，请以解压数据 SHA-256 判断内容；不可只改哈希而跳过字形审核。
   如果上游移除某个人工覆盖的键，或新规则已与覆盖一致，工具会保留候选快照供比较、列出需审核的覆盖并以非零状态结束。确认后更新覆盖与 `SOURCE.json` 的覆盖校验值，再以新文件名重新产生候选。
4. 执行 `python3 scripts/test-frozen-normalization.py --integration`，再跑词库、单字与长句回归；确认拼音、注音、繁简及所有平台的词库内容。

一般词库修订仍放在 `Resources/Lexicon/`。固定转换并不能解决所有一简对多繁的语义歧义，请以词条覆盖和回归案例处理。
