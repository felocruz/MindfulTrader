// tools/context_pipeline/context_cache_key.h
// Freshness/rebuild-decision logic, mirroring lbrnet's
// materialize_context_parquet.py::cache_key()/is_cache_fresh() exactly -- but
// correctly SEPARATES two versioning concepts that script's own "schema_version"
// field conflated under one name: the raw wire format (kSchemaVersion, gates
// ObservationData's byte layout, see context_reader.h's hard-refuse) vs. this
// tool's own Parquet OUTPUT column schema (kContextParquetCacheFormatVersion,
// bump when BuildArrowSchema()'s column set/naming changes). Pure decision
// function is unit-tested without file I/O; DecideRebuildPlan() is the thin
// file-reading wrapper, matching context_reader.h's own pure/glue precedent.
#pragma once

#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>

inline constexpr int kContextParquetCacheFormatVersion = 1;

struct FileStat {
    std::int64_t size = 0;
    std::int64_t mtime = 0;
};

inline FileStat StatFile(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        throw std::runtime_error("StatFile: cannot stat " + path);
    }
    return {static_cast<std::int64_t>(st.st_size), static_cast<std::int64_t>(st.st_mtime)};
}

struct ContextCacheKey {
    std::int64_t context_size = 0;
    std::int64_t context_mtime = 0;
    std::uint16_t wire_schema_version = 0;
    int parquet_cache_format_version = 0;
    std::size_t resume_offset = 0;
    std::uint64_t last_seq_id = 0;
    bool has_last_seq_id = false;
};

inline void WriteCacheKey(const std::string& meta_path, const ContextCacheKey& key) {
    std::ofstream out(meta_path);
    out << "{\n"
        << "  \"context_size\": " << key.context_size << ",\n"
        << "  \"context_mtime\": " << key.context_mtime << ",\n"
        << "  \"wire_schema_version\": " << key.wire_schema_version << ",\n"
        << "  \"parquet_cache_format_version\": " << key.parquet_cache_format_version << ",\n"
        << "  \"resume_offset\": " << key.resume_offset << ",\n"
        << "  \"last_seq_id\": "
        << (key.has_last_seq_id ? std::to_string(key.last_seq_id) : std::string("null")) << "\n"
        << "}\n";
}

// Minimal hand-rolled JSON field extraction -- the sidecar schema is small and
// fixed (6 known scalar fields), so a tiny dedicated parser is simpler and more
// appropriate here than pulling in a JSON library for a standalone tool.
inline std::optional<ContextCacheKey> ReadCacheKey(const std::string& meta_path) {
    std::ifstream in(meta_path);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();

    auto extract = [&](const std::string& field) -> std::optional<std::string> {
        const std::string needle = "\"" + field + "\":";
        auto pos = text.find(needle);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        pos += needle.size();
        auto end = text.find_first_of(",\n}", pos);
        std::string value = text.substr(pos, end - pos);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    };

    auto size_str = extract("context_size");
    auto mtime_str = extract("context_mtime");
    auto wire_str = extract("wire_schema_version");
    auto fmt_str = extract("parquet_cache_format_version");
    auto resume_str = extract("resume_offset");
    auto seq_str = extract("last_seq_id");
    if (!size_str || !mtime_str || !wire_str || !fmt_str || !resume_str) {
        return std::nullopt;
    }

    ContextCacheKey key;
    key.context_size = std::stoll(*size_str);
    key.context_mtime = std::stoll(*mtime_str);
    key.wire_schema_version = static_cast<std::uint16_t>(std::stoi(*wire_str));
    key.parquet_cache_format_version = std::stoi(*fmt_str);
    key.resume_offset = static_cast<std::size_t>(std::stoull(*resume_str));
    if (seq_str && *seq_str != "null") {
        key.last_seq_id = std::stoull(*seq_str);
        key.has_last_seq_id = true;
    }
    return key;
}

enum class RebuildPlan { kFresh, kIncremental, kFull };

// Pure decision, no file I/O -- mirrors materialize_context_parquet.py's
// is_cache_fresh()/incremental-vs-full logic exactly, including its careful
// "only stat-derived fields participate in freshness" rule (resume_offset/
// last_seq_id are bookkeeping, never compared here).
inline RebuildPlan DecideRebuildPlanFromStats(
    FileStat current, const std::optional<ContextCacheKey>& prior,
    std::uint16_t current_wire_schema_version, bool parquet_exists) {
    if (!parquet_exists || !prior.has_value()) {
        return RebuildPlan::kFull;
    }
    const bool same_schema = prior->wire_schema_version == current_wire_schema_version
        && prior->parquet_cache_format_version == kContextParquetCacheFormatVersion;
    if (!same_schema) {
        return RebuildPlan::kFull;
    }
    if (current.size == prior->context_size && current.mtime == prior->context_mtime) {
        return RebuildPlan::kFresh;
    }
    if (current.size > prior->context_size) {
        return RebuildPlan::kIncremental;
    }
    return RebuildPlan::kFull;
}

inline RebuildPlan DecideRebuildPlan(
    const std::string& context_path, const std::string& parquet_path,
    const std::string& meta_path, std::uint16_t current_wire_schema_version) {
    const bool parquet_exists = std::ifstream(parquet_path).good();
    const auto prior = ReadCacheKey(meta_path);
    const auto current = StatFile(context_path);
    return DecideRebuildPlanFromStats(current, prior, current_wire_schema_version, parquet_exists);
}
