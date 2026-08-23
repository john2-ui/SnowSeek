# 架构说明

SnowSeek 按依赖方向拆分为 `filesystem/document/analysis`、`index/storage`、
`query/ranking` 和 `cli`。CLI 只负责参数与输出；核心能力通过 `snowseek_core`
提供，便于以后嵌入其他本地程序。

```text
CLI ──┬── IndexBuilder ── Scanner + Tokenizer ── Segment/Storage
      └── QueryEngine ── Parser + Boolean/Phrase evaluator ── BM25 + Top-K
                                                   └───────── Source snippets
```

当前磁盘索引由单个不可变 Segment 构成；Manifest 和多 Segment 发布入口留到 M5。
所有跨平台磁盘数据使用固定宽度整数和显式字节序。首版 Segment 的精确布局和
兼容性规则见 [index-format.md](index-format.md)。

`document::TextReader` 使用固定大小缓冲区读取原文，并通过回调输出不跨越 UTF-8
字符边界的文本块。非法 UTF-8 默认替换为 U+FFFD，也可使用严格模式在首个错误的
原始字节偏移处终止。回调收到的 `string_view` 只在本次回调期间有效。

`analysis::TokenizerSession` 接受连续文本块并保留跨块 Token 状态，输出从零开始且
严格递增的 Token 位置。第一版仅索引 ASCII 字母、数字和下划线，非 ASCII 字符作为
分隔符；单个 Token 默认限制为 256 字节。

`document::DocumentStore` 按加入顺序分配连续 `DocumentId` 并保存内存文档表。
`index::InMemoryIndex` 将规范化词项映射到 Posting List；每个 Posting 内的位置严格
递增，同一词项的 DocumentId 也严格递增，为后续线性求交和磁盘编码提供不变量。

`index::InMemoryIndexBuilder` 连接 Scanner、TextReader、TokenizerSession、文档表与
内存倒排索引。每个文件先完整解析到临时 Token 集合，成功后才分配 DocumentId 并
提交 Posting，避免读取或分析失败留下半个文档。文档修改时间统一记录为 Unix Epoch
纳秒。

M2 使用单个不可变 Segment 持久化 Documents、Paths、Terms、Postings 和 Positions。
Header 与每个区域分别使用 CRC32C 校验，所有整数显式使用小端编码；加载器在构造
查询结构前验证版本、边界、顺序、计数与校验和。`IndexBuilder` 写入同目录临时文件
并自校验后发布。

M3 的 `query::parse_query` 将表达式解析为有深度上限的 AST，`QueryEngine` 对有序
DocumentId 集合执行布尔运算，短语命中额外验证连续 Position。AND 子树按估算文档
频率从小到大求交；匹配文档按正向去重词项的 BM25 之和评分，通过固定容量堆保留
Top-K，分数相同时按相对路径稳定排序。原文根目录不写入 v1 索引，调用方可显式提供
`source_root`，引擎只为最终 Top-K 读取原文并生成 UTF-8 行片段。
