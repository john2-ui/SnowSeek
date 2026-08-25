# SnowSeek

SnowSeek 是一个面向嵌入式 Linux 的零第三方运行依赖本地全文检索引擎，使用
C++20 实现。项目当前已完成 M3 查询与排序闭环，以及 M4 的构建内存统计、逻辑内存
预算、并行解析、按预算刷写和多级归并。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)
./build/snowseek --help
```

构建并查询单文件 v1 索引：

```bash
./build/snowseek index ./testdata --index ./snowseek-index
./build/snowseek index ./testdata --index ./snowseek-index \
  --temporary-space-limit 4GiB --merge-fan-in 16
./build/snowseek index ./testdata --index ./snowseek-index \
  --profile minimal --memory-limit 128MiB --threads 1
./build/snowseek query ./snowseek-index timeout --source ./testdata
./build/snowseek query ./snowseek-index '"timeout retry"' --source ./testdata
./build/snowseek query ./snowseek-index \
  '(timeout OR retry) AND extension:txt' --top-k 10 --jsonl --explain
./build/snowseek stats ./snowseek-index
./build/snowseek verify ./snowseek-index
```

索引写入 `segment-0000000000000001.idx`，保存相对源目录的文档路径。若个别文件
无法读取或分析，成功文档仍会发布，但 `index` 返回状态码 2 并输出诊断。
`IndexBuilder` 默认使用 Balanced：256 MiB 分类内存预算、2 个解析线程、128 MiB
批次目标、fan-in 16 并保留 Position。Minimal 使用 128 MiB、单线程、32 MiB 批次、
fan-in 4 且关闭 Position；Performance 使用 1 GiB、硬件线程数、512 MiB 批次和
fan-in 32。显式 `--memory-limit`、`--threads`、`--merge-fan-in` 可覆盖档位。

构建在目标目录的私有工作区刷写临时 Segment，再多级归并为上述单文件。临时空间
默认不限额；
`--temporary-space-limit` 接受字节或 `B`、`KiB`、`MiB`、`GiB`、`TiB`，按输入
Segment 总大小保守预检 spool 与候选文件的最坏空间，可能早于实际磁盘耗尽而拒绝。
`index` 输出 `memory_peak_bytes`、`temporary_peak_bytes`、线程、Position 状态和归并
层数。逻辑内存预算不统计 allocator、线程栈、运行库、内核页和 writer 序列化缓冲，
因此不是 RSS 配额；临时峰值也不等同于文件系统块配额。任意预算、持久化或校验失败
都不会替换已有索引，工作区会被清理。无 Position 索引仍支持词项、布尔和 BM25，
短语查询会明确报错。

查询表达式支持括号、大小写不敏感的 `AND`、`OR`、`NOT`、双引号精确短语、
`path:` Glob 和 `extension:` 精确扩展名过滤。相邻词项不会隐式连接，必须显式写
`AND`。默认输出相对路径、BM25 分数以及可用的原文行号和片段；`--source <dir>`
指定原语料根目录，`--paths-only` 保留每行一个路径的输出，`--jsonl` 输出结构化
结果，`--explain` 增加逐词评分贡献。默认 Top-K 为 20，上限为 1000。

测试不依赖第三方框架，并且在 Debug 和 Release 构建中都会执行显式检查。若需要将
编译器警告视为错误，可在配置时增加 `-DSNOWSEEK_WARNINGS_AS_ERRORS=ON`。

一键执行 GCC/Clang 的 Debug/Release 编译与测试矩阵：

```bash
./tools/test-matrix.sh
```

可通过 `SNOWSEEK_BUILD_JOBS` 调整并行任务数，通过 `SNOWSEEK_BUILD_ROOT`
指定构建产物目录。

可选的确定性索引基准、参数和当前实测见
[docs/memory-baseline.md](docs/memory-baseline.md)。

## 目录

- `include/snowseek/`：公开接口，按领域模块划分；
- `src/`：核心实现与 CLI；
- `tests/`：无第三方测试框架的单元和集成测试；
- `benchmarks/`：可选性能基准；
- `cmake/toolchains/`：嵌入式 Linux 交叉编译模板；
- `docs/`：架构、格式和开发规划文档；
- `tools/`：索引检查、数据集生成等辅助工具；
- `testdata/`：小型、可版本控制的测试语料。

详细实施顺序见 [docs/ROADMAP.md](docs/ROADMAP.md)。
