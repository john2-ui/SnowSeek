# 构建内存基线

## 目的与口径

该基线保留原单批次 `IndexBuilder` 的测量，并校准默认 128 MiB 刷写阈值下的分段
构建。基准生成确定性文本语料，完整执行扫描、Token 化、临时 Segment、K 路归并、
流式自校验和原子发布。它不使用随机源，也不设置尚未验证的吞吐或内存门槛。

`memory_*_bytes` 按 STL 容器 capacity、字符串 capacity 和哈希桶数量估算动态存储，
不包含 allocator 元数据、运行库及内核页。`memory_estimated_peak_bytes` 是各分类的
保守合计，不宣称等于堆分配或 RSS。Linux RSS 由基准进程通过
`getrusage(RUSAGE_SELF)` 独立读取；`rss_increment_bytes` 是构建后进程峰值与构建前
已有峰值之差。

## 复现

```bash
cmake -S . -B build-memory-baseline -DCMAKE_BUILD_TYPE=Release \
  -DSNOWSEEK_BUILD_TESTS=OFF -DSNOWSEEK_BUILD_BENCHMARKS=ON \
  -DSNOWSEEK_WARNINGS_AS_ERRORS=ON
cmake --build build-memory-baseline --parallel 2
./build-memory-baseline/benchmarks/snowseek_index_builder_benchmark
```

基准默认参数为 `--files 1024 --bytes-per-file 65536 --vocabulary 4096`，即
64 MiB 输入。三个参数都必须为正整数；基准拒绝格式错误、零值、重复参数、语料总量
乘法溢出以及超过默认扫描器单文件上限的文件大小。

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

结果只描述上述环境各一次测量。128 MiB 是文档提交后的活动容器容量阈值，不是 RSS
硬上限；STL vector 扩容、单文档临时 Token、序列化缓冲和分配器开销都会造成超出。
下一步将补充临时磁盘空间预算和有界 fan-in 多级归并，而不是把本次数字当作跨机器
性能门槛。
