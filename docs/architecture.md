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

`document::TextReader` 使用固定大小缓冲区读取原文，并通过回调输出不跨越 UTF-8
字符边界的文本块。非法 UTF-8 默认替换为 U+FFFD，也可使用严格模式在首个错误的
原始字节偏移处终止。回调收到的 `string_view` 只在本次回调期间有效。
