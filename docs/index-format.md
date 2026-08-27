# 索引目录与磁盘格式

本文统一说明 SnowSeek 0.2 的 Manifest v1、Segment v2、多段可见性和发布契约。

## 通用约定

- 所有整数使用固定宽度小端编码，不写入原生 C++ 对象或结构体填充。
- offset 和 length 均按字节计算，读取前必须检查溢出、越界、截断和尾随数据。
- CRC32C 使用 Castagnoli 多项式，校验值 `CRC32C("123456789") == 0xE3069283`。
- CRC32C 只检测意外损坏，不是安全哈希。

索引目录的稳定文件为：

```text
MANIFEST
segment-0000000000000001.idx
segment-0000000000000002.idx
...
```

`MANIFEST` 选择当前可见 generation 和活动 Segment。SegmentId 非零且单调递增，
文件名由 ID 格式化为至少 16 位十进制数字。

## Manifest v1

Manifest 由固定 64-byte Header 和连续的 SegmentId payload 组成。

### Header

| Offset | Size | 字段 | 约束 |
|---:|---:|---|---|
| 0 | 8 | Magic | ASCII `SNOWMNFT` |
| 8 | 4 | Version | `1` |
| 12 | 4 | Flags | `0` |
| 16 | 4 | Header size | `64` |
| 20 | 4 | Active Segment count | 正整数 |
| 24 | 8 | Generation | 非零 |
| 32 | 8 | Next SegmentId | 大于所有活动 ID |
| 40 | 8 | Payload length | `count × 8` |
| 48 | 4 | Payload CRC32C | 完整 payload |
| 52 | 4 | Header CRC32C | bytes `[0, 52)` |
| 56 | 8 | Reserved | `0` |

### Payload

Payload 是 `count` 个连续的小端 `u64 SegmentId`。ID 必须非零且严格递增，文件
必须在 payload 末尾结束。

## Segment v2

一个 Segment 是不可变文件，依次包含 200-byte Header 和五个 section：

```text
Header → Documents → Paths → Terms → Postings → Positions
```

### Header

| Offset | Size | 字段 | 约束 |
|---:|---:|---|---|
| 0 | 8 | Magic | ASCII `SNOWSEEK` |
| 8 | 4 | Format version | `2` |
| 12 | 4 | Feature flags | 仅支持已知位 |
| 16 | 4 | Header size | `200` |
| 20 | 4 | Section count | `5` |
| 24 | 8 | File size | Segment 精确长度 |
| 32 | 160 | Section directory | 五个 32-byte descriptor |
| 192 | 4 | Header CRC32C | bytes `[0, 192)` |
| 196 | 4 | Reserved | `0` |

Feature bit `0x00000001` 表示保存 Position。该位关闭时，Positions section 必须为空，
所有 Posting 的 position offset 必须为零。

### Section descriptor

| 相对 offset | Size | 字段 |
|---:|---:|---|
| 0 | 4 | Section kind |
| 4 | 4 | Flags，固定为 `0` |
| 8 | 8 | Segment 内绝对 offset |
| 16 | 8 | 长度 |
| 24 | 4 | Section CRC32C |
| 28 | 4 | Reserved，固定为 `0` |

descriptor 必须按以下顺序各出现一次：

| Kind | Value |
|---|---:|
| Documents | 1 |
| Paths | 2 |
| Terms | 3 |
| Postings | 4 |
| Positions | 5 |

Documents 从 byte 200 开始；后一个 section 必须紧接前一个 section，最后一个 section
的末尾必须等于 Header 中的 file size。空 section 的长度和 CRC32C 都为零。

## Section 布局

section 内部保存的 offset 都相对于被引用 section 的起点。

### Documents

开头是 `u64 document_count`，之后是连续的 48-byte 记录。DocumentId 从零开始并
严格连续。

| 记录 offset | Size | 字段 |
|---:|---:|---|
| 0 | 4 | DocumentId |
| 4 | 4 | Path length |
| 8 | 8 | Paths offset |
| 16 | 8 | Source file size |
| 24 | 8 | Unix Epoch 纳秒 mtime 的二进制补码 |
| 32 | 4 | Token count |
| 36 | 4 | Document flags |
| 40 | 4 | Raw source CRC32C |
| 44 | 4 | Reserved，固定为 `0` |

Document flag：

- `0x00000001`：Tombstone；
- `0x00000002`：content CRC32C 有效。

live 记录可保存源文件 CRC32C，用于增量变化检测。Tombstone 只保存路径，其文件
大小、mtime、Token 数、CRC 和 CRC-valid bit 必须为零，且不能被 Posting 引用。

### Paths

所有路径按记录顺序连接，不带终止符，由 Document 的 offset 和 length 定界。路径是
非空、规范化、源目录相对的 UTF-8 generic path，分隔符为 `/`；绝对路径以及
`.`、`..` 分量非法。

### Terms

开头是 `u64 term_count`，之后是连续的 32-byte 记录，再之后是所有 term bytes。

| 记录 offset | Size | 字段 |
|---:|---:|---|
| 0 | 8 | Terms 内 term byte offset |
| 8 | 4 | Term length |
| 12 | 4 | Document frequency |
| 16 | 8 | Postings offset |
| 24 | 8 | Posting length |

term 必须是非空 ASCII bytes，并严格递增。Posting length 必须等于
`document_frequency × 16`。

### Postings

每条 Posting 固定 16 bytes，同一 term 的 Posting 连续且 DocumentId 严格递增。

| 记录 offset | Size | 字段 |
|---:|---:|---|
| 0 | 4 | DocumentId |
| 4 | 4 | Term frequency |
| 8 | 8 | Positions offset |

frequency 必须非零。启用 Position 时，offset 指向连续
`term_frequency` 个 `u32`；未启用时 offset 为零。

### Positions

Position 是按 Posting 顺序保存的绝对、从零开始的 Token 序号，每项为小端 `u32`。
同一 Posting 内必须严格递增。

## 多 Segment 可见性

Manifest 按 SegmentId 递增顺序列出活动 Segment。对同一路径，按
`(Manifest 顺序, Segment 内 DocumentId)` 最后出现的记录获胜：

- 最后记录是 live：覆盖旧版本；
- 最后记录是 Tombstone：路径不可见。

读取目录时先验证所有活动 Segment，再为最终可见 live 文档分配连续全局 DocumentId，
只装载并重映射这些文档的 Posting。所有活动 Segment 必须具有相同的 Position 能力。
BM25 使用最终可见文档数和过滤后的 document frequency。

## 发布与恢复

Linux writer 在索引目录 fd 上持有独占 `flock`，按以下顺序发布：

1. 完整验证工作区候选 Segment；
2. 若配置了外部临时目录，检查索引文件系统空间，将候选复制为同目录
   `.snowseek-segment-*` 暂存文件并重新完整验证；
3. `fsync` 可发布候选，rename 为最终 Segment 文件并 `fsync` 目录；
4. 创建同目录 Manifest 临时文件，写入、`fsync` 并重读验证；
5. 原子 rename 为 `MANIFEST`；
6. 再次 `fsync` 目录；
7. 删除 `old_active - new_active` 并同步目录。

Manifest rename 是唯一逻辑提交点。提交前失败继续选择旧 generation；提交后同步或
清理失败保留旧 Segment 并返回诊断。显式配置临时目录后，即使两个目录实际位于同一
文件系统也始终执行回拷，使跨文件系统与同文件系统使用同一发布路径。

临时空间逻辑预算在发布窗口同时计入外部候选、索引目录候选副本和 Manifest。工作区
写入使用临时文件系统的可用空间，回拷前另行检查索引文件系统能否容纳候选副本和
Manifest；逻辑字节预算与文件系统块配额是两个独立约束。

下一次 writer 只清理索引目录内可识别的 `.snowseek-build-*`、
`.snowseek-segment-*`、`.snowseek-manifest-*` 和未被 Manifest 引用的合法 Segment
文件，未知文件保持不动。残留合法 Segment 的最大 ID 参与下一 ID 计算，避免崩溃后
复用标识符。正常退出会删除外部工作区；进程崩溃遗留在共享外部临时目录的
`.snowseek-build-*` 不自动清理，避免误删其他索引正在使用的工作区。

## 兼容性

0.2 reader 只接受 Manifest v1 和 Segment v2，并拒绝未知版本、feature bit、非零
reserved、非法边界、顺序、计数或校验和。Segment v1 或缺少 Manifest 的目录会要求
重新构建。

`index` 是旧目录的安全迁移入口：新 Segment v2 和 Manifest v1 持久化后才删除旧
固定 Segment。现有 Segment v2 + Manifest v1 字节保持兼容。固定宽度、小端编码和
无宿主 padding 允许 x86_64 Linux 与 AArch64 Linux 互读。

Position 关闭时仍保留 term frequency，因此词项、布尔、过滤和 BM25 查询可用；
短语查询必须明确报错。Delta/Varint 等编码变化需要新的 Segment 版本。
