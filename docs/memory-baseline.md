# 构建内存基线

## 目的与口径

该基线保留原单批次 `IndexBuilder` 的测量，并校准默认 128 MiB 刷写阈值下的分段
构建。基准生成确定性文本语料，完整执行扫描、Token 化、临时 Segment、K 路归并、
流式自校验和原子发布。它不使用随机源，也不设置尚未验证的吞吐或内存门槛。

`memory_*_bytes` 按 STL 容器 capacity、字符串 capacity 和哈希桶数量估算动态存储，
不包含 allocator 元数据、运行库及内核页。`memory_estimated_peak_bytes` 是各分类的
保守合计，不宣称等于堆分配或 RSS。Linux RSS 由基准进程通过
`getrusage(RUSAGE_SELF)` 独立读取；`rss_increment_bytes` 是构建后进程峰值与构建前
已有峰值之差。`temporary_peak_bytes` 是私有工作区内初始 Segment、中间 Segment、
spool 和候选文件同时存在时的逻辑长度峰值，不等同于文件系统已分配块数。
Manifest 临时文件也计入 `temporary_peak_bytes`，但已提交的正式 Segment 和
Manifest 不计入构建预算。`memory_peak_bytes` 是线程安全预算账本实际接受过的最大
并发分类 reservation，包含 72-byte 单 Segment Manifest 编码缓冲；它不
统计 allocator、线程栈、运行库、内核页和 writer 序列化缓冲，因此不是 RSS 配额。

## 复现

```bash
cmake -S . -B build-memory-baseline -DCMAKE_BUILD_TYPE=Release \
  -DSNOWSEEK_BUILD_TESTS=OFF -DSNOWSEEK_BUILD_BENCHMARKS=ON \
  -DSNOWSEEK_WARNINGS_AS_ERRORS=ON
cmake --build build-memory-baseline --parallel 2
./build-memory-baseline/benchmarks/snowseek_index_builder_benchmark
./build-memory-baseline/benchmarks/snowseek_index_builder_benchmark \
  --profile minimal
```

基准默认参数为 `--files 1024 --bytes-per-file 65536 --vocabulary 4096`，即
64 MiB 输入。三个参数都必须为正整数；基准拒绝格式错误、零值、重复参数、语料总量
乘法溢出以及超过默认扫描器单文件上限的文件大小。`--profile` 接受 `minimal`、
`balanced` 和 `performance`。

## 原单批次结果

- 日期：2026-08-23
- 架构：x86_64
- 编译器：GCC 9.4.0（Ubuntu 9.4.0-1ubuntu1~20.04.2）
- 构建：Release，warnings-as-errors，2 个并行编译任务
- 参数：1024 文件 × 65536 bytes，词表 4096

| 指标 | 实测值 |
|---|---:|
| 输入字节 | 67,108,864 |
| Token 数 | 6,874,651 |
| 耗时 | 3,369,872 us |
| 吞吐 | 18.992 MiB/s |
| 索引大小 | 94,831,768 bytes |
| metadata 估算 | 417,468 bytes |
| reader 峰值估算 | 327,695 bytes |
| token 峰值估算 | 2,062,336 bytes |
| dictionary 估算 | 663,024 bytes |
| posting 估算 | 323,432,664 bytes |
| 分类保守合计 | 326,903,187 bytes |
| 构建前进程峰值 RSS | 69,697,536 bytes |
| 进程峰值 RSS | 637,636,608 bytes |
| RSS 峰值增量 | 567,939,072 bytes |

## 默认 128 MiB 分段结果

- 日期：2026-08-23
- 架构：x86_64
- 编译器：GCC 9.4.0（Ubuntu 9.4.0-1ubuntu1~20.04.2）
- 构建：Release，warnings-as-errors，2 个并行编译任务
- 参数：1024 文件 × 65536 bytes，词表 4096
- Segment 刷写阈值：134,217,728 bytes

| 指标 | 实测值 |
|---|---:|
| 输入字节 | 67,108,864 |
| Token 数 | 6,874,651 |
| 临时 Segment 数 | 2 |
| 归并 fan-in | 16 |
| 归并层数 | 1 |
| 临时空间峰值 | 284,662,138 bytes |
| 耗时 | 5,421,591 us |
| 吞吐 | 11.805 MiB/s |
| 索引大小 | 94,831,768 bytes |
| metadata 估算 | 184,542 bytes |
| reader 峰值估算 | 327,695 bytes |
| token 峰值估算 | 2,062,336 bytes |
| dictionary 峰值估算 | 331,512 bytes |
| posting 峰值估算 | 148,033,044 bytes |
| 分类保守合计 | 150,939,129 bytes |
| 构建前进程峰值 RSS | 72,404,992 bytes |
| 进程峰值 RSS | 284,024,832 bytes |
| RSS 峰值增量 | 211,619,840 bytes |

相同输入生成的最终索引字节数保持不变。与原单批次测量相比，RSS 峰值增量从
567,939,072 bytes 降至 211,619,840 bytes，约下降 62.7%；分类保守合计约下降
53.8%。吞吐从 18.992 MiB/s 降至 11.805 MiB/s，代价主要来自临时 Segment 写入、
两遍 term 归并和最终流式验证。

归并层数和临时空间峰值于 2026-08-24 使用相同确定性参数复测；这两个逻辑资源指标
不替换上表 2026-08-23 的耗时和 RSS 记录。本次复测的最终索引大小和分类内存估算与
原记录一致。

## 资源档位结果

- 日期：2026-08-24
- 架构与编译器：x86_64，GCC 9.4.0
- 构建：Release，warnings-as-errors
- 参数：1024 文件 × 65536 bytes，词表 4096；每档独立进程测量

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

三档的逻辑峰值均未突破预算。Balanced 的 RSS 增量比 256 MiB 逻辑预算高约 15 MiB，
来自明确排除的线程栈、allocator 和序列化阶段等开销；Performance 同样不能把 1 GiB
解释为进程配额。Minimal 因移除 Positions 缩小最终索引，但不再支持短语查询。

上述表格是引入 Manifest 前的历史实测，不为尚未重新测量的 M5 改动改写数值；M5 会
使逻辑临时峰值最多增加一个 72-byte Manifest 临时文件，并新增 SegmentId 和
generation 输出。结果只描述上述环境各一次测量。临时空间限制采用输入大小推导的保守预检，可能在高
词项重复率下早于实际空间需求拒绝构建；这些数字不能作为跨机器性能门槛。下一步将
补充自定义临时目录，并在 AArch64 上校准 RSS、吞吐和索引兼容性。
