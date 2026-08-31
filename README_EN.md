# SnowSeek

## Introduction

[中文](README.md)

SnowSeek is a local full-text search engine for embedded Linux, written in
C++20 with no third-party runtime dependencies. It supports full and
incremental indexing, deletion, compaction, Boolean and phrase queries, BM25
ranking, and durable publication through immutable Segments and a Manifest.

## Build

SnowSeek requires Linux, CMake 3.16 or newer, and a C++20-capable GCC or Clang.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
./build/snowseek --version
```

## Usage

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

Install the built binary into `/usr/local/bin` to run it without a path:

```bash
./tools/install.sh
snowseek --help
./tools/uninstall.sh
```

Override the build or installation directory with `SNOWSEEK_BUILD_DIR` and
`SNOWSEEK_INSTALL_DIR`. A custom installation directory must be in `PATH`.

## Command-line options

Run `snowseek --help` for the command summary. The supported command forms are:

```text
snowseek index <source> --index <dir> [options]
snowseek update <source> --index <dir> [options]
snowseek remove <index> --path <glob> [options]
snowseek compact <index> [options]
snowseek query <index> <expression> [options]
snowseek stats|verify <index>
```

`index`, `update`, `remove`, and `compact` accept optional
`--temporary-directory`, `--temporary-space-limit`, `--memory-limit`,
`--threads`, `--profile`, and `--merge-fan-in` settings. Sizes accept `B`,
`KiB`, `MiB`, `GiB`, and `TiB`. Profiles are `minimal`, `balanced` (default),
and `performance`. `remove` requires at least one repeatable `--path` POSIX
Glob.

Queries accept terms, quoted phrases, case-insensitive `AND`, `OR`, and `NOT`,
parentheses, case-sensitive `path:` POSIX Globs, and case-insensitive exact
`extension:` filters. Precedence is `NOT` > `AND` > `OR`; adjacent operands
require an explicit operator. Query options are `--source`, `--top-k`,
`--jsonl`, `--paths-only`, and `--explain`.

See the Chinese [command-line tables](README.md#命令行参数) for required fields,
examples, defaults, constraints, and expression details.

## Tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSNOWSEEK_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel 2
(cd build && ctest --output-on-failure)
./tools/test-matrix.sh
```

## Benchmarks

```bash
cmake -S . -B build-benchmark -DCMAKE_BUILD_TYPE=Release \
  -DSNOWSEEK_BUILD_BENCHMARKS=ON
cmake --build build-benchmark --parallel 2
./build-benchmark/benchmarks/snowseek_tokenizer_benchmark
./build-benchmark/benchmarks/snowseek_index_builder_benchmark --samples 100
```

## TODO

See [docs/architecture.md](docs/architecture.md#后续-todo) for architecture and
the roadmap.
