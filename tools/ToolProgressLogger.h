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

#pragma once

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
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
    }

    ToolProgressLogger(const ToolProgressLogger&) = delete;
    ToolProgressLogger& operator=(const ToolProgressLogger&) = delete;

    // Writes one timestamped line (wall-clock HH:MM:SS + elapsed seconds
    // since construction) and flushes immediately, so `tail -f`/`cat` always
    // shows the true current state even if the process later crashes/hangs.
    // Also appended to the in-memory transcript archived on destruction --
    // any result/report a tool wants preserved must go through Log(), not a
    // bare std::printf/std::puts, or it will not survive (see directive above).
    void Log(const std::string& message) {
        char timeBuf[32];
        const std::string line = FormatLine(message, timeBuf, sizeof(timeBuf));
        m_transcript += line;
        m_transcript += '\n';
        if (!m_file) return;
        std::fprintf(m_file, "%s\n", line.c_str());
        std::fflush(m_file);
    }

    // Convenience: "processed N / ~M (P%)" -- pass 0 for totalEstimate if
    // unknown (e.g. streaming without a pre-counted row total).
    void LogProgress(std::size_t processed, std::size_t totalEstimate) {
        char buf[160];
        if (totalEstimate > 0) {
            const double pct = 100.0 * static_cast<double>(processed) / static_cast<double>(totalEstimate);
            std::snprintf(buf, sizeof(buf), "processed %zu / ~%zu (%.1f%%)", processed, totalEstimate, pct);
        } else {
            std::snprintf(buf, sizeof(buf), "processed %zu", processed);
        }
        Log(buf);
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
    }

    std::string m_toolName;
    std::string m_outputDir;
    std::string m_transcript;
    std::FILE* m_file = nullptr;
    std::chrono::steady_clock::time_point m_startTime;
};

