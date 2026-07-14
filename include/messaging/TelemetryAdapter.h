#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "generated/mts_schema_generated.h"
#include "generated/mts_schema_contract_generated.h"

namespace MTS::Messaging {

/**
 * @brief Simple POD (Plain Old Data) for internal C++ use
 */
struct TelemetryData {
    std::string screen_1_trend;
    std::string screen_2_trend;
    std::string screen_3_entry;
    double current_pnl;
};

class TelemetryAdapter {
public:
    /**
     * @brief Transforms TelemetryData into a canonical Diagnostic envelope
     * @return A vector of bytes ready for TransportStream::Emit()
     */
    static std::vector<uint8_t> Pack(const TelemetryData& data) {
        flatbuffers::FlatBufferBuilder builder(MTS::Schema::Contract::kBuilderCapacityTelemetryDiagnostic);

        const auto sender = builder.CreateString("TelemetryAdapter");
        const auto message = builder.CreateString(
            std::string("screen_1_trend=") + data.screen_1_trend +
            "|screen_2_trend=" + data.screen_2_trend +
            "|screen_3_entry=" + data.screen_3_entry +
            "|current_pnl=" + std::to_string(data.current_pnl));

        auto diagnostic = MTS::Schema::CreateDiagnostic(
            builder,
            MTS::Schema::Contract::NowTimestampUs(),
            sender,
            message,
            0.0f,
            0.0f
        );

        auto envelope = MTS::Schema::Contract::BuildEnvelope(
            builder,
            MTS::Schema::Contract::kEnvelopeDiagnostic,
            diagnostic.Union()
        );

        builder.Finish(envelope);

        const uint8_t* buf = builder.GetBufferPointer();
        size_t size = builder.GetSize();
        return std::vector<uint8_t>(buf, buf + size);
    }
};

} // namespace MTS::Messaging
