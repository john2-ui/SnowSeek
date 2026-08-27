# 构建资源基线

## 目的

该基准使用确定性文本语料，完整执行扫描、Token 化、临时 Segment、K 路归并、校验
和发布，用于比较资源档位并校准逻辑预算。数据只描述指定环境的一次测量，不作为跨
机器性能门槛。

指标口径：

- `memory_peak_bytes`：构建账本接受过的最大并发逻辑 reservation；
- RSS：Linux `getrusage(RUSAGE_SELF)` 报告的进程峰值；
- `temporary_peak_bytes`：私有工作区文件同时存在时的逻辑长度峰值；
- 索引大小：最终 Segment 字节数。

逻辑内存不包含 allocator、线程栈、运行库、内核页和部分序列化缓冲，因此不是 RSS
配额；临时峰值也不等于文件系统实际分配块数。

## 复现

```bash
cmake -S . -B build-memory-baseline -DCMAKE_BUILD_TYPE=Release \
  -DSNOWSEEK_BUILD_TESTS=OFF \
  -DSNOWSEEK_BUILD_BENCHMARKS=ON \
  -DSNOWSEEK_WARNINGS_AS_ERRORS=ON
cmake --build build-memory-baseline --parallel 2

./build-memory-baseline/benchmarks/snowseek_index_builder_benchmark
./build-memory-baseline/benchmarks/snowseek_index_builder_benchmark \
  --profile minimal
```

默认参数：

```text
--files 1024 --bytes-per-file 65536 --vocabulary 4096
```

即 64 MiB 输入。`--profile` 接受 `minimal`、`balanced` 和 `performance`；
三个语料参数必须为正整数。

## 现有测量

- 日期：2026-08-24
- 架构：x86_64
- 编译器：GCC 9.4.0
- 构建：Release，warnings-as-errors
- 输入：1024 文件 × 65536 bytes，词表 4096
- 每个档位在独立进程测量

| 指标 | Minimal | Balanced | Performance |
|---|---:|---:|---:|
| 线程 | 1 | 2 | 16 |
| Position | 关闭 | 开启 | 开启 |
| 内存预算 | 134,217,728 | 268,435,456 | 1,073,741,824 |
| 逻辑内存峰值 | 36,395,022 | 153,295,562 | 201,987,618 |
| RSS 峰值增量 | 62,836,736 | 284,086,272 | 462,254,080 |
| 临时 Segment | 8 | 2 | 1 |
| 归并层数 | 2 | 1 | 0 |
| 临时空间峰值 | 202,166,326 | 284,662,138 | 94,831,768 |
| 索引大小 | 67,333,164 | 94,831,768 | 94,831,768 |
| 吞吐 MiB/s | 15.148 | 10.308 | 18.060 |

三档逻辑峰值均未突破预算。Minimal 不保存 Position，因此索引更小，但不支持短语
查询。Balanced 和 Performance 的 RSS 高于逻辑峰值，差值来自预算口径明确排除的
开销。

这些数据采集于 Manifest 增量发布完成前，保留为历史基线，不代表当前 M5 路径的
绝对性能。后续应在相同语料上重新测量完整构建、update 和 compact，并补充 AArch64
设备结果。
