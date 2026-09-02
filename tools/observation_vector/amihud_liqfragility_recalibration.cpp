// amihud_liqfragility_recalibration.cpp -- tick-level replica for the
// required (not optional) FeatureScaler.h recalibration follow-on named in
// SCRATCHPAD.md/docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-
// vector-brainstorm.md §1.11: amihud_illiquidity/liq_fragility (dims 12/13)
// were just made live-reactive (StudyHelperFunctions.cpp, 2026-08-29),
// reading the current still-forming bar instead of the last closed one --
// the old winsorization bounds were calibrated against the OLD (lagging)
// distribution and no longer describe what these dims now measure.
//
// This tool replicates the NEW CalculateAmihudIlliquidity/
// CalculateLiquidityFragility logic (faithful reference port -- the real
// functions take SCStudyInterfaceRef and cannot be #included standalone,
// same constraint as tools/observation_vector/mean_rev_z_variant_comparison.cpp) against real
// MES 1-second bars, aggregated into 15-min bars for the ATR(10,Wilder)/
// VolumeSMA(21) reference series exactly as TripleScreen3.cpp computes them.
// Every raw per-tick value is fed through the REAL FeatureScaler
// (include/FeatureScaler.h, unmodified) via UpdateAndNormalize(), so the
// z-score/exceedance-rate statistics reported are against the actual
// production scaling code, not a re-derivation of it.
//
// Build: g++ -O2 -std=c++17 -Iinclude -I<vcpkg>/nlohmann tools/observation_vector/amihud_liqfragility_recalibration.cpp -o amihud_liqfragility_recalibration
// Usage: ./amihud_liqfragility_recalibration ticks_1s.bin [live_bar_min_volume]
//   ticks_1s.bin: [int64 count][int64 timestamp_us][float32 high][float32 low][float32 close][float32 volume]

#include "FeatureScaler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kLookbackN = 20;              // representative mid-point of the adaptive [10,40] range, same
                                             // precedent as tools/observation_vector/mean_rev_z_variant_comparison.cpp's kGateThreshold note
constexpr long long kBarUs = 15LL * 60 * 1'000'000; // 15-min bar bucket width in microseconds

struct Tick {
    long long timestamp_us;
    float high, low, close, volume;
};

template <typename T>
std::vector<T> ReadArray(std::ifstream& in, std::int64_t count) {
    std::vector<T> out(static_cast<std::size_t>(count));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count * static_cast<std::int64_t>(sizeof(T))));
    return out;
}

std::vector<Tick> LoadTicks(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::int64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    const auto ts = ReadArray<std::int64_t>(in, count);
    const auto high = ReadArray<float>(in, count);
    const auto low = ReadArray<float>(in, count);
    const auto close = ReadArray<float>(in, count);
    const auto volume = ReadArray<float>(in, count);

    std::vector<Tick> out(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        out[idx] = {ts[idx], high[idx], low[idx], close[idx], volume[idx]};
    }
    return out;
}

struct Bar {
    long long timestamp_us;
    float high, low, close, volume;
    float atr = 0.0f;        // Wilder's ATR(10)
    float volumeSma = 0.0f;  // SimpleMovAvg(volume, 21)
};

// Wilder's ATR(period), matching sc.ATR(sc.BaseDataIn, ..., 10, MOVAVGTYPE_WILDERS).
void ComputeAtrWilder(std::vector<Bar>& bars, int period) {
    if (bars.empty()) return;
    float prevAtr = 0.0f;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const float prevClose = (i == 0) ? bars[i].close : bars[i - 1].close;
        const float tr = std::max({bars[i].high - bars[i].low,
                                    std::fabs(bars[i].high - prevClose),
                                    std::fabs(bars[i].low - prevClose)});
        if (i == 0) {
            prevAtr = tr;
        } else {
            prevAtr = (prevAtr * (period - 1) + tr) / period;
        }
        bars[i].atr = prevAtr;
    }
}

void ComputeVolumeSma(std::vector<Bar>& bars, int period) {
    double sum = 0.0;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        sum += bars[i].volume;
        if (static_cast<int>(i) >= period) sum -= bars[i - period].volume;
        const int n = std::min<int>(period, static_cast<int>(i) + 1);
        bars[i].volumeSma = static_cast<float>(sum / n);
    }
}

// Faithful port of the NEW (2026-08-29, live-reactive) CalculateAmihudIlliquidity
// (StudyHelperFunctions.cpp) -- historical closed-bar window (i=1..lookback_n)
// plus a live term from the current tick's volume-so-far/live price, guarded
// on kLiveBarMinVolume.
float AmihudLive(const std::vector<Bar>& bars, std::size_t barIdx, double historicalSum, int historicalCount,
                  float liveVolumeSoFar, float livePrice, float liveBarMinVolume) {
    double sum = historicalSum;
    int count = historicalCount;
    const double prevClose = static_cast<double>(bars[barIdx - 1].close);
    if (liveVolumeSoFar >= liveBarMinVolume && livePrice > 0.0f && prevClose > 0.0) {
        const double dollarVol = static_cast<double>(livePrice) * static_cast<double>(liveVolumeSoFar);
        if (dollarVol >= 1.0) {
            const double logRet = std::fabs(std::log(static_cast<double>(livePrice) / prevClose));
            sum += logRet / dollarVol;
            ++count;
        }
    }
    if (count < 2) return 0.0f;  // cfc::ComputeAmihudIlliquidity's degenerate carry-forward -- no prior value at cold start
    return static_cast<float>(sum / count);
}

// Faithful port of the NEW (2026-08-29, live-reactive) CalculateLiquidityFragility.
float LiqFragilityLive(float liveHigh, float liveLow, float liveVolumeSoFar, float atrRef, float volumeSma,
                        float prevFragility, float liveBarMinVolume) {
    if (liveVolumeSoFar < liveBarMinVolume) {
        return std::clamp(prevFragility, 0.0f, 1.0f);
    }
    float barRange = liveHigh - liveLow;
    if (barRange < 0.00001f) barRange = 0.00001f;
    if (atrRef < 0.0001f) {
        return std::clamp(prevFragility, 0.0f, 1.0f);
    }
    const float rangeRatio = std::clamp(barRange / atrRef, 0.1f, 5.0f);
    const float logRatio = std::log(rangeRatio);
    const float rangeSignal = 1.0f / (1.0f + std::exp(-2.0f * logRatio));

    float thinness = 0.5f;
    if (volumeSma > 1.0f && liveVolumeSoFar > 0.0f) {
        const float volRatio = liveVolumeSoFar / volumeSma;
        thinness = std::clamp(1.5f - volRatio, 0.0f, 1.0f);
    }
    const float fragilityRaw = std::clamp(0.65f * rangeSignal + 0.35f * (rangeSignal * thinness), 0.0f, 1.0f);
    const float alpha = (fragilityRaw > prevFragility) ? 0.30f : 0.15f;
    return std::clamp(alpha * fragilityRaw + (1.0f - alpha) * prevFragility, 0.0f, 1.0f);
}

struct ZStats {
    std::size_t n = 0;
    double sumAbsZ = 0.0;
    float maxAbsZ = 0.0f;
    float madAtMaxZ = 0.0f;  // shrinkage-audit: local MAD scale at the single most extreme |z| event
    std::size_t hits6 = 0, hits25 = 0, hits45 = 0;
    std::vector<float> sample;    // for percentile reporting (downsampled)
    std::vector<float> madSample; // paired 1:1 with `sample` -- local MAD scale at that same tick

    void Record(float z, float localMad) {
        ++n;
        const float az = std::fabs(z);
        sumAbsZ += az;
        if (az > maxAbsZ) {
            maxAbsZ = az;
            madAtMaxZ = localMad;
        }
        if (az >= 6.0f) ++hits6;
        if (az >= 25.0f) ++hits25;
        if (az >= 45.0f) ++hits45;
        if (n % 50 == 0) {
            sample.push_back(az);       // downsample for percentile calc
            madSample.push_back(localMad);
        }
    }

    // Rate at an arbitrary threshold -- the actual production winsorization
    // bound in force for a dim isn't always one of the 6/25/45 fixed points
    // above (e.g. liq_fragility's real bound is LOGZ_WINSOR_SIGMA_OVERRIDE[13]=12.0).
    double RateAt(float threshold) const {
        // Reuse the downsampled `sample` (already |z| values) for an approximate
        // rate at an arbitrary threshold -- consistent sampling rate as the
        // percentile calc above, same statistical validity.
        if (sample.empty()) return 0.0;
        std::size_t hits = 0;
        for (float az : sample) {
            if (az >= threshold) ++hits;
        }
        return 100.0 * hits / sample.size();
    }

    void Report(const char* name, float productionBound) const {
        std::vector<float> sorted = sample;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) -> float {
            if (sorted.empty()) return 0.0f;
            std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
            return sorted[idx];
        };
        std::printf("%s: n=%zu  mean|z|=%.4f  max|z|=%.2f  p50=%.3f p90=%.3f p99=%.3f p99.9=%.3f\n",
                    name, n, sumAbsZ / std::max<std::size_t>(n, 1), maxAbsZ,
                    pct(0.50), pct(0.90), pct(0.99), pct(0.999));
        std::printf("%s: |z|>=6 rate=%.4f%%  |z|>=25 rate=%.4f%%  |z|>=45 rate=%.4f%%\n",
                    name, 100.0 * hits6 / std::max<std::size_t>(n, 1),
                    100.0 * hits25 / std::max<std::size_t>(n, 1),
                    100.0 * hits45 / std::max<std::size_t>(n, 1));
        std::printf("%s: REAL PRODUCTION BOUND=%.1f  rate-at-bound=%.4f%%\n",
                    name, productionBound, RateAt(productionBound));

        // Shrinkage-collapse audit: correlate local MAD scale against |z| for
        // the downsampled pairs. A genuine collapse signature (the pattern
        // that made dim3/dim9/dim7/dim0/dim6/dim4 need shrinkage) looks like
        // the most extreme |z| events clustering at anomalously SMALL local
        // MAD relative to its own typical value -- not a large raw deviation
        // on a normal-scale MAD (a real tail event, which needs a wider
        // winsorization bound, not shrinkage).
        if (!madSample.empty()) {
            std::vector<float> madSorted = madSample;
            std::sort(madSorted.begin(), madSorted.end());
            auto madPct = [&](double p) -> float {
                std::size_t idx = static_cast<std::size_t>(p * (madSorted.size() - 1));
                return madSorted[idx];
            };
            // Pearson correlation between local MAD and |z| across all downsampled pairs.
            double sumMad = 0, sumZ = 0;
            for (std::size_t k = 0; k < sample.size(); ++k) { sumMad += madSample[k]; sumZ += sample[k]; }
            const double meanMad = sumMad / sample.size(), meanZ = sumZ / sample.size();
            double cov = 0, varMad = 0, varZ = 0;
            for (std::size_t k = 0; k < sample.size(); ++k) {
                const double dm = madSample[k] - meanMad, dz = sample[k] - meanZ;
                cov += dm * dz; varMad += dm * dm; varZ += dz * dz;
            }
            const double corr = (varMad > 0 && varZ > 0) ? cov / std::sqrt(varMad * varZ) : 0.0;
            // Mean local MAD among the top 1% most extreme |z| events, vs. the overall median MAD.
            std::vector<std::size_t> idxSorted(sample.size());
            for (std::size_t k = 0; k < idxSorted.size(); ++k) idxSorted[k] = k;
            std::sort(idxSorted.begin(), idxSorted.end(), [&](std::size_t a, std::size_t b) { return sample[a] > sample[b]; });
            const std::size_t topN = std::max<std::size_t>(1, sample.size() / 100);
            double sumMadTop = 0;
            for (std::size_t k = 0; k < topN; ++k) sumMadTop += madSample[idxSorted[k]];
            const double meanMadTop = sumMadTop / topN;
            std::printf("%s: [shrinkage audit] corr(localMAD,|z|)=%.4f (negative = collapse signature)  "
                        "median_MAD=%.6g  mean_MAD_top1pct|z|=%.6g (ratio=%.4f)  MAD_at_max|z|=%.6g\n",
                        name, corr, madPct(0.50), meanMadTop, meanMadTop / std::max(madPct(0.50), 1e-30f), madAtMaxZ);
            std::printf("%s: [MAD percentiles, candidate SHRINKAGE_SCALE_MIN floor points] "
                        "p0.1=%.6g p1=%.6g p5=%.6g p10=%.6g p50=%.6g\n",
                        name, madPct(0.001), madPct(0.01), madPct(0.05), madPct(0.10), madPct(0.50));
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s ticks_1s.bin [zsample_dump_path] [guard1,guard2,...]\n", argv[0]);
        return 1;
    }
    // Multi-candidate mode: test several kLiveBarMinVolume guards in ONE tick pass
    // (empirical-tuning requirement, SCRATCHPAD.md) instead of re-reading 38.5M
    // ticks once per candidate. Default set spans the range explicitly named as
    // unvalidated: 5, 10 (current placeholder), 50.
    std::vector<float> guards = {5.0f, 10.0f, 50.0f};
    if (argc >= 4) {
        guards.clear();
        std::string s = argv[3];
        std::size_t pos = 0;
        while (pos < s.size()) {
            std::size_t comma = s.find(',', pos);
            guards.push_back(std::strtof(s.substr(pos, comma - pos).c_str(), nullptr));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }

    const auto ticks = LoadTicks(argv[1]);
    std::printf("loaded %zu 1-second ticks; testing guards:", ticks.size());
    for (float g : guards) std::printf(" %.1f", g);
    std::printf("\n");

    // --- Aggregate into 15-min bars ---
    std::vector<Bar> bars;
    std::vector<std::size_t> barStartTickIdx;  // first tick index belonging to each bar
    long long curBucket = -1;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const long long bucket = ticks[i].timestamp_us / kBarUs;
        if (bucket != curBucket) {
            curBucket = bucket;
            bars.push_back({ticks[i].timestamp_us, ticks[i].high, ticks[i].low, ticks[i].close, ticks[i].volume});
            barStartTickIdx.push_back(i);
        } else {
            Bar& b = bars.back();
            b.high = std::max(b.high, ticks[i].high);
            b.low = std::min(b.low, ticks[i].low);
            b.close = ticks[i].close;
            b.volume += ticks[i].volume;
        }
    }
    barStartTickIdx.push_back(ticks.size());  // sentinel end
    std::printf("aggregated into %zu 15-min bars\n", bars.size());

    ComputeAtrWilder(bars, 10);
    ComputeVolumeSma(bars, 21);

    struct Candidate {
        float guard;
        FeatureScaler fs;
        ZStats amihudStats, liqFragStats;
        float prevFragility = 0.0f;
    };
    std::vector<Candidate> cands;
    for (float g : guards) cands.push_back({g});

    constexpr int kWarmupBars = 100;
    for (std::size_t barIdx = kWarmupBars; barIdx + 1 < bars.size(); ++barIdx) {
        // Historical Amihud window (i=1..kLookbackN closed bars), recomputed once per bar --
        // independent of the guard (only the live term depends on it).
        double historicalSum = 0.0;
        int historicalCount = 0;
        for (int i = 1; i <= kLookbackN; ++i) {
            if (static_cast<int>(barIdx) - i < 1) break;
            const auto& cur = bars[barIdx - i];
            const auto& prev = bars[barIdx - i - 1];
            if (cur.volume < 1.0f || cur.close <= 0.0f || prev.close <= 0.0f) continue;
            const double dollarVol = static_cast<double>(cur.close) * static_cast<double>(cur.volume);
            if (dollarVol < 1.0) continue;
            const double logRet = std::fabs(std::log(static_cast<double>(cur.close) / prev.close));
            historicalSum += logRet / dollarVol;
            ++historicalCount;
        }

        const float atrRef = bars[barIdx].atr;
        const float volumeSma = bars[barIdx].volumeSma;

        float liveHigh = -1e30f, liveLow = 1e30f, liveVolumeSoFar = 0.0f;
        for (std::size_t t = barStartTickIdx[barIdx]; t < barStartTickIdx[barIdx + 1]; ++t) {
            liveHigh = std::max(liveHigh, ticks[t].high);
            liveLow = std::min(liveLow, ticks[t].low);
            liveVolumeSoFar += ticks[t].volume;
            const float livePrice = ticks[t].close;

            for (auto& c : cands) {
                const float amihud = AmihudLive(bars, barIdx, historicalSum, historicalCount, liveVolumeSoFar, livePrice, c.guard);
                const float liqFragility = LiqFragilityLive(liveHigh, liveLow, liveVolumeSoFar, atrRef, volumeSma, c.prevFragility, c.guard);

                std::array<float, FeatureScaler::N_DIMS> obs{};
                obs[12] = amihud;
                obs[13] = liqFragility;
                c.fs.UpdateAndNormalize(obs);
                // lastLocalMad[12] now populated unconditionally (FeatureScaler.h,
                // 2026-08-29 diagnostic addition) for the shrinkage-collapse audit.
                c.amihudStats.Record(c.fs.lastRawZ[12], c.fs.lastLocalMad[12]);
                // liq_fragility (dim 13) is a LOGZ dim. lastRawZ[13] is now populated
                // unconditionally by the LOGZ branch itself (FeatureScaler.h, 2026-08-29
                // diagnostic addition) with the REAL shrinkage-blended z -- reading it
                // directly (instead of manually recomputing the plain formula from
                // latestLogMedian/latestLogScale, which silently bypassed
                // ComputeShrinkageZ() and made the prior audit run blind to
                // SHRINKAGE_SCALE_MIN[13]) is what makes this audit shrinkage-aware.
                c.liqFragStats.Record(c.fs.lastRawZ[13], c.fs.latestLogScale[13]);
            }
        }
        // Bar-close value becomes next bar's prev_fragility, matching the real EMA anchor.
        for (auto& c : cands) {
            c.prevFragility = LiqFragilityLive(liveHigh, liveLow, liveVolumeSoFar, atrRef, volumeSma, c.prevFragility, c.guard);
        }
    }

    for (auto& c : cands) {
        std::printf("\n=== guard=%.1f -- amihud_illiquidity (dim 12, SOFTLOGZ) ===\n", c.guard);
        c.amihudStats.Report("amihud", FeatureScaler::STATE_WINSOR_SIGMA);
        std::printf("\n=== guard=%.1f -- liq_fragility (dim 13, LOGZ) ===\n", c.guard);
        c.liqFragStats.Report("liq_fragility", FeatureScaler::LOGZ_WINSOR_SIGMA_OVERRIDE[13]);
    }

    // Dump the FIRST candidate's (guards[0], the current placeholder=10 unless
    // overridden) |z| samples for the GPD/EVT bound fit -- same file as before.
    ZStats& amihudStats = cands[0].amihudStats;
    ZStats& liqFragStats = cands[0].liqFragStats;

    // Dump downsampled |z| samples for a proper GPD/EVT tail fit in Python --
    // same Pickands-Balkema-de Haan methodology already used for every other
    // dim's winsorization bound in this project (Gang doc, D6/D7/Task 3/4).
    if (argc >= 3) {
        const std::string dumpPath = argv[2];
        std::ofstream out(dumpPath);
        out << "dim,abs_z\n";
        for (float az : amihudStats.sample) out << "amihud," << az << "\n";
        for (float az : liqFragStats.sample) out << "liq_fragility," << az << "\n";
        std::printf("\ndumped %zu+%zu |z| samples to %s\n", amihudStats.sample.size(), liqFragStats.sample.size(), dumpPath.c_str());
    }
    return 0;
}
