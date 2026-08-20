# 架构说明

SnowSeek 按依赖方向拆分为 `filesystem/document/analysis`、`index/storage`、
`query/ranking` 和 `cli`。CLI 只负责参数与输出；核心能力通过 `snowseek_core`
提供，便于以后嵌入其他本地程序。

```text
CLI ──┬── IndexBuilder ── Scanner + Tokenizer ── Segment/Storage
      └── QueryEngine ── Parser + PostingReader ── BM25 + Top-K
```

磁盘 Segment 只追加且不可变。Manifest 是已提交 Segment 的唯一发布入口，查询端
只读取 Manifest 可见的数据。所有跨平台磁盘数据使用固定宽度整数和显式字节序。

