#include "storage/segment_merge.hpp"

#include "common/checked_arithmetic.hpp"
#include "storage/binary_codec.hpp"
#include "storage/checksum.hpp"
#include "storage/index_header.hpp"
#include "storage/index_file_internal.hpp"
#include "storage/segment_format_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowseek::storage::detail {
namespace {

constexpr std::size_t kCopyBufferSize = 64 * 1024;

using common::detail::checked_add;
using common::detail::checked_multiply;
using detail::open_input;
using detail::read_exact;
using detail::seek_to;

/**
 * @brief Narrows a Segment integer after checking the destination range.
 * @tparam Destination Unsigned destination type.
 * @param value Source value.
 * @param field Field named in a range diagnostic.
 * @return The narrowed value.
 * @throws std::runtime_error If value exceeds the destination range.
 */
template <typename Destination>
[[nodiscard]] Destination checked_narrow(std::uint64_t value,
                                         std::string_view field) {
        if (value > std::numeric_limits<Destination>::max()) {
                throw std::runtime_error(std::string(field) +
                                         " exceeds the Segment format limit");
        }
        return static_cast<Destination>(value);
}

/**
 * @brief Opens a new truncated spool or candidate output stream.
 * @param path Destination path to create.
 * @return An open binary stream.
 * @throws std::runtime_error If the file cannot be created.
 */
[[nodiscard]] std::ofstream open_output(const std::filesystem::path &path) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
                throw std::runtime_error("failed to create merge output: " +
                                         path.string());
        }
        return output;
}

/**
 * @brief Closes an output and reports delayed write errors.
 * @param output Stream to flush and close.
 * @throws std::runtime_error If flushing or closing fails.
 */
void finish_output(std::ofstream &output) {
        output.close();
        if (!output) {
                throw std::runtime_error("failed to write merge spool");
        }
}

/**
 * @brief Removes one completed merge spool before the next merge group.
 * @param path Private spool path owned by the current build workspace.
 * @throws std::runtime_error If the spool cannot be removed.
 */
void remove_spool(const std::filesystem::path &path) {
        std::error_code error;
        const bool removed = std::filesystem::remove(path, error);
        if (error || !removed) {
                throw std::runtime_error("failed to remove merge spool: " +
                                         path.string());
        }
}

/**
 * @brief Reads one validated v2 header from a Segment.
 * @param path Segment path.
 * @return Decoded header.
 * @throws std::runtime_error If opening or header decoding fails.
 */
[[nodiscard]] IndexHeader
read_segment_header(const std::filesystem::path &path) {
        auto input = open_input(path);
        return read_header(input);
}

class TermCursor {
      public:
        /**
         * @brief Opens one temporary Segment at its first term.
         * @param source Valid temporary Segment and logical statistics.
         * @param document_base Global ID added to its local document IDs.
         * @throws std::runtime_error If the Segment term table is malformed.
         */
        TermCursor(const SegmentSource &source, std::uint64_t document_base)
            : header_(read_segment_header(source.path)),
              term_records_(open_input(source.path)),
              term_bytes_(open_input(source.path)),
              postings_(open_input(source.path)),
              positions_(open_input(source.path)),
              document_base_(document_base),
              has_positions_((header_.feature_flags & kFeaturePositions) != 0) {
                seek_to(term_records_,
                        section(header_, SectionKind::terms).offset);
                term_count_ = read_u64_le(term_records_);
                if (term_count_ != source.stats.term_count) {
                        throw std::runtime_error(
                                "temporary Segment term count mismatch");
                }
                advance();
        }

        /** @brief Returns whether this cursor currently names a term. */
        [[nodiscard]] bool valid() const noexcept { return valid_; }

        /** @brief Returns the current normalized term. */
        [[nodiscard]] const std::string &term() const noexcept { return term_; }

        /** @brief Returns the current term's local document frequency. */
        [[nodiscard]] std::uint32_t document_frequency() const noexcept {
                return record_.document_frequency;
        }

        /**
         * @brief Advances to the next term record.
         * @throws std::runtime_error If the next term record or bytes are
         * truncated.
         */
        void advance() {
                if (term_index_ >= term_count_) {
                        valid_ = false;
                        term_.clear();
                        return;
                }
                record_ = read_term_record(term_records_);
                term_.assign(record_.term_length, '\0');
                seek_to(term_bytes_,
                        checked_add(section(header_, SectionKind::terms).offset,
                                    record_.term_offset, "term offset"));
                read_exact(term_bytes_, term_.data(), term_.size());
                ++term_index_;
                valid_ = true;
        }

        /**
         * @brief Copies the current postings with remapped IDs and positions.
         * @param postings_output Destination Posting records spool.
         * @param positions_output Destination Position values spool.
         * @param output_position_offset Current byte offset in final Positions.
         * @throws std::runtime_error If an input is malformed, an ID exceeds
         * format limits, or writing fails.
         */
        void copy_postings(std::ostream &postings_output,
                           std::ostream &positions_output,
                           std::uint64_t &output_position_offset) {
                seek_to(postings_,
                        checked_add(
                                section(header_, SectionKind::postings).offset,
                                    record_.posting_offset, "posting offset"));
                for (std::uint32_t index = 0;
                     index < record_.document_frequency; ++index) {
                        const auto posting = read_posting_record(postings_);
                        const auto global_document =
                                checked_narrow<std::uint32_t>(
                                        checked_add(document_base_,
                                                    posting.document_id,
                                                    "global document id"),
                                        "global document id");
                        write_posting_record(
                                postings_output,
                                PostingRecord{
                                        global_document, posting.frequency,
                                        has_positions_ ? output_position_offset
                                                       : 0});

                        if (has_positions_) {
                                seek_to(positions_,
                                        checked_add(
                                                section(header_,
                                                        SectionKind::positions)
                                                        .offset,
                                                posting.position_offset,
                                                    "position offset"));
                                for (std::uint32_t position_index = 0;
                                     position_index < posting.frequency;
                                     ++position_index) {
                                        write_u32_le(positions_output,
                                                     read_u32_le(positions_));
                                }
                                output_position_offset = checked_add(
                                        output_position_offset,
                                        checked_multiply(
                                                posting.frequency,
                                                kPositionRecordSize,
                                                "position byte length"),
                                        "position offset");
                        } else if (posting.position_offset != 0) {
                                throw std::runtime_error("positionless Segment "
                                                         "has nonzero offset");
                        }
                }
        }

      private:
        IndexHeader header_;
        std::ifstream term_records_;
        std::ifstream term_bytes_;
        std::ifstream postings_;
        std::ifstream positions_;
        std::uint64_t document_base_{};
        std::uint64_t term_count_{};
        std::uint64_t term_index_{};
        bool valid_{};
        TermRecord record_;
        std::string term_;
        bool has_positions_{};
};

/**
 * @brief Visits equal-term groups in lexicographic K-way order.
 * @tparam Consumer Callable receiving term, source indexes, and cursors.
 * @param sources Temporary Segments in document order.
 * @param consumer Callback invoked before each cursor group advances.
 * @throws std::runtime_error If cursor input is malformed or consumer fails.
 */
template <typename Consumer>
void walk_terms(const std::vector<SegmentSource> &sources, Consumer consumer) {
        std::vector<TermCursor> cursors;
        cursors.reserve(sources.size());
        std::uint64_t document_base = 0;
        for (const auto &source : sources) {
                cursors.emplace_back(source, document_base);
                document_base = checked_add(
                        document_base, source.stats.physical_document_count,
                        "document count");
        }

        const auto compare = [&cursors](std::size_t left, std::size_t right) {
                if (cursors[left].term() != cursors[right].term()) {
                        return cursors[left].term() > cursors[right].term();
                }
                return left > right;
        };
        std::priority_queue<std::size_t, std::vector<std::size_t>,
                            decltype(compare)>
                heap(compare);
        for (std::size_t index = 0; index < cursors.size(); ++index) {
                if (cursors[index].valid()) {
                        heap.push(index);
                }
        }

        std::vector<std::size_t> group;
        group.reserve(cursors.size());
        while (!heap.empty()) {
                const std::string term = cursors[heap.top()].term();
                group.clear();
                while (!heap.empty() && cursors[heap.top()].term() == term) {
                        group.push_back(heap.top());
                        heap.pop();
                }
                std::sort(group.begin(), group.end());
                consumer(term, group, cursors);
                for (const auto index : group) {
                        cursors[index].advance();
                        if (cursors[index].valid()) {
                                heap.push(index);
                        }
                }
        }
}

/**
 * @brief Concatenates document tables while remapping IDs and path offsets.
 * @param sources Temporary Segments in document order.
 * @param documents_output Final Documents section spool.
 * @param paths_output Final Paths section spool.
 * @throws std::runtime_error If an input is malformed, IDs overflow, or a
 * spool write fails.
 */
void merge_documents(const std::vector<SegmentSource> &sources,
                     std::ostream &documents_output,
                     std::ostream &paths_output) {
        std::uint64_t document_count = 0;
        for (const auto &source : sources) {
                document_count =
                        checked_add(document_count,
                                    source.stats.physical_document_count,
                                    "document count");
        }
        write_u64_le(documents_output, document_count);

        std::uint64_t document_base = 0;
        std::uint64_t output_path_offset = 0;
        for (const auto &source : sources) {
                const auto header = read_segment_header(source.path);
                auto documents_input = open_input(source.path);
                auto paths_input = open_input(source.path);
                seek_to(documents_input,
                        section(header, SectionKind::documents).offset);
                const auto local_count = read_u64_le(documents_input);
                if (local_count !=
                    source.stats.physical_document_count) {
                        throw std::runtime_error(
                                "temporary Segment document count mismatch");
                }
                for (std::uint64_t index = 0; index < local_count; ++index) {
                        auto record = read_document_record(documents_input);
                        const auto input_path_offset = record.path_offset;
                        if (record.document_id != index ||
                            record.reserved != 0 ||
                            (record.flags & ~kSupportedDocumentFlags) != 0) {
                                throw std::runtime_error(
                                        "invalid temporary document record");
                        }
                        const auto global_id = checked_narrow<std::uint32_t>(
                                checked_add(document_base, record.document_id,
                                            "global document id"),
                                "global document id");
                        record.document_id = global_id;
                        record.path_offset = output_path_offset;
                        write_document_record(documents_output, record);

                        std::string path_bytes(record.path_length, '\0');
                        seek_to(paths_input,
                                checked_add(
                                        section(header, SectionKind::paths)
                                                .offset,
                                        input_path_offset,
                                        "path offset"));
                        read_exact(paths_input, path_bytes.data(),
                                   path_bytes.size());
                        paths_output.write(path_bytes.data(),
                                           static_cast<std::streamsize>(
                                                   path_bytes.size()));
                        output_path_offset =
                                checked_add(output_path_offset,
                                            record.path_length,
                                            "paths section length");
                }
                document_base = checked_add(document_base, local_count,
                                            "document base");
        }
}

/**
 * @brief Copies one spool into the candidate and updates its checksum.
 * @param path Spool path to consume.
 * @param output Candidate stream.
 * @param checksum Section checksum state to extend.
 * @param buffer Fixed-size reusable copy buffer.
 * @return Number of bytes copied.
 * @throws std::runtime_error If reading or writing fails.
 */
[[nodiscard]] std::uint64_t
copy_spool(const std::filesystem::path &path, std::ostream &output,
           Crc32c &checksum, std::array<char, kCopyBufferSize> &buffer) {
        auto input = open_input(path);
        std::uint64_t copied = 0;
        while (input) {
                input.read(buffer.data(),
                           static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count > 0) {
                        output.write(buffer.data(), count);
                        if (!output) {
                                throw std::runtime_error(
                                        "failed to assemble merged Segment");
                        }
                        checksum.update(std::string_view(
                                buffer.data(),
                                static_cast<std::size_t>(count)));
                        copied = checked_add(copied,
                                             static_cast<std::uint64_t>(count),
                                             "spool length");
                }
        }
        if (!input.eof()) {
                throw std::runtime_error("failed to read merge spool");
        }
        return copied;
}

/**
 * @brief Encodes one little-endian uint64 prefix in memory.
 * @param value Value to encode.
 * @return Exactly eight encoded bytes.
 * @throws std::runtime_error If the in-memory stream fails.
 */
[[nodiscard]] std::string encode_u64(std::uint64_t value) {
        std::ostringstream stream(std::ios::out | std::ios::binary);
        write_u64_le(stream, value);
        const auto bytes = stream.str();
        if (bytes.size() != 8) {
                throw std::runtime_error("failed to encode section count");
        }
        return bytes;
}

} // namespace

std::uint64_t estimate_segment_merge_memory(std::size_t source_count) {
        const auto source_bytes =
                checked_multiply(source_count,
                                 sizeof(SegmentSource) + sizeof(TermCursor) +
                                         2 * sizeof(std::size_t),
                                 "merge cursor memory");
        return checked_add(source_bytes, kCopyBufferSize + kIndexHeaderSize,
                           "merge working memory");
}

std::uint64_t merge_index_files(const std::filesystem::path &output,
                                const std::vector<SegmentSource> &sources) {
        if (sources.empty()) {
                throw std::invalid_argument(
                        "merge requires at least one temporary Segment");
        }

        const auto first_header = read_segment_header(sources.front().path);
        for (const auto &source : sources) {
                if (read_segment_header(source.path).feature_flags !=
                    first_header.feature_flags) {
                        throw std::runtime_error("cannot merge Segments with "
                                                 "different features");
                }
        }

        const auto parent = output.parent_path();
        const auto documents_path = parent / "documents.spool";
        const auto paths_path = parent / "paths.spool";
        const auto term_records_path = parent / "term-records.spool";
        const auto term_bytes_path = parent / "term-bytes.spool";
        const auto postings_path = parent / "postings.spool";
        const auto positions_path = parent / "positions.spool";

        // Spool document metadata once in final global ID order.
        auto documents_output = open_output(documents_path);
        auto paths_output = open_output(paths_path);
        merge_documents(sources, documents_output, paths_output);
        finish_output(documents_output);
        finish_output(paths_output);

        // The first term pass discovers the fixed term-table size.
        std::uint64_t term_count = 0;
        walk_terms(sources, [&term_count](const std::string &,
                                          const std::vector<std::size_t> &,
                                          std::vector<TermCursor> &) {
                term_count = checked_add(term_count, 1, "term count");
        });

        auto term_records_output = open_output(term_records_path);
        auto term_bytes_output = open_output(term_bytes_path);
        auto postings_output = open_output(postings_path);
        auto positions_output = open_output(positions_path);
        std::uint64_t term_bytes_written = 0;
        std::uint64_t posting_offset = 0;
        std::uint64_t position_offset = 0;
        const auto term_bytes_begin =
                checked_add(8,
                            checked_multiply(term_count, kTermRecordSize,
                                             "term table length"),
                            "term byte offset");

        // The second pass writes merged term metadata and remapped postings.
        walk_terms(sources, [&](const std::string &term,
                                const std::vector<std::size_t> &group,
                                std::vector<TermCursor> &cursors) {
                std::uint64_t document_frequency = 0;
                for (const auto index : group) {
                        document_frequency =
                                checked_add(document_frequency,
                                            cursors[index].document_frequency(),
                                            "document frequency");
                }
                const auto posting_length =
                        checked_multiply(document_frequency, kPostingRecordSize,
                                         "term posting length");
                write_term_record(
                        term_records_output,
                        TermRecord{
                                checked_add(term_bytes_begin,
                                            term_bytes_written, "term offset"),
                                checked_narrow<std::uint32_t>(term.size(),
                                                              "term length"),
                                checked_narrow<std::uint32_t>(
                                        document_frequency,
                                        "document frequency"),
                                posting_offset, posting_length});
                term_bytes_output.write(
                        term.data(), static_cast<std::streamsize>(term.size()));

                for (const auto index : group) {
                        cursors[index].copy_postings(postings_output,
                                                     positions_output,
                                                     position_offset);
                }
                term_bytes_written = checked_add(
                        term_bytes_written, term.size(), "term bytes length");
                posting_offset = checked_add(posting_offset, posting_length,
                                             "posting offset");
        });
        finish_output(term_records_output);
        finish_output(term_bytes_output);
        finish_output(postings_output);
        finish_output(positions_output);

        // Assemble packed sections while calculating their streaming CRC32C.
        auto candidate = open_output(output);
        std::array<char, kIndexHeaderSize> empty_header{};
        candidate.write(empty_header.data(), empty_header.size());
        IndexHeader header;
        header.feature_flags = first_header.feature_flags;
        std::uint64_t file_offset = kIndexHeaderSize;
        std::uint64_t spool_bytes = 0;
        std::array<char, kCopyBufferSize> copy_buffer{};
        const std::array<std::vector<std::filesystem::path>, kIndexSectionCount>
                section_parts{{{documents_path},
                               {paths_path},
                               {term_records_path, term_bytes_path},
                               {postings_path},
                               {positions_path}}};
        const auto term_count_prefix = encode_u64(term_count);
        for (std::size_t index = 0; index < section_parts.size(); ++index) {
                auto &descriptor =
                        section(header, kIndexSectionOrder[index]);
                descriptor.offset = file_offset;
                Crc32c checksum;
                std::uint64_t section_length = 0;
                if (index == 2) {
                        candidate.write(term_count_prefix.data(),
                                        term_count_prefix.size());
                        checksum.update(term_count_prefix);
                        section_length = term_count_prefix.size();
                }
                for (const auto &part : section_parts[index]) {
                        const auto copied = copy_spool(part, candidate,
                                                       checksum, copy_buffer);
                        spool_bytes = checked_add(spool_bytes, copied,
                                                  "merge spool bytes");
                        section_length = checked_add(section_length, copied,
                                                     "section length");
                }
                descriptor.length = section_length;
                descriptor.checksum = checksum.value();
                file_offset = checked_add(file_offset, section_length,
                                          "index file size");
        }
        header.file_size = file_offset;
        candidate.seekp(0);
        write_header(candidate, header);
        finish_output(candidate);

        const auto peak_additional_disk_bytes = checked_add(
                spool_bytes, header.file_size, "merge temporary bytes");
        for (const auto &parts : section_parts) {
                for (const auto &part : parts) {
                        remove_spool(part);
                }
        }

        return peak_additional_disk_bytes;
}

} // namespace snowseek::storage::detail
