# SnowSeek

SnowSeek 是一个面向嵌入式 Linux 的零第三方运行依赖本地全文检索引擎，使用
C++20 实现。项目目前处于框架搭建阶段。

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
./build/snowseek query ./snowseek-index timeout
./build/snowseek query ./snowseek-index "timeout AND retry"
./build/snowseek stats ./snowseek-index
./build/snowseek verify ./snowseek-index
```

索引写入 `segment-0000000000000001.idx`，保存相对源目录的文档路径。若个别文件
无法读取或分析，成功文档仍会发布，但 `index` 返回状态码 2 并输出诊断。

测试不依赖第三方框架，并且在 Debug 和 Release 构建中都会执行显式检查。若需要将
编译器警告视为错误，可在配置时增加 `-DSNOWSEEK_WARNINGS_AS_ERRORS=ON`。

一键执行 GCC/Clang 的 Debug/Release 编译与测试矩阵：

```bash
./tools/test-matrix.sh
```

可通过 `SNOWSEEK_BUILD_JOBS` 调整并行任务数，通过 `SNOWSEEK_BUILD_ROOT`
指定构建产物目录。

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
