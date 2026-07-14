#pragma once

#include "generated/mts_schema_generated.h"
#include "flatbuffers/flatbuffers.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace MTS {
namespace Schema {
namespace Contract {

inline constexpr std::size_t kObservationDim = 16;
inline constexpr std::size_t kAsymmetryDim = 8;

using ObservationArray = std::array<float, kObservationDim>;
using AsymmetryArray = std::array<float, kAsymmetryDim>;

inline constexpr std::size_t kObsLogVarianceRatio = 0;
inline constexpr std::size_t kObsBurstinessIndex = 1;
inline constexpr std::size_t kObsRelativeRange = 2;
inline constexpr std::size_t kObsCorrectionAction = 3;
inline constexpr std::size_t kObsVolConvexity = 4;
inline constexpr std::size_t kObsLempelZiv = 5;
inline constexpr std::size_t kObsHurstExponent = 6;
inline constexpr std::size_t kObsMicroAsymmetry = 7;
inline constexpr std::size_t kObsFisherInfo = 8;
inline constexpr std::size_t kObsTailIndex = 9;
inline constexpr std::size_t kObsSkewnessIdx = 10;
inline constexpr std::size_t kObsVpinToxicity = 11;
inline constexpr std::size_t kObsLiqFragility = 12;
inline constexpr std::size_t kObsRecurrenceRate = 13;
inline constexpr std::size_t kObsFractalDim = 14;
inline constexpr std::size_t kObsMeanRevZ = 15;

inline constexpr std::size_t kAsymShannonEntropy = 0;
inline constexpr std::size_t kAsymShannonEfficiency = 1;
inline constexpr std::size_t kAsymTalebKurtosis = 2;
inline constexpr std::size_t kAsymTalebSkewness = 3;
inline constexpr std::size_t kAsymTalebCliff = 4;
inline constexpr std::size_t kAsymParetoRot = 5;
inline constexpr std::size_t kAsymRaschkeBurst = 6;
inline constexpr std::size_t kAsymSessionQualityScore = 7;

inline constexpr std::array<const char*, kObservationDim> kObservationFieldNames = {
    "log_variance_ratio",
    "burstiness_index",
    "relative_range",
    "correction_action",
    "vol_convexity",
    "lempel_ziv",
    "hurst_exponent",
    "micro_asymmetry",
    "fisher_info",
    "tail_index",
    "skewness_idx",
    "vpin_toxicity",
    "liq_fragility",
    "recurrence_rate",
    "fractal_dim",
    "mean_rev_z",
};

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
    return MTS::Schema::ObservationData(
        values[kObsLogVarianceRatio],
        values[kObsBurstinessIndex],
        values[kObsRelativeRange],
        values[kObsCorrectionAction],
        values[kObsVolConvexity],
        values[kObsLempelZiv],
        values[kObsHurstExponent],
        values[kObsMicroAsymmetry],
        values[kObsFisherInfo],
        values[kObsTailIndex],
        values[kObsSkewnessIdx],
        values[kObsVpinToxicity],
        values[kObsLiqFragility],
        values[kObsRecurrenceRate],
        values[kObsFractalDim],
        values[kObsMeanRevZ]);
}

inline ObservationArray ToObservationArray(
    const MTS::Schema::ObservationData& observation) {
    return {
        observation.log_variance_ratio(),
        observation.burstiness_index(),
        observation.relative_range(),
        observation.correction_action(),
        observation.vol_convexity(),
        observation.lempel_ziv(),
        observation.hurst_exponent(),
        observation.micro_asymmetry(),
        observation.fisher_info(),
        observation.tail_index(),
        observation.skewness_idx(),
        observation.vpin_toxicity(),
        observation.liq_fragility(),
        observation.recurrence_rate(),
        observation.fractal_dim(),
        observation.mean_rev_z(),
    };
}

inline MTS::Schema::AsymmetryContext MakeAsymmetryContext(
    const AsymmetryArray& values) {
    return MTS::Schema::AsymmetryContext(
        values[kAsymShannonEntropy],
        values[kAsymShannonEfficiency],
        values[kAsymTalebKurtosis],
        values[kAsymTalebSkewness],
        values[kAsymTalebCliff],
        values[kAsymParetoRot],
        values[kAsymRaschkeBurst],
        values[kAsymSessionQualityScore]);
}

inline AsymmetryArray ToAsymmetryArray(
    const MTS::Schema::AsymmetryContext& asymmetry) {
    return {
        asymmetry.shannon_entropy(),
        asymmetry.shannon_efficiency(),
        asymmetry.taleb_kurtosis(),
        asymmetry.taleb_skewness(),
        asymmetry.taleb_cliff(),
        asymmetry.pareto_rot(),
        asymmetry.raschke_burst(),
        asymmetry.session_quality_score(),
    };
}

static_assert(std::is_standard_layout<MTS::Schema::ObservationData>::value,
              "ObservationData must remain a standard-layout FlatBuffers struct");
static_assert(std::is_standard_layout<MTS::Schema::AsymmetryContext>::value,
              "AsymmetryContext must remain a standard-layout FlatBuffers struct");
static_assert(sizeof(MTS::Schema::ObservationData) == (kObservationDim * sizeof(float)),
              "ObservationData schema drift: expected 16 float fields");
static_assert(sizeof(MTS::Schema::AsymmetryContext) == (kAsymmetryDim * sizeof(float)),
              "AsymmetryContext schema drift: expected 8 float fields");

}  // namespace Contract
}  // namespace Schema
}  // namespace MTS
