#pragma once

#include "generated/mts_schema_generated.h"
#include "flatbuffers/flatbuffers.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace MTS {
namespace Schema {
namespace Contract {

inline constexpr std::size_t kObservationDim = 18;

inline constexpr std::size_t kObsLogScaleRatio = 0;
inline constexpr std::size_t kObsBurstinessIndex = 1;
inline constexpr std::size_t kObsRelativeRange = 2;
inline constexpr std::size_t kObsLogScaleExpansionRatio = 3;
inline constexpr std::size_t kObsLempelZiv = 4;
inline constexpr std::size_t kObsHurstExponent = 5;
inline constexpr std::size_t kObsMicroAsymmetry = 6;
inline constexpr std::size_t kObsFisherInfo = 7;
inline constexpr std::size_t kObsFastHurstExponent = 8;
inline constexpr std::size_t kObsTailIndex = 9;
inline constexpr std::size_t kObsSkewnessIdx = 10;
inline constexpr std::size_t kObsAmihudIlliquidity = 11;
inline constexpr std::size_t kObsLiqFragility = 12;
inline constexpr std::size_t kObsFastTalebKurtosis = 13;
inline constexpr std::size_t kObsRecurrenceRate = 14;
inline constexpr std::size_t kObsFractalDim = 15;
inline constexpr std::size_t kObsMeanRevZ = 16;
inline constexpr std::size_t kObsFastMeanRevZ = 17;

inline constexpr std::array<const char*, kObservationDim> kObservationFieldNames = {
    "log_scale_ratio",
    "burstiness_index",
    "relative_range",
    "log_scale_expansion_ratio",
    "lempel_ziv",
    "hurst_exponent",
    "micro_asymmetry",
    "fisher_info",
    "fast_hurst_exponent",
    "tail_index",
    "skewness_idx",
    "amihud_illiquidity",
    "liq_fragility",
    "fast_taleb_kurtosis",
    "recurrence_rate",
    "fractal_dim",
    "mean_rev_z",
    "fast_mean_rev_z",
};
inline constexpr std::uint16_t kSchemaVersion = 240;  // from mts_schema.fbs's WIRE_SCHEMA_VERSION marker, see brainstorm doc §10.2
inline constexpr std::size_t kAsymmetryDim = 8;

using ObservationArray = std::array<float, kObservationDim>;
using AsymmetryArray = std::array<float, kAsymmetryDim>;

inline constexpr std::size_t kAsymShannonEntropy = 0;
inline constexpr std::size_t kAsymShannonEfficiency = 1;
inline constexpr std::size_t kAsymTalebKurtosis = 2;
inline constexpr std::size_t kAsymTalebSkewness = 3;
inline constexpr std::size_t kAsymTalebCliff = 4;
inline constexpr std::size_t kAsymParetoRot = 5;
inline constexpr std::size_t kAsymRaschkeBurst = 6;
inline constexpr std::size_t kAsymSessionQualityScore = 7;

inline constexpr std::array<const char*, kAsymmetryDim> kAsymmetryFieldNames = {
    "shannon_entropy",
    "shannon_efficiency",
    "taleb_kurtosis",
    "taleb_skewness",
    "taleb_cliff",
    "pareto_rot",
    "raschke_burst",
    "session_quality_score",
};

inline constexpr std::size_t kRiskGateFieldCount = 18;
inline constexpr std::size_t kRiskGateFloatFieldCount = 15;

inline constexpr std::array<const char*, kRiskGateFieldCount> kRiskGateFieldNames = {
    "shannon_flow_entropy",
    "shannon_efficiency",
    "taleb_kurtosis",
    "taleb_skewness",
    "elder_chandelier_atr",
    "vol_convexity",
    "pareto_tail_alpha",
    "amihud_illiquidity",
    "spread_stress",
    "hurst_exponent",
    "fractal_dim",
    "mean_rev_z",
    "raschke_burst",
    "fisher_info",
    "regime_duration",
    "is_valid",
    "snapshot_timestamp_us",
    "amihud_percentile",
};

inline constexpr std::array<const char*, kRiskGateFloatFieldCount> kRiskGateFloatFieldNames = {
    "shannon_flow_entropy",
    "shannon_efficiency",
    "taleb_kurtosis",
    "taleb_skewness",
    "elder_chandelier_atr",
    "vol_convexity",
    "pareto_tail_alpha",
    "amihud_illiquidity",
    "spread_stress",
    "hurst_exponent",
    "fractal_dim",
    "mean_rev_z",
    "raschke_burst",
    "fisher_info",
    "amihud_percentile",
};

// Output-column names for RiskGateContext's float fields -- suffixed with
// _raw ONLY where the plain name collides with an ObservationData column
// (raw/unscaled here vs. log-z/winsorized there), computed by set
// intersection against kObservationFieldNames, not hardcoded (§10.4).
inline constexpr std::array<const char*, kRiskGateFloatFieldCount> kRiskGateFloatOutputColumnNames = {
    "shannon_flow_entropy",
    "shannon_efficiency",
    "taleb_kurtosis",
    "taleb_skewness",
    "elder_chandelier_atr",
    "vol_convexity",
    "pareto_tail_alpha",
    "amihud_illiquidity_raw",
    "spread_stress",
    "hurst_exponent_raw",
    "fractal_dim_raw",
    "mean_rev_z_raw",
    "raschke_burst",
    "fisher_info_raw",
    "amihud_percentile",
};

inline constexpr std::uint16_t kConfigDefaultMaxIndicators = 50;
inline constexpr std::uint16_t kConfigDefaultFeatureVectorSize = 42;

using EnvelopeMessage = MTS::Schema::Message;
using RoutingMessageType = MTS::Schema::MessageType;

inline constexpr EnvelopeMessage kEnvelopeHeartbeat = MTS::Schema::Message_Heartbeat;
inline constexpr EnvelopeMessage kEnvelopeDiagnostic = MTS::Schema::Message_Diagnostic;
inline constexpr EnvelopeMessage kEnvelopePreFlightCheckRequest = MTS::Schema::Message_PreFlightCheckRequest;
inline constexpr EnvelopeMessage kEnvelopePreFlightCheckResponse = MTS::Schema::Message_PreFlightCheckResponse;
inline constexpr EnvelopeMessage kEnvelopeConfigResponse = MTS::Schema::Message_ConfigResponse;
inline constexpr EnvelopeMessage kEnvelopeTradeRequest = MTS::Schema::Message_TradeRequest;
inline constexpr EnvelopeMessage kEnvelopeTradeResponse = MTS::Schema::Message_TradeResponse;
inline constexpr EnvelopeMessage kEnvelopeModelPrediction = MTS::Schema::Message_ModelPrediction;
inline constexpr EnvelopeMessage kEnvelopePositionUpdate = MTS::Schema::Message_PositionUpdate;

inline constexpr RoutingMessageType kRoutingHeartbeat = MTS::Schema::MessageType_HEARTBEAT;
inline constexpr RoutingMessageType kRoutingObservation = MTS::Schema::MessageType_OBSERVATION;
inline constexpr RoutingMessageType kRoutingHmmCommand = MTS::Schema::MessageType_HMM_COMMAND;
inline constexpr RoutingMessageType kRoutingDiagnostic = MTS::Schema::MessageType_DIAGNOSTIC;

inline constexpr std::size_t kHeartbeatFieldCount = 11;
inline constexpr std::size_t kBuilderCapacityPreFlightResponse = 512;
inline constexpr std::size_t kBuilderCapacityHeartbeat = 512;
inline constexpr std::size_t kBuilderCapacityDiagnostic = 256;
inline constexpr std::size_t kBuilderCapacityTradeRequest = 1024;
inline constexpr std::size_t kBuilderCapacityTelemetryDiagnostic = 512;

inline int64_t NowTimestampUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline const char* EnvelopeMessageName(EnvelopeMessage message) {
    return MTS::Schema::EnumNameMessage(message);
}

inline const char* RoutingMessageTypeName(RoutingMessageType type) {
    return MTS::Schema::EnumNameMessageType(type);
}

inline bool VerifyEnvelopeBuffer(const void* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }
    flatbuffers::Verifier verifier(static_cast<const std::uint8_t*>(data), size);
    return verifier.VerifyBuffer<MTS::Schema::MTS_Envelope>(nullptr);
}

inline const MTS::Schema::MTS_Envelope* GetVerifiedEnvelope(const void* data, std::size_t size) {
    if (!VerifyEnvelopeBuffer(data, size)) {
        return nullptr;
    }
    return ::flatbuffers::GetRoot<MTS::Schema::MTS_Envelope>(static_cast<const std::uint8_t*>(data));
}

inline flatbuffers::Offset<MTS::Schema::MTS_Envelope> BuildEnvelope(
    flatbuffers::FlatBufferBuilder& fbb,
    EnvelopeMessage data_type,
    flatbuffers::Offset<void> data_union) {
    MTS::Schema::MTS_EnvelopeBuilder env_builder(fbb);
    env_builder.add_data_type(data_type);
    env_builder.add_data(data_union);
    return env_builder.Finish();
}

inline MTS::Schema::ObservationData MakeObservationData(
    const ObservationArray& values) {
    MTS::Schema::ObservationData obs;
    std::memcpy(&obs, values.data(), sizeof(obs));
    return obs;
}

inline ObservationArray ToObservationArray(
    const MTS::Schema::ObservationData& observation) {
    ObservationArray out;
    std::memcpy(out.data(), &observation, sizeof(observation));
    return out;
}

// ObservationData/AsymmetryContext are FlatBuffers structs: standard-layout,
// exactly kObservationDim/kAsymmetryDim * sizeof(float) bytes (asserted
// below), same field order as ObservationArray/AsymmetryArray -- a single
// memcpy is bit-identical to (and faster than) per-field accessor copies,
// and removes one of the three hand-maintained field-order duplication
// points (the .fbs declaration, the kObsXxx/kAsymXxx indices above, and
// this function's own former explicit per-field listing).
inline MTS::Schema::AsymmetryContext MakeAsymmetryContext(
    const AsymmetryArray& values) {
    MTS::Schema::AsymmetryContext ctx;
    std::memcpy(&ctx, values.data(), sizeof(ctx));
    return ctx;
}

inline AsymmetryArray ToAsymmetryArray(
    const MTS::Schema::AsymmetryContext& asymmetry) {
    AsymmetryArray out;
    std::memcpy(out.data(), &asymmetry, sizeof(asymmetry));
    return out;
}

static_assert(std::is_standard_layout<MTS::Schema::ObservationData>::value,
              "ObservationData must remain a standard-layout FlatBuffers struct");
static_assert(std::is_standard_layout<MTS::Schema::AsymmetryContext>::value,
              "AsymmetryContext must remain a standard-layout FlatBuffers struct");
static_assert(sizeof(MTS::Schema::ObservationData) == (kObservationDim * sizeof(float)),
              "ObservationData schema drift: field count no longer matches kObservationDim "
              "(MakeObservationData/ToObservationArray's memcpy requires an exact size match)");
static_assert(sizeof(MTS::Schema::AsymmetryContext) == (kAsymmetryDim * sizeof(float)),
              "AsymmetryContext schema drift: field count no longer matches kAsymmetryDim "
              "(MakeAsymmetryContext/ToAsymmetryArray's memcpy requires an exact size match)");

}  // namespace Contract
}  // namespace Schema
}  // namespace MTS
