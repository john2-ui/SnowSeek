# SnowSeek 分阶段开发规划

## 1. 规划原则

开发顺序以“先形成可验证闭环，再增加资源约束和可靠性”为原则。每个阶段都必须
具备明确输入、可运行产物、自动化测试和退出条件；性能结论只记录实测数据。

预计完整 MVP 为 M1～M5。M6、M7 属于增强阶段，不阻塞首次发布。
当前实现已完成 M3、M4 的有界并行构建和 M5：Segment v2、多 Segment 装载、
`update`、Tombstone、Glob 删除、显式/自动压缩，以及 writer 锁、持久化原子提交与
启动恢复。0.2.0 进一步将公开 C++ 边界收敛为 `index.hpp`、`search.hpp` 和
`version.hpp`，并停止读取 Segment v1/无 Manifest 索引。自定义临时目录与 AArch64
实测继续暂缓。

## 2. 阶段总览

| 阶段 | 目标 | 主要交付物 | 当前状态 |
|---|---|---|---|
| M0 | 工程基线 | CMake、模块骨架、CI、本机与交叉编译 | 已完成 |
| M1 | 内存检索闭环 | 扫描、Token 化、内存倒排索引、单词/AND 查询 | 已完成 |
| M2 | 持久化索引 | 版本化格式、词典、文档表、Posting、校验 | 已完成 |
| M3 | 完整查询能力 | 布尔/短语查询、过滤、BM25、Top-K、片段 | 已完成 |
| M4 | 有界内存构建 | 临时 Segment、K 路归并、资源档位、ARM64 | 核心完成，设备实测待补 |
| M5 | 增量与可靠性 | Manifest、Tombstone、Compaction、原子提交 | `0.2.0` 已完成 |
| M6 | 性能工程 | 压缩、Skip、查询规划、mmap/pread、Benchmark | 规划中 |
| M7 | 代码结构搜索 | C/C++ 轻量 Lexer、symbol/comment 字段 | 规划中 |

## 3. M0：工程基线

### 工作项

- 已将公开头文件固定为 `snowseek/index.hpp`、`snowseek/search.hpp` 和
  `snowseek/version.hpp`，AST、内存索引与存储协议保持私有；
- 配置 GCC/Clang 的警告选项和 Release/Debug 构建；
- 建立无第三方依赖的测试程序与 CTest 入口；
- 验证 x86_64 构建，准备 AArch64、ARMv7 Toolchain；
- 定义格式化、提交和版本管理约定；
- 建立小型英文、源码、日志和异常 UTF-8 测试语料。

### 退出条件

- `cmake`、构建、CTest 全部通过；
- `snowseek --help` 和 `snowseek --version` 可运行；
- 公共模块不依赖 CLI；
- CI 至少覆盖 GCC 和 Clang。

## 4. M1：单机内存索引

### 工作项

- 递归扫描目录，处理权限错误、符号链接和文件大小上限；
- 分块读取普通文本文件；
- 实现 ASCII 标识符 Tokenizer、位置记录和大小写归一化；
- 建立内存词典、文档表和 Posting List；
- 实现单词查询及两个 Posting List 的 AND 求交；
- 打通 `index`、`query` 两个 CLI 命令的临时内存模式。

### 退出条件

- 对固定语料的查询结果和位置完全正确；
- 非法 UTF-8、空文件、超长 Token 不崩溃；
- Posting 中 DocumentId 和 Position 严格递增；
- 有 Tokenizer 和 Posting 运算基准数据。

## 5. M2：持久化索引

### 工作项

- 先编写 `docs/index-format.md`，再实现磁盘编码；
- 定义 Magic、版本、Feature Flags、Offset、Length 和字节序；
- 实现 Documents、Paths、Terms、Postings、Positions 区域；
- 实现区域 Checksum；
- 增加 `stats`、`verify` 命令；
- 对所有磁盘输入执行溢出和边界检查。

### 退出条件

- 进程退出后可快速重新打开并得到相同结果；
- 截断、Magic 错误、版本错误和校验错误均被拒绝；
- 同一输入重复构建得到逻辑一致的索引；
- x86_64 生成的索引可由 AArch64 读取。

## 6. M3：查询与排序

### 工作项

- 编写查询 Lexer、递归下降 Parser 和表达式 AST；
- 支持括号、AND、OR、NOT 和双引号短语；
- 实现 `path:`、`extension:` 过滤；
- 按文档频率重排 AND 求交顺序；
- 实现 BM25、固定容量 Top-K 和评分解释；
- 只为 Top-K 读取原文并生成带行号片段；
- 支持文本和 JSONL 输出。

### 退出条件

- 运算符优先级、括号和错误消息有完整测试；
- 短语查询使用 Position 验证，而非仅做词项 AND；
- 排名固定、可解释且不受遍历顺序影响；
- 查询深度、表达式长度和 Top-K 均有上限。

## 7. M4：有界内存与嵌入式适配

### 工作项

- 已将当前构建内存估算拆分到元数据、读取、Token、词典和 Posting；
- 已按默认 128 MiB 容量阈值刷写不可变临时 Segment，并定义失败清理规则；
- 已实现默认 fan-in 16 的多级 K 路归并、临时空间硬预算和磁盘不足诊断；
- 已提供 Minimal、Balanced、Performance 配置；
- 已支持 `--memory-limit`、`--threads` 和无 Position 的 Minimal 构建；
- 自定义临时目录暂缓；
- AArch64 设备上的 RSS、吞吐和写入量实测暂缓。

### 退出条件

- 在固定内存预算下索引明显大于内存的数据集；
- 峰值 RSS 不显著突破配置预算，额外开销有文档说明；
- Minimal 模式可单线程运行且可关闭 Position；
- 磁盘空间不足时失败可诊断，且不会发布半成品索引。

## 8. M5：增量索引与可靠性

### 工作项

- 已定义多 Segment Manifest v1、Segment v2 和单调递增 SegmentId；
- 已按路径、大小、纳秒修改时间及原始内容 CRC32C 识别变化；
- 已使用 Tombstone 表示删除和旧版本失效；
- 已实现 `update`、`remove`、`compact` 及 16 Segment 软阈值自动压缩；
- 已使用目录锁、临时文件、`fsync`、目录同步和原子 `rename` 完成可靠全量提交；
- 已在 writer 启动时识别并清理未提交工作区、Manifest 临时文件和孤儿 Segment；
- 已覆盖每个发布观察点的进程中断恢复和并发 writer 测试；
- 已按最新路径记录构造跨 Segment 可见性映射，并复用现有查询与 BM25 路径。

### 退出条件

- 新增、修改、删除后的结果等价于全量重建；
- 更新操作可安全重复执行；
- 在提交过程的任意故障点退出，目录可解析为完整旧 generation 或完整新 generation；
- Compaction 前后查询结果一致；
- 以上条件已纳入 `0.2.0`。

## 9. M6：性能工程

### 工作项

- 建立固定数据集与冷/热缓存 Benchmark 流程；
- 对 DocId 和 Position 使用 Delta + Varint；
- 为长 Posting 添加 Skip 信息或 Galloping Search；
- 比较排序数组词典、缓存策略、`pread` 与 `mmap`；
- 减少热路径分配，必要时引入 Query Arena；
- 记录索引体积、构建吞吐、P50/P95/P99 和写放大。

### 退出条件

- 每项优化都有变更前后的可复现实测；
- 不以明显增加内存或破坏可靠性换取未说明的性能；
- 性能回归测试可以独立运行并导出机器可读结果。

## 10. M7：可选代码结构搜索

### 工作项

- 实现不做宏展开的轻量 C/C++ Lexer；
- 区分标识符、注释、字符串、类型和简单函数声明；
- 增加 `symbol:`、`comment:` 等字段及对应权重；
- 保证普通文本索引路径不依赖语言分析器。

### 退出条件

- 对典型 C/C++ 文件提供比纯文本搜索更精确的字段结果；
- 遇到不完整代码、宏和未知语法时可以安全降级；
- 不宣称提供完整 AST 或编译器语义。

## 11. 每阶段统一完成标准

每个里程碑合并前应同时满足：

1. 新功能具有单元或集成测试；
2. 错误路径和资源上限得到测试；
3. Debug 与 Release 构建通过，编译器警告清零；
4. 用户可见行为同步到 README 或对应设计文档；
5. 若涉及性能，提交可复现的基线和环境信息；
6. 若涉及磁盘格式，说明兼容性和版本升级策略。

## 12. 推荐近期任务顺序

完成 M5 后建议依次推进：

1. 增加自定义临时目录并定义跨文件系统发布规则；
2. 在 AArch64 环境验证峰值 RSS、索引兼容性和查询结果；
3. 进入 M6，先建立增量更新与压缩的可复现基准；
4. 评估 Delta/Varint 与查询 I/O 优化。
