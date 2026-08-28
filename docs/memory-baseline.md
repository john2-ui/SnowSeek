# 索引维护基准

## 目的

该基准使用确定性文本语料，统一测量完整构建、增量更新和压缩在冷、热文件缓存下的
延迟、吞吐、索引体积、写放大及逻辑资源峰值。结果只用于相同硬件、文件系统、编译器
和参数之间的比较，不作为跨机器性能门槛。

## 构建与运行

```bash
cmake -S . -B build-benchmark -DCMAKE_BUILD_TYPE=Release \
  -DSNOWSEEK_BUILD_BENCHMARKS=ON \
  -DSNOWSEEK_WARNINGS_AS_ERRORS=ON
cmake --build build-benchmark --parallel 2

./build-benchmark/benchmarks/snowseek_index_builder_benchmark
./build-benchmark/benchmarks/snowseek_index_builder_benchmark \
  --samples 100
```

默认参数为：

```text
--files 1024
--bytes-per-file 65536
--vocabulary 4096
--changed-files 1
--samples 10
--profile balanced
```

`--changed-files` 必须小于等于 `--files`。正式记录 P99 时建议使用至少 100 个样本；
默认 10 个样本便于日常运行，此时 nearest-rank P95 和 P99 都可能选择最大样本。

## 场景与缓存

每个样本使用独立索引状态，场景准备不计入测量：

| 操作 | 样本初始状态 | 吞吐与写放大分母 |
|---|---|---:|
| `rebuild` | 空索引目录 | 完整语料字节 |
| `update` | 旧 generation 的复制 | 被修改文件字节 |
| `compact` | 双 Segment 索引的复制 | 压缩前活动 Segment 字节 |

冷缓存对相关语料和索引文件执行 `fsync`，再调用
`posix_fadvise(POSIX_FADV_DONTNEED)`；热缓存在计时前顺序预读相关文件。两种准备都在
计时和 I/O 计数之外。冷缓存只驱逐文件内容页，不清空目录项、inode 或整机全局缓存，
因此不等同于 root 执行的 `drop_caches`。

## 指标口径

- 延迟：`steady_clock` 记录操作时间，排序后使用 nearest-rank 输出 P50/P95/P99；
- 吞吐：全部样本的逻辑输入字节除以全部样本总耗时；
- 写入字节：操作前后 `/proc/self/io` 的 `write_bytes` 差值之和；
- 写放大：总 `write_bytes` 除以总逻辑输入字节；
- 活动体积：Manifest 选中的所有 Segment 逻辑字节；
- 目录体积：操作后索引目录内所有普通文件的逻辑字节；
- 内存与临时空间：各场景样本报告值的最大值；
- RSS：`getrusage(RUSAGE_SELF)` 的全进程生命周期峰值，不代表单个场景峰值。

写入字节包含临时 Segment、发布文件和文件系统实际提交的块，但受块大小、缓存回写和
文件系统实现影响。复制种子索引、缓存准备和样本清理发生在计数窗口外。

输出使用稳定的 `<operation>.<cache>.<metric>=<value>` 键，例如：

```text
update.cold.elapsed_us_p95=...
update.cold.throughput_mib_per_second=...
update.cold.kernel_write_bytes_total=...
update.cold.write_amplification=...
```

## 当前测量

- 日期：2026-08-27
- 架构：x86_64，16 个在线 CPU
- 内核：Linux 5.15.0-139-generic
- 文件系统：ext4，基准目录位于 `/tmp`
- 编译器：GCC 9.4.0
- 构建：Release，warnings-as-errors
- 输入：1024 文件 × 65536 bytes，词表 4096，修改 1 个文件
- 采样：每个“操作 × 缓存”组合 10 次

延迟和吞吐结果：

| 场景 | P50（ms） | P95（ms） | P99（ms） | 吞吐（MiB/s） |
|---|---:|---:|---:|---:|
| rebuild cold | 7547.102 | 14332.074 | 14332.074 | 6.771 |
| rebuild hot | 8789.745 | 14532.967 | 14532.967 | 6.023 |
| update cold | 2455.229 | 2469.721 | 2469.721 | 0.025 |
| update hot | 2313.355 | 2346.501 | 2346.501 | 0.027 |
| compact cold | 4238.700 | 4649.473 | 4649.473 | 21.096 |
| compact hot | 4237.604 | 4318.307 | 4318.307 | 21.308 |

体积、写放大和逻辑资源峰值：

| 场景 | 活动 Segment（前 → 后，bytes） | 操作后目录（bytes） | 逻辑分母总计（bytes） | 内核写入总计（bytes） | 写放大 | 内存峰值（bytes） | 临时峰值（bytes） |
|---|---:|---:|---:|---:|---:|---:|---:|
| rebuild cold | 0 → 94839960 | 94840032 | 671088640 | 2847047680 | 4.242 | 153303754 | 284686714 |
| rebuild hot | 0 → 94839960 | 94840032 | 671088640 | 2847047680 | 4.242 | 153303754 | 284686714 |
| update cold | 94839960 → 95099700 | 95099780 | 655360 | 2662400 | 4.062 | 164536103 | 259820 |
| update hot | 94839960 → 95099700 | 95099780 | 655360 | 2662400 | 4.062 | 164536103 | 259820 |
| compact cold | 95099700 → 94839960 | 94840032 | 950997000 | 948469760 | 0.997 | 162146072 | 94840032 |
| compact hot | 95099700 → 94839960 | 94840032 | 950997000 | 948469760 | 0.997 | 162146072 | 94840032 |

本次进程生命周期 RSS 峰值为 `476319744` bytes。10 个样本下 P95 与 P99 都取最大
样本；表中冷、热差异也可能包含运行顺序和系统负载噪声，不能据此建立跨机器阈值。

优化前火焰图、热点分析和已验证的前后结果见
[索引维护性能优化](optimization.md)。
