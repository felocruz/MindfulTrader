#include "MindfulTrader_Precompiled.h"

Trade::Trade()
    : m_parent_order_id( 0 )
    , m_stop_internal_order_id( 0 )
    , m_target_internal_order_id( 0 )
    , m_symbol( "" )
    , m_firestore_doc_id( "" )
    , m_status(TradeStatusEnum::NO_TRADE)
    , m_side(TradeSideEnum::FLAT)
    , m_size( 0.0 )
    , m_entry_price( 0.0 )
    , m_entry_index( -1 )
    , m_exit_index( -1 )
    , m_exit_price( 0.0 )
    , m_stop( 0.0 )
    , m_target( 0.0 )
    , m_entry_date()
    , m_exit_date()
    , m_unrealized_pnl( 0.0 )
    , m_realized_pnl( 0.0 )
    , m_channel( 0 )
    , m_entry_grade( 0 )
    , m_exit_grade( 0 )
    , m_trade_grade( 0 )
    , m_entry_high( 0.0 )
    , m_entry_low( 0.0 )
    , m_exit_high( 0.0 )
    , m_exit_low( 0.0 )
    , m_mae_ticks( 0.0 )
    , m_mfe_ticks( 0.0 )
    , m_confidence( 0.0f )
    , m_highest_price( 0.0 )
    , m_lowest_price( 0.0 )
{}

void Trade::Reset(SCStudyInterfaceRef sc) {
    // Reset all member variables to their default state.
    m_parent_order_id = 0;
    m_stop_internal_order_id = 0;
    m_target_internal_order_id = 0;
    m_symbol = sc.GetRealTimeSymbol();
    m_firestore_doc_id = "";
    m_status = TradeStatusEnum::NO_TRADE;
    m_side = TradeSideEnum::FLAT;
    m_size = 0.0;
    m_entry_price = 0.0;
    m_entry_index = -1;
    m_exit_index = -1;
    m_exit_price = 0.0;
    m_stop = 0.0;
    m_target = 0.0;
    m_entry_date = "";
    m_exit_date = "";
    m_exitReasonTag = "";
    m_realized_pnl = 0.0;
    m_unrealized_pnl = 0.0;
    m_entry_grade = 0;
    m_exit_grade = 0;
    m_trade_grade = 0;
    m_entry_high = 0.0;
    m_entry_low = 0.0;
    m_exit_high = 0.0;
    m_exit_low = 0.0;
    m_mae_ticks = 0.0;
    m_mfe_ticks = 0.0;
    m_confidence = 0.0f;
    m_highest_price = 0.0;
    m_lowest_price = 0.0;
}

void Trade::Open(SCStudyInterfaceRef sc, int parent_order_id, double size, double entry_price, double entry_time, const s_SCTradeOrder& orderDetails) {
    m_parent_order_id = parent_order_id;
    m_symbol = orderDetails.Symbol.GetChars();
    // Defensive: ensure size is non-zero. If zero, treat as FLAT and log a warning.
    m_size = fabs(size);
    m_entry_timestamp = entry_time;
    if (m_size == 0.0) {
        Logger::getInstance().log("Trade::Open called with zero size — treating as FLAT.");
        m_side = TradeSideEnum::FLAT;
    } else {
        m_side = (size > 0) ? TradeSideEnum::LONG : TradeSideEnum::SHORT;
    }
    m_entry_price = entry_price;
    m_status = TradeStatusEnum::OPEN;
    m_entry_date = sc.DateTimeToString(sc.BaseDateTimeIn[sc.Index], FLAG_DT_COMPLETE_DATETIME);
    m_entry_index = sc.Index;
    m_entry_high = sc.High[sc.Index];
    m_entry_low = sc.Low[sc.Index];
    
    // Initialize MAE/MFE tracking
    m_highest_price = entry_price;
    m_lowest_price = entry_price;
    m_mae_ticks = 0.0;
    m_mfe_ticks = 0.0;
    
    UpdateGrades(sc); // Calculate initial grades immediately
}

void Trade::ScaleIn(double new_average_price, double new_size) {
    // Defensive: only apply scale-in if new_size is positive.
    if (new_size <= 0.0) {
        Logger::getInstance().log("Trade::ScaleIn called with non-positive size; ignoring.");
        return;
    }
    m_entry_price = new_average_price;
    m_size = new_size;
}

void Trade::Close(SCStudyInterfaceRef sc, double exit_price, const s_SCOrderFillData& /*latestFill*/) {
    m_exit_price = exit_price;
    m_status = TradeStatusEnum::CLOSE;
    m_exit_index = sc.Index;  // Capture exit bar index for bars_held calculation
    m_exit_date = sc.DateTimeToString(sc.BaseDateTimeIn[sc.Index], FLAG_DT_COMPLETE_DATETIME);
    m_exit_high = sc.High[sc.Index];
    m_exit_low = sc.Low[sc.Index];

    // Final price-extreme check with actual fill price
    if (exit_price > m_highest_price) m_highest_price = exit_price;
    if (exit_price < m_lowest_price)  m_lowest_price  = exit_price;

    // Compute MAE/MFE ticks once (deferred from per-tick Update path)
    const double tick_size = sc.TickSize;
    if (tick_size > 0.0) {
        const double inv_tick = 1.0 / tick_size;
        if (m_side == TradeSideEnum::LONG) {
            m_mfe_ticks = (m_highest_price - m_entry_price) * inv_tick;
            m_mae_ticks = (m_entry_price - m_lowest_price) * inv_tick;
        } else if (m_side == TradeSideEnum::SHORT) {
            m_mfe_ticks = (m_entry_price - m_lowest_price) * inv_tick;
            m_mae_ticks = (m_highest_price - m_entry_price) * inv_tick;
        }
    }

    CalculatePnL(m_exit_price);
    UpdateGrades(sc);
}

void Trade::Update(SCStudyInterfaceRef sc) {
    if (m_status != TradeStatusEnum::OPEN) return;
    
    const double current_price = sc.Close[sc.Index];
    
    // Track price extremes (MAE/MFE ticks computed once in Close(), not per tick)
    if (current_price > m_highest_price) m_highest_price = current_price;
    if (current_price < m_lowest_price)  m_lowest_price  = current_price;
    
    CalculatePnL(current_price);
    UpdateGrades(sc);
}

void Trade::CalculatePnL(double current_price) {
    if (m_status == TradeStatusEnum::OPEN) {
        if (m_size <= 0.0) {
            m_unrealized_pnl = 0.0;
        } else if (m_side == TradeSideEnum::LONG) {
            m_unrealized_pnl = (current_price - m_entry_price) * m_size;
        } else if (m_side == TradeSideEnum::SHORT) {
            m_unrealized_pnl = (m_entry_price - current_price) * m_size;
        } else {
            m_unrealized_pnl = 0.0;
        }
    } else if (m_status == TradeStatusEnum::CLOSE) {
        if (m_size <= 0.0) {
            m_realized_pnl = 0.0;
        } else if (m_side == TradeSideEnum::LONG) {
            m_realized_pnl = (m_exit_price - m_entry_price) * m_size;
        } else if (m_side == TradeSideEnum::SHORT) {
            m_realized_pnl = (m_entry_price - m_exit_price) * m_size;
        } else {
            m_realized_pnl = 0.0;
        }

        m_unrealized_pnl = 0.0;
    }
}

void Trade::UpdateGrades(SCStudyInterfaceRef sc) {
    // This logic is complex and tightly coupled to the sc object, so it remains within Trade.
    // First, update the entry bar high/low if the bar is still forming.
    if (sc.Index == m_entry_index && sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_NOT_CLOSED) {
        m_entry_high = static_cast<double>(sc.High[sc.Index]);
        m_entry_low = static_cast<double>(sc.Low[sc.Index]);

        if (m_side == TradeSideEnum::LONG) {
            m_entry_grade = std::max(0, static_cast<int>(CalculateGradeValue(m_entry_price - m_entry_low, m_entry_high - m_entry_low)));
        }
        else if (m_side == TradeSideEnum::SHORT) {
            m_entry_grade = std::max(0, static_cast<int>(CalculateGradeValue(m_entry_high - m_entry_price, m_entry_high - m_entry_low)));
        }
    }

    double current_exit_price = (m_status == TradeStatusEnum::OPEN) ? sc.Close[sc.Index] : m_exit_price;
    m_exit_high = sc.High[sc.Index];
    m_exit_low = sc.Low[sc.Index];

    if (m_side == TradeSideEnum::LONG) {
        m_exit_grade = std::max(0, static_cast<int>(CalculateGradeValue(current_exit_price - m_exit_low, m_exit_high - m_exit_low)));
        m_trade_grade = std::max(0, static_cast<int>(CalculateGradeValue(current_exit_price - m_entry_price, m_channel)));
    } else if (m_side == TradeSideEnum::SHORT) {
        m_exit_grade = std::max(0, static_cast<int>(CalculateGradeValue(m_exit_high - current_exit_price, m_exit_high - m_exit_low)));
        m_trade_grade = std::max(0, static_cast<int>(CalculateGradeValue(m_entry_price - current_exit_price, m_channel)));
    }
}

// Legacy JSON serialization removed - Elite v2.4 uses FlatBuffer binary format exclusively

std::vector<uint8_t> Trade::CreateTradeRequestFlatBuffer() const {
    flatbuffers::FlatBufferBuilder builder(512);
    
    // Build strings (these return offsets in the FlatBuffer arena)
    auto pattern_offset = builder.CreateString(m_pattern_name);
    
    // Determine request type based on side
    MTS::Schema::TradeRequestType request_type = MTS::Schema::TradeRequestType_ENTER_LONG;
    TradeActionEnum action = TradeActionEnum::ENTER_LONG;
    if (m_side == TradeSideEnum::SHORT) {
        request_type = MTS::Schema::TradeRequestType_ENTER_SHORT;
        action = TradeActionEnum::ENTER_SHORT;
    }
    
    // Use the CreateTradeRequest helper function (generated by flatc)
    // NOTE: Entry price, quantity, stop loss, and target are determined by C++ internal logic
    // not sent to Python. Python sends only the trade signal (confidence + pattern).
    auto trade_request = MTS::Schema::CreateTradeRequest(
        builder,
        m_parent_order_id,           // order_id
        request_type,                // request_type (ENTER_LONG or ENTER_SHORT)
        MTS::Schema::ActionDomain_TRADE_ACTION,
        static_cast<int8_t>(action),
        m_confidence,                // model_confidence (0.0-1.0)
        pattern_offset,              // pattern (offset to string)
        0,                          // timestamp_us (TODO: add proper timestamp)
        0,                          // metadata (optional: 0 means no metadata)
        true                        // allow_new_entries (default: true)
    );
    
    builder.Finish(trade_request);
    
    // Return serialized bytes
    uint8_t* buf = builder.GetBufferPointer();
    size_t size = builder.GetSize();
    return std::vector<uint8_t>(buf, buf + size);
}

std::vector<uint8_t> Trade::CreateTradeCloseFlatBuffer() const {
    flatbuffers::FlatBufferBuilder builder(512);
    
    // Build TradeClose using helper function
    auto firestore_id_offset = builder.CreateString(m_firestore_doc_id);
    
    // Calculate bars_held from entry/exit indices (accurate)
    uint32_t bars_held = (m_exit_index >= 0 && m_entry_index >= 0) 
        ? (m_exit_index - m_entry_index) 
        : 0;
    
    auto trade_close = MTS::Schema::CreateTradeClose(
        builder,
        firestore_id_offset,         // firestore_doc_id
        m_parent_order_id,           // order_id
        static_cast<float>(m_exit_price),   // exit_price
        static_cast<uint32_t>(m_size),      // quantity
        m_realized_pnl,              // pnl
        bars_held,                   // bars_held (calculated from indices)
        static_cast<float>(m_mae_ticks),    // mae_ticks
        static_cast<float>(m_mfe_ticks),    // mfe_ticks
        0,                           // timestamp_us (TODO: add timestamp)
        // Elder Trade Grade — post-trade attribution telemetry
        static_cast<int16_t>(m_trade_grade),    // trade_grade (% channel capture)
        static_cast<int16_t>(m_entry_grade),    // entry_grade (bar positioning)
        static_cast<int16_t>(m_exit_grade),     // exit_grade (bar positioning)
        static_cast<float>(m_channel)           // keltner_channel_width
    );
    
    builder.Finish(trade_close);
    
    // Return serialized bytes
    uint8_t* buf = builder.GetBufferPointer();
    size_t size = builder.GetSize();
    return std::vector<uint8_t>(buf, buf + size);
}


// Helper functions

double Trade::CalculateGradeValue(double numerator, double denominator) const {
    constexpr double kGradeMultiplier = 100.0;
    if (denominator == 0.0) return 0.0;
    return (numerator / denominator) * kGradeMultiplier;
}

void Trade::SetConfidence(float confidence) {
    m_confidence = confidence;
}


// --- Setters ---
void Trade::SetStop(double price) { m_stop = price; }
void Trade::SetTarget(double price) { m_target = price; }
void Trade::SetFirestoreDocId(const std::string& doc_id) { m_firestore_doc_id = doc_id; }
void Trade::SetAttachedOrderIds(int stop_id, int target_id) { m_stop_internal_order_id = stop_id; m_target_internal_order_id = target_id; }

//void Trade::SetStopInternalOrderID(int orderId) { m_stopInternalOrderID = orderId; }
//void Trade::SetTargetInternalOrderID(int orderId) { m_targetInternalOrderID = orderId; }
void Trade::SetChannel(double channel) { m_channel = channel; }

void Trade::SetPattern(const std::string& patternEnum, int patternId, const std::string& patternName) {
    m_pattern_enum = patternEnum;
    m_pattern_id = patternId;
    m_pattern_name = patternName;
}

// Move overload - avoids 2 string copies when called with temporaries
void Trade::SetPattern(std::string&& patternEnum, int patternId, std::string&& patternName) {
    m_pattern_enum = std::move(patternEnum);
    m_pattern_id = patternId;
    m_pattern_name = std::move(patternName);
}

void Trade::SetFirestoreDocId(std::string&& doc_id) {
    m_firestore_doc_id = std::move(doc_id);
}

// --- GETTERS ---
const std::string& Trade::GetSymbol() const { return m_symbol; }
double Trade::GetSize() const { return m_size; }
double Trade::GetEntryPrice() const { return m_entry_price; }
double Trade::GetExitPrice() const { return m_exit_price; }
const std::string& Trade::GetFirestoreDocId() const { return m_firestore_doc_id; }
//double Trade::GetRiskReward() const { return m_riskReward; }
double Trade::GetUnrealizedPnL() const { return m_unrealized_pnl; }
double Trade::GetRealizedPnL() const { return m_realized_pnl; }
int Trade::GetEntryGrade() const { return m_entry_grade; }
int Trade::GetExitGrade() const { return m_exit_grade; }
