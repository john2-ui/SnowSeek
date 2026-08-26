# SnowSeek Segment Format v2

## 1. Scope

Version 2 stores one immutable Segment in the ordered active set selected by
the directory's [`MANIFEST`](manifest-format.md). Segment filenames derive from
monotonic IDs, for example
`<index-directory>/segment-0000000000000001.idx`. The directory layer does not
change the Segment bytes defined here. The
format contains no native C++ object representations: every integer has an
explicit width and is encoded in little-endian byte order.

Readers must reject unsupported versions or feature flags, nonzero reserved
fields, invalid offsets or lengths, integer overflow, truncation, and checksum
failure. CRC32C detects accidental corruption and is not a security hash.

## 2. Header

The header is exactly 200 bytes. Header CRC32C covers bytes `[0, 192)`; the
checksum field and final reserved field are outside that range.

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 8 | Magic | ASCII `SNOWSEEK` |
| 8 | 4 | Format version | `2` |
| 12 | 4 | Feature flags | Supported bits only |
| 16 | 4 | Header size | `200` |
| 20 | 4 | Section count | `5` |
| 24 | 8 | File size | Exact Segment byte length |
| 32 | 160 | Section directory | Five 32-byte descriptors |
| 192 | 4 | Header CRC32C | CRC32C of bytes `[0, 192)` |
| 196 | 4 | Reserved | `0` |

Feature bit `0x00000001` indicates that the Positions section is present.
Every other bit is unsupported. When the bit is clear, the Positions
section must be empty and every Posting position offset must be zero.

### 2.1 Section descriptor

Each descriptor has this 32-byte layout:

| Relative offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Section kind |
| 4 | 4 | Reserved flags, always `0` |
| 8 | 8 | Absolute section offset from the start of the file |
| 16 | 8 | Section length in bytes |
| 24 | 4 | CRC32C of the raw section bytes |
| 28 | 4 | Reserved, always `0` |

Descriptors occur exactly once in this order:

| Kind | Value |
|---|---:|
| Documents | 1 |
| Paths | 2 |
| Terms | 3 |
| Postings | 4 |
| Positions | 5 |

The Documents offset is 200. Each later offset equals the preceding offset plus
length, and the final offset plus length equals the header file size. Empty
sections have length and CRC32C equal to zero. Gaps and overlaps are invalid.

## 3. Sections

All offsets stored inside a section are relative to the beginning of the
referenced section. Counts and lengths must be validated before multiplication
or addition.

### 3.1 Documents

The section begins with a `u64` document count, followed by that many 48-byte
records in ascending, contiguous DocumentId order.

| Relative offset | Size | Field |
|---:|---:|---|
| 0 | 4 | DocumentId |
| 4 | 4 | Path length |
| 8 | 8 | Offset in Paths |
| 16 | 8 | Source file size |
| 24 | 8 | Signed modification time in Unix Epoch nanoseconds |
| 32 | 4 | Indexed token count |
| 36 | 4 | Document flags |
| 40 | 4 | Raw source CRC32C |
| 44 | 4 | Reserved, always `0` |

The signed timestamp uses its 64-bit two's-complement bit representation in
little-endian order.

Document flag bit `0x00000001` marks a Tombstone and bit `0x00000002` marks
the content CRC32C as valid. Other bits are rejected. A live record may carry
the CRC used by incremental change detection. A Tombstone stores only its
relative path: file size, mtime, token count, CRC, and CRC-valid bit must all be
zero, and no Posting may reference it. CRC32C is a non-adversarial change
fingerprint, not a cryptographic hash.

### 3.2 Paths

Paths are source-root-relative generic UTF-8 bytes concatenated without a
terminator. Document records delimit them with offset and length. Writers reject
paths that cannot be represented as nonempty valid UTF-8; `/` is the portable
separator stored by `generic_u8string()`. Absolute paths and `.` or `..` path
components are invalid, so a stored path cannot escape its source root.

### 3.3 Terms

The section starts with a `u64` term count, then that many fixed 32-byte records,
then the concatenated term bytes referenced by those records.

| Relative offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Term byte offset in this Terms section |
| 8 | 4 | Term byte length |
| 12 | 4 | Document frequency |
| 16 | 8 | Posting byte offset in Postings |
| 24 | 8 | Posting byte length |

Term records are strictly sorted by normalized term bytes. Terms are nonempty
ASCII bytes. Each posting length equals document frequency multiplied by
16.

### 3.4 Postings

Each Posting is a fixed 16-byte record. A term's records are contiguous and
DocumentIds are strictly increasing.

| Relative offset | Size | Field |
|---:|---:|---|
| 0 | 4 | DocumentId |
| 4 | 4 | Term frequency |
| 8 | 8 | Position byte offset in Positions |

Term frequency is nonzero. When Positions are enabled, the offset references
exactly `term_frequency` consecutive position values. When disabled, the offset
is zero.

### 3.5 Positions

Positions are absolute zero-based token ordinals encoded as consecutive `u32`
values in Posting order. Positions for one Posting are strictly increasing.
Delta and Varint compression require a later format version or feature and are
deferred to M6.

## 4. Checksums

CRC32C uses the reflected Castagnoli polynomial `0x82F63B78`, an initial state
of `0xFFFFFFFF`, and a final XOR of `0xFFFFFFFF`. The check value for ASCII
`123456789` is `0xE3069283`.

## 5. Multi-Segment visibility

The Manifest orders active SegmentIds increasingly. For each relative path,
the last record in `(Manifest Segment order, local DocumentId order)` is
visible. A last live record replaces an older version; a last Tombstone removes
the path. Directory loading validates all Segments first, assigns contiguous
global IDs only to visible live records, and remaps retained Postings. BM25 uses
this global live population and the filtered document frequency. All active
Segments must agree on the Positions feature.

## 6. Compatibility

A 0.2 reader accepts exactly Segment v2 with known feature bits; Segment v1 is
rejected with an instruction to rebuild the index. A 0.2 writer emits the same
v2 byte contract documented here, so existing Segment v2 + Manifest v1 indexes
remain byte-compatible. Logical records use fixed-width identifiers,
explicit byte order, and no host padding so a Segment produced on x86_64 Linux
can be consumed on AArch64 Linux. Readers retain term frequency when the
Positions feature is absent; term, Boolean, filtering, and BM25 operations stay
available, while exact phrase evaluation must report that positions are not
present rather than silently degrading to conjunction.
