# SnowSeek

SnowSeek 是一个面向嵌入式 Linux 的零第三方运行依赖本地全文检索引擎，使用
C++20 实现。项目当前已完成 M3 查询与排序闭环、M4 有界并行构建，以及 M5 的
多 Segment 增量更新、删除、压缩和 POSIX 可靠发布。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)
./build/snowseek --help
```

构建、维护并查询由 Manifest 激活的 Segment v2 索引：

```bash
./build/snowseek index ./testdata --index ./snowseek-index
./build/snowseek index ./testdata --index ./snowseek-index \
  --temporary-space-limit 4GiB --merge-fan-in 16
./build/snowseek index ./testdata --index ./snowseek-index \
  --temporary-directory /mnt/snowseek-tmp
./build/snowseek index ./testdata --index ./snowseek-index \
  --profile minimal --memory-limit 128MiB --threads 1
./build/snowseek update ./testdata --index ./snowseek-index --threads 2
./build/snowseek remove ./snowseek-index --path '*.log' --path 'cache/**'
./build/snowseek compact ./snowseek-index
./build/snowseek query ./snowseek-index timeout --source ./testdata
./build/snowseek query ./snowseek-index '"timeout retry"' --source ./testdata
./build/snowseek query ./snowseek-index \
  '(timeout OR retry) AND extension:txt' --top-k 10 --jsonl --explain
./build/snowseek stats ./snowseek-index
./build/snowseek verify ./snowseek-index
```

首次构建写入 `segment-0000000000000001.idx` 和 `MANIFEST`；`update` 为新增、修改和
缺失路径追加 delta Segment，`remove` 追加与大小写敏感 POSIX Glob 匹配的 Tombstone，
`compact` 将当前可见 live 文档重写为一个 Segment。完全未变化或未匹配时为 no-op，
不消耗 generation 或 SegmentId。活动 Segment 超过 16 个时会尝试自动压缩；自动压缩
失败仍提交 delta、返回状态码 2 并输出维护诊断。被删除但仍在源目录中的文件会被后续
`update` 重新加入。

Segment 保存相对源目录的文档路径、纳秒 mtime 和原始内容 CRC32C。CRC 仅用于普通
变化检测，不是安全哈希。0.2 的读取命令只接受 Segment v2 + Manifest v1；Segment
v1 或缺少 `MANIFEST` 的旧目录会明确要求重新构建。`index` 是安全迁移路径：新 v2
Segment 和 Manifest 持久化后才清理旧固定 Segment，提交点前失败仍保留旧文件。若
个别文件无法读取或分析，成功文档仍会发布，但 `index` 返回状态码 2 并输出诊断。
`IndexWriter` 默认使用 Balanced：256 MiB 分类内存预算、2 个解析线程、128 MiB
批次目标、fan-in 16 并保留 Position。Minimal 使用 128 MiB、单线程、32 MiB 批次、
fan-in 4 且关闭 Position；Performance 使用 1 GiB、硬件线程数、512 MiB 批次和
fan-in 32。显式 `--memory-limit`、`--threads`、`--merge-fan-in` 可覆盖档位。

构建默认在目标目录的私有工作区刷写临时 Segment，再多级归并为一个候选 Segment。
`--temporary-directory <dir>` 可将 `index`、`update`、`remove` 和 `compact` 的工作区
放入一个已存在目录。显式配置后，最终候选始终先复制回索引目录的
`.snowseek-segment-*` 暂存文件，再以同文件系统 rename 发布；因此临时目录可以位于
另一文件系统。临时空间默认不限额；
`--temporary-space-limit` 接受字节或 `B`、`KiB`、`MiB`、`GiB`、`TiB`，按输入
Segment 总大小保守预检 spool 与候选文件的最坏空间。使用外部临时目录时，逻辑峰值
还包括“外部候选 + 索引目录候选副本 + Manifest”的重叠窗口，通常至少接近最终
Segment 大小的两倍；构建文件检查临时文件系统，发布副本则单独检查索引文件系统，
任一检查都可能早于实际磁盘耗尽而拒绝。
维护命令按固定顺序输出 `outcome`、`revision`、`segments`，再输出命令相关计数：
`index` 为 `indexed`、`failed`，`update` 为 `added`、`modified`、`removed`、
`unchanged`、`failed`，`remove` 为 `matched`，`compact` 为
`discarded_records`。最后依次输出 `memory_peak_bytes`、
`temporary_peak_bytes`、`warning_count`。诊断继续写入 stderr；成功、带警告和致命
错误的退出码分别为 0、2、1。查询、`stats` 和 `verify` 输出保持不变。

逻辑内存预算不统计 allocator、线程栈、运行库、内核页和 writer 序列化缓冲，
因此不是 RSS 配额；临时峰值也不等同于文件系统块配额。任意预算、持久化或校验失败
都不会在 Manifest 提交点前替换已有索引，工作区会被清理。提交后的旧 Segment 清理
失败不会回滚新 generation：CLI 返回状态码 2，下一次 writer 会重试清理。写入使用
目录级 `flock`、文件/目录 `fsync` 和同目录原子 rename；查询无锁读取一个完整
generation。下一次 writer 会清理索引目录内的 `.snowseek-segment-*` 发布残留；
进程崩溃遗留在共享外部临时目录的 `.snowseek-build-*` 不会自动跨索引清理，以免删除
其他 writer 的工作区。无 Position 索引仍支持词项、布尔和 BM25，短语查询会明确
报错。

查询表达式支持括号、大小写不敏感的 `AND`、`OR`、`NOT`、双引号精确短语、
`path:` Glob 和 `extension:` 精确扩展名过滤。相邻词项不会隐式连接，必须显式写
`AND`。默认输出相对路径、BM25 分数以及可用的原文行号和片段；`--source <dir>`
指定原语料根目录，`--paths-only` 保留每行一个路径的输出，`--jsonl` 输出结构化
结果，`--explain` 增加逐词评分贡献。默认 Top-K 为 20，上限为 1000。

## C++ API 0.2

0.2 有意收窄并破坏旧 C++ API。公开头文件只有
`snowseek/index.hpp`、`snowseek/search.hpp` 和 `snowseek/version.hpp`：

| 旧入口 | 0.2 入口 |
|---|---|
| `snowseek::index::IndexBuilder` | 绑定目录与资源选项的 `snowseek::IndexWriter`；调用 `rebuild`、`update`、`remove`、`compact` |
| `snowseek::query::QueryEngine` | 使用 PImpl 隐藏加载结构的 `snowseek::Searcher` |
| `snowseek::storage` 类型与校验函数 | `snowseek::validate_index(path) -> IndexStats` |
| `snowseek::query::SearchResult` 的行号/空字符串约定 | `SearchHit::snippet` 的 `optional<SourceSnippet>` |
| `snowseek/common/version.hpp` | `snowseek/version.hpp`，版本值为 `0.2.0` |

AST、Scanner、Tokenizer、DocumentStore、内存倒排结构、BM25、codec 和 Manifest
均为 `src` 内部实现，不再通过公开头间接暴露。`IndexOptions` 只公开资源档位及
`memory_limit_bytes`、`temporary_space_limit_bytes`、`temporary_directory`、
`worker_threads`、`merge_fan_in` 五个可选覆盖项。`IndexResult` 将结果分为
`IndexOutcome`、revision/活动 Segment、`ChangeCounts`、精简 `BuildMetrics` 和按
`DiagnosticStage` 标记的统一诊断列表。`IndexStats` 的 `documents`、`terms`、
`postings`、`positions` 是可见逻辑计数，`bytes`、`segments`、`tombstones` 是活动
Segment 的物理计数。现有 Segment v2 + Manifest v1 字节保持兼容。

测试不依赖第三方框架，并且在 Debug 和 Release 构建中都会执行显式检查。若需要将
编译器警告视为错误，可在配置时增加 `-DSNOWSEEK_WARNINGS_AS_ERRORS=ON`。

一键执行 GCC/Clang 的 Debug/Release 编译与测试矩阵：

```bash
./tools/test-matrix.sh
```

可通过 `SNOWSEEK_BUILD_JOBS` 调整并行任务数，通过 `SNOWSEEK_BUILD_ROOT`
指定构建产物目录。

Linux 上可启用统一维护基准，测量完整构建、增量更新和压缩的冷/热文件缓存延迟、
吞吐、索引体积和写放大：

```bash
cmake -S . -B build-benchmark -DCMAKE_BUILD_TYPE=Release \
  -DSNOWSEEK_BUILD_BENCHMARKS=ON
cmake --build build-benchmark --parallel 2
./build-benchmark/benchmarks/snowseek_index_builder_benchmark
./build-benchmark/benchmarks/snowseek_index_builder_benchmark --samples 100
```

缓存定义、统计口径、参数和当前实测见
[docs/memory-baseline.md](docs/memory-baseline.md)。
Manifest v1 与 Segment v2 的统一磁盘契约见
[docs/index-format.md](docs/index-format.md)。

## 目录

- `include/snowseek/`：三个稳定的 0.2 公开头文件；
- `src/`：核心实现与 CLI；
- `tests/`：无第三方测试框架的单元和集成测试；
- `benchmarks/`：可选性能基准；
- `cmake/toolchains/`：嵌入式 Linux 交叉编译模板；
- `docs/`：架构、格式、资源基线和后续计划；
- `tools/`：索引检查、数据集生成等辅助工具；
- `testdata/`：小型、可版本控制的测试语料。

项目结构与后续 TODO 见 [docs/architecture.md](docs/architecture.md)。
