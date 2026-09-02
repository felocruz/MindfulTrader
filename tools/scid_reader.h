// tools/scid_reader.h
// mmap'd Sierra Chart .scid tick decode: epoch conversion, benign-jitter
// resequencing, ask/bid/spread/trade_side derivation. Ported byte-for-byte
// from lbrnet/data/mes_continuous.py's decode_scid_ticks() -- see
// docs/superpowers/specs/2026-09-02-scid-tick-parquet-cpp-tool-spec.md §2.
//
// DOD: decode is a direct mmap read view over the raw file bytes (no
// std::vector<s_IntradayRecord> copy of the active window), binary-search
// windowing via LowerBoundUs (the C++ analog of np.searchsorted), and a
// caller-owned DecodedTickChunk reused (Reserve/Clear) across every
// on_chunk callback -- never a fresh heap-allocated chunk per callback.
#ifndef TOOLS_SCID_READER_H
#define TOOLS_SCID_READER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace scid {

// Matches lbrnet/data/mes_continuous.py's HEADER_SIZE (s_IntradayFileHeader's
// on-disk size, sierra_chart_dependencies/IntradayRecord.h).
constexpr std::size_t kHeaderSize = 56;

// 1899-12-30 -> 1970-01-01 in microseconds (mes_continuous.py's SC_EPOCH_OFFSET_US).
constexpr std::int64_t kScEpochOffsetUs = 2209161600000000;

// mes_continuous.py's _MAX_BENIGN_TIMESTAMP_JITTER_US: real exchange feed-handler
// jitter resequenced silently under this bound; anything past it is corruption.
constexpr std::int64_t kMaxBenignTimestampJitterUs = 1'000'000;

// POD overlay matching s_IntradayRecord's 40-byte on-disk layout
// (sierra_chart_dependencies/IntradayRecord.h): SCDateTimeMS is a plain
// int64_t microsecond count (scdatetime.h:1043), so this reinterprets
// cleanly with no padding on a little-endian host.
struct ScidRecordView {
    std::int64_t raw_timestamp;
    float open, high, low, close;
    std::uint32_t trades, volume, bid_volume, ask_volume;
};
static_assert(sizeof(ScidRecordView) == 40, "ScidRecordView must match .scid's 40-byte record stride");

// One chunk's worth of decoded, resequenced ticks -- caller owns one instance
// for a whole contract's decode, reserving once and clearing between chunks
// (context_to_parquet.cpp's ChunkBuffers pattern).
struct DecodedTickChunk {
    std::vector<std::int64_t> timestamp_us;
    std::vector<double> trade_price, ask_price, bid_price, spread;
    std::vector<std::int64_t> volume, bid_volume, ask_volume, trade_side;

    void Reserve(std::size_t n) {
        timestamp_us.reserve(n);
        trade_price.reserve(n);
        ask_price.reserve(n);
        bid_price.reserve(n);
        spread.reserve(n);
        volume.reserve(n);
        bid_volume.reserve(n);
        ask_volume.reserve(n);
        trade_side.reserve(n);
    }

    void Clear() {
        timestamp_us.clear();
        trade_price.clear();
        ask_price.clear();
        bid_price.clear();
        spread.clear();
        volume.clear();
        bid_volume.clear();
        ask_volume.clear();
        trade_side.clear();
    }

    std::size_t Rows() const { return timestamp_us.size(); }
};

// RAII mmap'd view over one .scid file's record region. Non-copyable,
// non-movable (each caller opens its own instance; no cross-thread transfer
// needed by any current consumer).
class ScidFileView {
public:
    explicit ScidFileView(const std::string& path) : path_(path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw std::runtime_error(path + ": failed to open");
        }
        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error(path + ": fstat failed");
        }
        file_size_ = static_cast<std::size_t>(st.st_size);
        if (file_size_ < kHeaderSize ||
            (file_size_ - kHeaderSize) % sizeof(ScidRecordView) != 0) {
            ::close(fd_);
            throw std::runtime_error(
                path + ": file size " + std::to_string(file_size_) +
                " does not decompose into a " + std::to_string(kHeaderSize) +
                "-byte header plus a whole number of " +
                std::to_string(sizeof(ScidRecordView)) + "-byte records");
        }
        n_records_ = (file_size_ - kHeaderSize) / sizeof(ScidRecordView);

        void* mapped = ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        ::close(fd_);  // mapping remains valid after fd close
        fd_ = -1;
        if (mapped == MAP_FAILED) {
            throw std::runtime_error(path + ": mmap failed");
        }
        base_ = static_cast<const char*>(mapped);
        data_ = base_ + kHeaderSize;
    }

    ~ScidFileView() {
        if (base_ != nullptr) {
            ::munmap(const_cast<char*>(base_), file_size_);
        }
    }

    ScidFileView(const ScidFileView&) = delete;
    ScidFileView& operator=(const ScidFileView&) = delete;
    ScidFileView(ScidFileView&&) = delete;
    ScidFileView& operator=(ScidFileView&&) = delete;

    const std::string& Path() const { return path_; }
    std::size_t RecordCount() const { return n_records_; }

    const ScidRecordView& operator[](std::size_t idx) const {
        return *reinterpret_cast<const ScidRecordView*>(data_ + idx * sizeof(ScidRecordView));
    }

    // First record index whose raw_timestamp >= (target_posix_us + kScEpochOffsetUs)
    // -- the C++ analog of np.searchsorted(..., side="left").
    std::size_t LowerBoundUs(std::int64_t target_posix_us) const {
        const std::int64_t target = target_posix_us + kScEpochOffsetUs;
        std::size_t lo = 0, hi = n_records_;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if ((*this)[mid].raw_timestamp < target) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    // Advisory only (madvise's return value is not load-bearing correctness --
    // callers still get correct decoded output regardless of kernel paging
    // behavior). Rounds the start address down to the page boundary since
    // madvise requires a page-aligned address.
    void AdviseRange(std::size_t lo, std::size_t hi, int advice) const {
        if (lo >= hi) return;
        const char* range_start = data_ + lo * sizeof(ScidRecordView);
        const char* range_end = data_ + hi * sizeof(ScidRecordView);
        const long page_size = ::sysconf(_SC_PAGESIZE);
        const auto start_addr = reinterpret_cast<std::uintptr_t>(range_start);
        const auto aligned_start = start_addr - (start_addr % static_cast<std::uintptr_t>(page_size));
        const std::size_t length =
            static_cast<std::size_t>(range_end - reinterpret_cast<const char*>(aligned_start));
        ::madvise(reinterpret_cast<void*>(aligned_start), length, advice);
    }

private:
    std::string path_;
    int fd_ = -1;
    const char* base_ = nullptr;
    const char* data_ = nullptr;
    std::size_t file_size_ = 0;
    std::size_t n_records_ = 0;
};

// Decodes [start_us, end_us) (end_us=nullopt means through end of file),
// resequencing benign out-of-order jitter (stable sort over the WHOLE
// window, matching the Python original's whole-window np.argsort, since a
// jitter pair can straddle a chunk boundary), and invokes on_chunk once per
// chunk_rows-sized batch via the caller-owned, Reserve()'d chunk buffer.
// Throws std::runtime_error if any backwards jump exceeds
// kMaxBenignTimestampJitterUs.
inline void DecodeScidTicks(
    const ScidFileView& file, std::int64_t start_us, std::optional<std::int64_t> end_us,
    std::size_t chunk_rows, DecodedTickChunk& chunk,
    const std::function<void(const DecodedTickChunk&)>& on_chunk) {
    const std::size_t lo = file.LowerBoundUs(start_us);
    const std::size_t hi = end_us.has_value() ? file.LowerBoundUs(*end_us) : file.RecordCount();
    if (lo >= hi) return;

    file.AdviseRange(lo, hi, MADV_SEQUENTIAL);

    // Pass 1: detect jitter/violations across the whole window without
    // materializing it -- O(1) extra memory for the common (no-jitter) case.
    bool has_jitter = false;
    bool has_violation = false;
    std::int64_t worst_violation_us = 0;
    std::int64_t prev_ts = file[lo].raw_timestamp;
    for (std::size_t i = lo + 1; i < hi; ++i) {
        const std::int64_t ts = file[i].raw_timestamp;
        const std::int64_t diff = ts - prev_ts;
        if (diff < 0) {
            has_jitter = true;
            const std::int64_t magnitude = -diff;
            if (magnitude > kMaxBenignTimestampJitterUs) {
                has_violation = true;
                worst_violation_us = std::max(worst_violation_us, magnitude);
            }
        }
        prev_ts = ts;
    }
    if (has_violation) {
        throw std::runtime_error(
            file.Path() + " has out-of-order timestamps exceeding the " +
            std::to_string(kMaxBenignTimestampJitterUs) +
            "us benign-jitter bound (worst backwards jump: " +
            std::to_string(worst_violation_us) + "us) -- this indicates corrupted or "
            "misaligned data, not benign feed jitter.");
    }

    // Only materialize an index-reorder buffer (bounded to this window's
    // size, not the whole file) when jitter actually needs resequencing.
    std::vector<std::size_t> order;
    if (has_jitter) {
        order.resize(hi - lo);
        std::iota(order.begin(), order.end(), lo);
        std::stable_sort(order.begin(), order.end(), [&file](std::size_t a, std::size_t b) {
            return file[a].raw_timestamp < file[b].raw_timestamp;
        });
    }

    for (std::size_t pos = 0; pos < hi - lo; ++pos) {
        const std::size_t idx = has_jitter ? order[pos] : (lo + pos);
        const ScidRecordView& rec = file[idx];
        const double high = rec.high;
        const double low = rec.low;
        chunk.timestamp_us.push_back(rec.raw_timestamp - kScEpochOffsetUs);
        chunk.trade_price.push_back(rec.close);
        chunk.ask_price.push_back(high);
        chunk.bid_price.push_back(low);
        chunk.spread.push_back(high - low);
        chunk.volume.push_back(rec.volume);
        chunk.bid_volume.push_back(rec.bid_volume);
        chunk.ask_volume.push_back(rec.ask_volume);
        chunk.trade_side.push_back(rec.ask_volume > 0 ? 1 : (rec.bid_volume > 0 ? -1 : 0));

        if (chunk.Rows() >= chunk_rows) {
            on_chunk(chunk);
            chunk.Clear();
        }
    }
    if (chunk.Rows() > 0) {
        on_chunk(chunk);
        chunk.Clear();
    }

    file.AdviseRange(lo, hi, MADV_DONTNEED);
}

}  // namespace scid

#endif  // TOOLS_SCID_READER_H
