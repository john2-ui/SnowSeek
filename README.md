# SnowSeek

## 项目简介

[English](README_EN.md)

SnowSeek 是一个使用 C++20 编写、面向嵌入式 Linux 的本地全文检索引擎，无第三方
运行依赖。它支持完整构建、增量更新、删除、压缩、布尔与短语查询、BM25 排序，以及
基于不可变 Segment 和 Manifest 的可靠发布。

## 构建方式

需要 Linux、CMake 3.16 或更高版本，以及支持 C++20 的 GCC 或 Clang。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
./build/snowseek --version
```

## 使用方式

直接使用构建产物：

```bash
./build/snowseek index ./testdata --index ./snowseek-index
./build/snowseek update ./testdata --index ./snowseek-index
./build/snowseek remove ./snowseek-index --path '*.log'
./build/snowseek compact ./snowseek-index
./build/snowseek query ./snowseek-index \
  '(timeout OR retry) AND extension:txt' --top-k 10
./build/snowseek stats ./snowseek-index
./build/snowseek verify ./snowseek-index
```

安装到当前机器后，可以直接使用 `snowseek`：

```bash
./tools/install.sh
snowseek --help
./tools/uninstall.sh
```

安装目录默认为 `/usr/local/bin`，权限不足时脚本会调用 `sudo`。可通过
`SNOWSEEK_BUILD_DIR` 指定构建目录，通过 `SNOWSEEK_INSTALL_DIR` 指定安装目录：

```bash
mkdir -p "$HOME/.local/bin"
SNOWSEEK_INSTALL_DIR="$HOME/.local/bin" ./tools/install.sh
SNOWSEEK_INSTALL_DIR="$HOME/.local/bin" ./tools/uninstall.sh
```

自定义目录需要已经加入 `PATH`，才能省略可执行文件路径。

## 命令行参数

### 命令

| 命令 | 必须参数 | 示例 | 含义 |
|---|---|---|---|
| `index <source> --index <dir>` | 是 | `snowseek index ./docs --index ./index` | 完整构建或重建索引。 |
| `update <source> --index <dir>` | 是 | `snowseek update ./docs --index ./index` | 为新增、修改和删除的路径发布增量 Segment。 |
| `remove <index> --path <glob>` | 是 | `snowseek remove ./index --path '*.log'` | 使用大小写敏感的 POSIX Glob 发布 Tombstone；`--path` 可重复。 |
| `compact <index>` | 是 | `snowseek compact ./index` | 将当前可见文档压缩为一个 Segment。 |
| `query <index> <expression>` | 是 | `snowseek query ./index 'error AND retry'` | 执行查询并按 BM25 排序。 |
| `stats <index>` | 是 | `snowseek stats ./index` | 校验索引并输出统计信息。 |
| `verify <index>` | 是 | `snowseek verify ./index` | 完整校验索引。 |
| `--help`、`-h` | 否 | `snowseek --help` | 显示帮助。 |
| `--version` | 否 | `snowseek --version` | 显示版本。 |

### 索引选项

以下选项适用于 `index`、`update`、`remove` 和 `compact`。资源选项均为可选项且
每项最多出现一次；`--index` 和 `--path` 的要求见下表。

| 参数 | 必须 | 示例 | 含义 |
|---|---|---|---|
| `--index <dir>` | `index`、`update` 必须 | `--index ./index` | 指定目标索引目录。 |
| `--path <glob>` | `remove` 至少一个 | `--path 'cache/**'` | 选择要删除的相对路径，可重复；应加引号避免 Shell 提前展开。 |
| `--temporary-directory <dir>` | 否 | `--temporary-directory /mnt/tmp` | 将工作区放入已存在目录；发布前仍会把候选复制回索引目录并重新校验。 |
| `--temporary-space-limit <size>` | 否 | `--temporary-space-limit 4GiB` | 限制逻辑临时空间；支持正整数以及 `B`、`KiB`、`MiB`、`GiB`、`TiB`。 |
| `--memory-limit <size>` | 否 | `--memory-limit 256MiB` | 覆盖档位的逻辑构建内存上限，单位格式同上。 |
| `--threads <N>` | 否 | `--threads 4` | 最多并行解析 N 个文件，N 必须大于 0。 |
| `--profile <name>` | 否 | `--profile minimal` | 资源档位：`minimal`、`balanced` 或 `performance`；默认 `balanced`。 |
| `--merge-fan-in <N>` | 否 | `--merge-fan-in 16` | 每组最多归并 N 个 Segment，N 至少为 2。 |

资源档位：

| 档位 | 内存 | 解析线程 | Merge fan-in | Position / 短语查询 |
|---|---:|---:|---:|---|
| `minimal` | 128 MiB | 1 | 4 | 不保存 / 不支持 |
| `balanced` | 256 MiB | 2 | 16 | 保存 / 支持 |
| `performance` | 1 GiB | 硬件线程数 | 32 | 保存 / 支持 |

### 查询选项

| 参数 | 必须 | 示例 | 含义 |
|---|---|---|---|
| `--source <dir>` | 否 | `--source ./docs` | 从原始语料读取命中行号和片段。 |
| `--top-k <N>` | 否 | `--top-k 10` | 最多返回 N 条结果，默认 20，上限 1000。 |
| `--jsonl` | 否 | `--jsonl` | 每条结果输出一个 JSON 对象；不能与 `--paths-only` 同时使用。 |
| `--paths-only` | 否 | `--paths-only` | 每行只输出一个相对路径；不能与 `--jsonl` 或 `--explain` 同时使用。 |
| `--explain` | 否 | `--explain` | 输出各查询词的 BM25 评分贡献。 |

### 查询表达式

| 表达式 | 示例 | 含义 |
|---|---|---|
| 词项 | `timeout` | 查询一个归一化词项。 |
| 精确短语 | `"timeout retry"` | 按连续 Position 匹配；`minimal` 索引不支持。 |
| `AND` | `timeout AND retry` | 同时满足两个表达式。 |
| `OR` | `timeout OR retry` | 满足任一表达式。 |
| `NOT` | `timeout AND NOT retry` | 排除后续表达式。 |
| 括号 | `(timeout OR retry) AND failed` | 改变求值顺序。 |
| 路径过滤 | `path:"src/*.cpp"` | 使用大小写敏感的 POSIX Glob 匹配索引相对路径。 |
| 扩展名过滤 | `extension:cpp` | 大小写不敏感地精确匹配扩展名，可带前导点。 |

`AND`、`OR`、`NOT` 大小写不敏感，优先级为 `NOT` > `AND` > `OR`。相邻词项
不会隐式连接，必须显式使用布尔运算符。双引号值可用 `\"` 和 `\\` 转义引号和
反斜杠。查询表达式最长 4096 字节，语法树深度上限为 32。

## 测试命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSNOWSEEK_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel 2
(cd build && ctest --output-on-failure)
```

执行 GCC/Clang × Debug/Release 测试矩阵：

```bash
./tools/test-matrix.sh
```

可通过 `SNOWSEEK_BUILD_JOBS` 设置并行任务数，通过 `SNOWSEEK_BUILD_ROOT` 设置矩阵
构建目录。

## Benchmark 命令

```bash
cmake -S . -B build-benchmark -DCMAKE_BUILD_TYPE=Release \
  -DSNOWSEEK_BUILD_BENCHMARKS=ON
cmake --build build-benchmark --parallel 2

./build-benchmark/benchmarks/snowseek_tokenizer_benchmark
./build-benchmark/benchmarks/snowseek_index_builder_benchmark
./build-benchmark/benchmarks/snowseek_index_builder_benchmark --samples 100
```

索引维护 benchmark 仅在 Linux 构建；参数和统计口径见
[docs/memory-baseline.md](docs/memory-baseline.md)。

## TODO

项目结构、设计说明和后续计划见 [docs/architecture.md](docs/architecture.md#后续-todo)。
