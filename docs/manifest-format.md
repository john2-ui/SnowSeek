# SnowSeek Manifest Format v1

## 1. 作用域

索引目录中的稳定文件 `MANIFEST` 选择当前可查询 generation。Manifest v1 保存一个
非空、严格递增的活动 SegmentId 列表；Segment
文件名不写入 payload，而是由非零 `SegmentId` 推导为十进制、至少 16 位零填充的
`segment-<id>.idx`。

0.2 reader 要求 `MANIFEST` 存在，且所有活动 Segment 都是 v2。缺少 Manifest、
Manifest 损坏、引用缺失或 Segment 校验失败都必须报错，不能静默回退。现有
Segment v2 + Manifest v1 目录的字节保持兼容。

## 2. Header

Header 固定为 64 bytes，所有整数使用小端编码。Header CRC32C 覆盖 `[0, 52)`；自身
校验字段和保留字段不在覆盖范围内。

| Offset | Size | Field | v1 约束 |
|---:|---:|---|---|
| 0 | 8 | Magic | ASCII `SNOWMNFT` |
| 8 | 4 | Version | `1` |
| 12 | 4 | Flags | `0` |
| 16 | 4 | Header size | `64` |
| 20 | 4 | Active Segment count | 正整数 |
| 24 | 8 | Generation | 非零，完整发布后单调递增 |
| 32 | 8 | Next SegmentId | 非零且大于所有活动 ID |
| 40 | 8 | Payload length | checked `count × 8` |
| 48 | 4 | Payload CRC32C | 完整 payload 的 CRC32C |
| 52 | 4 | Header CRC32C | bytes `[0, 52)` 的 CRC32C |
| 56 | 8 | Reserved | `0` |

## 3. Payload

Payload 是连续的小端 `u64 SegmentId`。ID 必须非零且严格递增。
文件必须在声明的 payload 末尾结束，不接受截断或尾随 bytes。CRC32C 参数与 Segment
格式相同。

## 4. 发布与恢复契约

Linux writer 在索引目录 fd 上持有独占 `flock`，按以下顺序发布：

1. 验证并 `fsync` 候选 Segment；
2. rename 为最终 Segment 文件并 `fsync` 目录；
3. 在同目录创建唯一 Manifest 临时文件，写入、`fsync` 并重读验证；
4. 原子 rename 为 `MANIFEST`，再 `fsync` 目录；
5. 仅删除 `old_active - new_active`，并再次同步目录。

Manifest rename 切换可见 generation。若随后的目录同步失败，writer 保留旧 Segment
并报告提交后诊断；删除旧 Segment 只发生在新 Manifest 目录项成功同步后。下一次
writer 在持锁状态下清理 `.snowseek-build-*`、`.snowseek-manifest-*` 和未引用的合法
Segment 文件名，但不触碰未知文件。计算新 ID 时先纳入残留合法 Segment 的最大 ID，
因此进程中断不会造成 ID 重用。

`index` 是 Segment v1 或无 Manifest 旧目录的唯一迁移路径，不把旧 Segment 当作
可查询 generation 读取。writer 先写入并持久化新 Segment v2 和 Manifest，只在
Manifest rename 提交并同步目录后才清理旧固定 Segment；提交前失败仍保留旧文件。
