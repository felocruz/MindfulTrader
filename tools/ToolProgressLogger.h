// ToolProgressLogger.h -- shared, minimal progress-logging utility for
// standalone tools/ executables (tools/observation_vector/, tools/
// scid_processing/, tools/context_pipeline/). Writes timestamped lines to
// tools/log/<toolName>.log (gitignored, regenerable) so a long-running
// recalibration/analysis tool's progress can be checked (tail -f, cat)
// without sitting and watching the terminal for it to finish -- the whole
// point is assessing progress mid-run, not just a post-mortem record.
//
// Convention: every tools/ executable that can run longer than a few seconds
// should construct one of these near the top of main() (toolName matching
// the executable's own name) and call Log()/LogProgress() at a reasonable
// cadence (the class does no throttling itself -- callers decide, e.g.
// "every 5,000,000 ticks", to avoid flooding the log file).
//
// TOP-LEVEL DIRECTIVE (2026-09-03): tool output must never depend on a
// terminal's scrollback surviving -- a real ~1h43m real-tick-data validation
// run's only report was lost this way (printed via raw stdout, terminal
// closed/cleared before it was read) and had to be re-run from scratch.
// To make this structural, not a per-tool discipline problem: every line
// passed to Log() is also appended to an in-memory transcript, which this
// class writes out ITSELF on destruction to tools/output/<toolName>_
// <timestamp>.txt -- a permanent, uniquely-timestamped archive, never
// truncated/overwritten across runs (unlike tools/log/'s live-progress
// file). No extra code is required per-tool for this guarantee; it is
// automatic for anything already using ToolProgressLogger.
//
// TOP-LEVEL DIRECTIVE (2026-09-04): an archived tools/output/*.txt is USELESS
// if nobody ever looks at it -- found 2026-09-04 that 3 completed recalibration
// archives sat unused for hours despite containing real, actionable findings
// (only noticed by chance while investigating an unrelated question). Root
// cause: tools/output/ is entirely gitignored, so nothing in it is ever
// visible to `git status`/diffs/code review -- the only way to "notice" a
// finding was to remember it existed, which does not scale. Fix: this class
// now ALSO appends one row per run to tools/RECALIBRATION_LEDGER.md (git-
// tracked, unlike tools/output/ itself), automatically, on destruction --
// same "no extra code required per-tool" guarantee as the archive above.
// Every session touching tools/observation_vector/ MUST check that ledger's
// PENDING rows before launching a new heavy pass (a pending row may already
// answer the question a new run would re-derive at real compute cost).

#pragma once

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

class ToolProgressLogger {
public:
    // toolName should match the executable's own name (e.g.
    // "amihud_liqfragility_recalibration") so tools/log/<toolName>.log is
    // unambiguous about which run produced it. Truncates any prior log from
    // an earlier run of the same tool -- this is a live-progress file, not
    // an append-forever history (tools/output/'s timestamped archive is the
    // history; see class-level comment above).
    explicit ToolProgressLogger(const std::string& toolName,
                                 const std::string& logDir = "tools/log",
                                 const std::string& outputDir = "tools/output")
        : m_toolName(toolName), m_outputDir(outputDir) {
        std::filesystem::create_directories(logDir);
        std::filesystem::create_directories(outputDir);
        const std::string path = logDir + "/" + toolName + ".log";
        m_file = std::fopen(path.c_str(), "w");
        m_startTime = std::chrono::steady_clock::now();
        if (m_file) {
            Log("=== " + toolName + " started ===");
        } else {
            std::fprintf(stderr, "ToolProgressLogger: could not open %s for writing (dir missing?)\n", path.c_str());
        }
    }

    ~ToolProgressLogger() {
        if (m_file) {
            Log("=== finished ===");
            std::fclose(m_file);
        }
        ArchiveTranscript();
        AppendLedgerEntry();
    }

    ToolProgressLogger(const ToolProgressLogger&) = delete;
    ToolProgressLogger& operator=(const ToolProgressLogger&) = delete;

    // Optional, set once near the top of main() after parsing argv (e.g.
    // SetScope(dimsFlag)) -- appears in the ledger row so a PENDING entry is
    // actionable without opening the archive first. Defaults to "unspecified"
    // if never called; not required for the ledger guarantee to apply.
    void SetScope(const std::string& scope) { m_scope = scope; }

    // Writes one timestamped line (wall-clock HH:MM:SS + elapsed seconds
    // since construction) and flushes immediately, so `tail -f`/`cat` always
    // shows the true current state even if the process later crashes/hangs.
    // Also appended to the in-memory transcript archived on destruction --
    // any result/report a tool wants preserved must go through Log(), not a
    // bare std::printf/std::puts, or it will not survive (see directive above).
    // Thread-safe (mutex-guarded): some tools/ executables decode/process in
    // parallel (e.g. scid_to_ticks_parquet.cpp's per-contract worker threads) --
    // a bare std::string append here would race across threads.
    void Log(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        char timeBuf[32];
        const std::string line = FormatLine(message, timeBuf, sizeof(timeBuf));
        m_transcript += line;
        m_transcript += '\n';
        if (!m_file) return;
        std::fprintf(m_file, "%s\n", line.c_str());
        std::fflush(m_file);
    }

    // Convenience: "processed N / ~M (P%) rss=X MB" -- pass 0 for totalEstimate
    // if unknown (e.g. streaming without a pre-counted row total). Always
    // includes current RSS: a real ~1h43m run was OOM-killed (VS Code/WSL
    // taken down with it, 2026-09-03) with a log full of progress percentages
    // and zero memory visibility -- useless for telling "working normally" from
    // "about to be killed". RSS in every checkpoint line makes that visible.
    void LogProgress(std::size_t processed, std::size_t totalEstimate) {
        char buf[200];
        const std::size_t rssMb = CurrentRssKB() / 1024;
        if (totalEstimate > 0) {
            const double pct = 100.0 * static_cast<double>(processed) / static_cast<double>(totalEstimate);
            std::snprintf(buf, sizeof(buf), "processed %zu / ~%zu (%.1f%%) rss=%zuMB", processed, totalEstimate, pct, rssMb);
        } else {
            std::snprintf(buf, sizeof(buf), "processed %zu rss=%zuMB", processed, rssMb);
        }
        Log(buf);
    }

    // Reads this process's own resident set size from /proc/self/status.
    // Returns 0 if unavailable (e.g. non-Linux) rather than throwing --
    // memory reporting is a diagnostic nicety, must never crash the tool.
    static std::size_t CurrentRssKB() {
        std::ifstream status("/proc/self/status");
        if (!status.is_open()) return 0;
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream iss(line.substr(6));
                std::size_t kb = 0;
                iss >> kb;
                return kb;
            }
        }
        return 0;
    }

    // Self-imposed memory ceiling for long-running tools -- call at the same
    // cadence as LogProgress. Throws (not exit()/abort()) so main()'s own
    // try/catch can log the fatal cause and let this object's destructor run
    // normally, archiving the full transcript instead of losing it to a raw
    // OOM-kill (see class-level directive). Callers running several dim-group
    // passes concurrently should pass a correspondingly lower budget, since
    // the OS memory ceiling is shared across all of them.
    void CheckMemoryBudget(std::size_t maxRssMB) {
        const std::size_t rssMb = CurrentRssKB() / 1024;
        if (rssMb > maxRssMB) {
            const std::string msg = "FATAL: RSS " + std::to_string(rssMb) + "MB exceeded budget " +
                                     std::to_string(maxRssMB) + "MB -- aborting before OOM-kill";
            Log(msg);
            throw std::runtime_error(m_toolName + ": " + msg);
        }
    }

private:
    std::string FormatLine(const std::string& message, char* timeBuf, std::size_t timeBufSize) {
        const auto now = std::chrono::system_clock::now();
        const std::time_t nowC = std::chrono::system_clock::to_time_t(now);
        std::strftime(timeBuf, timeBufSize, "%H:%M:%S", std::localtime(&nowC));
        const double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count();
        char lineBuf[512];
        std::snprintf(lineBuf, sizeof(lineBuf), "[%s +%.1fs] %s", timeBuf, elapsedSec, message.c_str());
        return std::string(lineBuf);
    }

    // Writes the full accumulated transcript to a uniquely-timestamped file
    // under tools/output/ -- this is the permanent record; tools/log/ is
    // only ever the live-progress view of the same content.
    void ArchiveTranscript() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t nowC = std::chrono::system_clock::to_time_t(now);
        char stampBuf[32];
        std::strftime(stampBuf, sizeof(stampBuf), "%Y%m%d_%H%M%S", std::localtime(&nowC));
        const std::string path = m_outputDir + "/" + m_toolName + "_" + stampBuf + ".txt";
        std::FILE* out = std::fopen(path.c_str(), "w");
        if (!out) {
            std::fprintf(stderr, "ToolProgressLogger: could not archive output to %s\n", path.c_str());
            return;
        }
        std::fwrite(m_transcript.data(), 1, m_transcript.size(), out);
        std::fclose(out);
        m_archivedPath = path;
    }

    // Appends one row to the git-tracked tools/RECALIBRATION_LEDGER.md so this
    // run's archive can never again go unnoticed -- see the class-level
    // TOP-LEVEL DIRECTIVE (2026-09-04) above. Creates the ledger with a header
    // if it doesn't exist yet. Append-only, single short line -- safe under
    // POSIX O_APPEND semantics even with a few concurrent tool runs (this
    // repo's own established "no CI gate, loudly logged, never silent"
    // convention; not worth a lock file for a solo-dev tool).
    void AppendLedgerEntry() {
        constexpr const char* kLedgerPath = "tools/RECALIBRATION_LEDGER.md";
        const bool exists = std::filesystem::exists(kLedgerPath);
        std::ofstream ledger(kLedgerPath, std::ios::app);
        if (!ledger.is_open()) {
            std::fprintf(stderr, "ToolProgressLogger: could not open %s for the ledger entry\n", kLedgerPath);
            return;
        }
        if (!exists) {
            ledger << "# Recalibration Ledger\n\n"
                      "Auto-appended by ToolProgressLogger on every tools/ run -- see its own\n"
                      "2026-09-04 TOP-LEVEL DIRECTIVE. **Check PENDING REVIEW rows before launching\n"
                      "a new heavy recalibration pass**; a pending row may already answer the question.\n"
                      "Update the Status column by hand once a row's finding is actually consumed\n"
                      "(e.g. `ACTED ON (commit abc1234, 2026-09-05)`) -- this file is git-tracked\n"
                      "specifically so that edit shows up in normal code review, unlike the archives\n"
                      "themselves (tools/output/ is gitignored).\n\n"
                      "| Date | Tool | Scope | Archive | Status |\n"
                      "|------|------|-------|---------|--------|\n";
        }
        char timeBuf[32];
        const auto now = std::chrono::system_clock::now();
        const std::time_t nowC = std::chrono::system_clock::to_time_t(now);
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", std::localtime(&nowC));
        ledger << "| " << timeBuf << " | " << m_toolName << " | " << (m_scope.empty() ? "unspecified" : m_scope)
               << " | " << (m_archivedPath.empty() ? "(archive write failed)" : m_archivedPath)
               << " | PENDING REVIEW |\n";
    }

    std::string m_toolName;
    std::string m_outputDir;
    std::string m_transcript;
    std::string m_scope;
    std::string m_archivedPath;
    std::FILE* m_file = nullptr;
    std::chrono::steady_clock::time_point m_startTime;
    std::mutex m_mutex;
};

