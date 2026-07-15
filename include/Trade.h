#pragma once

#include <string>
#include "MindfulTraderConstants.h"
#include "sierrachart.h"
#include "Indicator.h"
#include "flatbuffers/flatbuffers.h"
#include "generated/mts_schema_generated.h"


enum class TradeStatusEnum : int {
    NO_TRADE = 0,
    OPEN = 1,
    CLOSE = 2
};

class Trade {
public:
    Trade();

    void Reset(SCStudyInterfaceRef sc);

    // State update methods called by PositionManager
    void Open(SCStudyInterfaceRef sc, int parent_order_id, double size, double entry_price, double entry_time, const s_SCTradeOrder& orderDetails);
    void ScaleIn(double new_average_price, double new_size);
    void Close(SCStudyInterfaceRef sc, double exit_price, const s_SCOrderFillData& latestFill);
    void Update(SCStudyInterfaceRef sc);

    // Setters for externally managed data
    void SetStop(double price); 
    void SetTarget(double price);
    void SetFirestoreDocId(const std::string& doc_id);
    void SetFirestoreDocId(std::string&& doc_id);  // Move overload for efficiency
    void SetAttachedOrderIds(int stop_id, int target_id);
    void SetChannel(double channel);
    void SetPattern(const std::string& patternEnum, int patternId, const std::string& patternName);
    void SetPattern(std::string&& patternEnum, int patternId, std::string&& patternName);  // Move overload
    void SetConfidence(float confidence);  // Store AI signal confidence at entry

    // Triple-Barrier: explicit exit-reason tag (e.g. "TIME_STOP"), set by the
    // deterministic close path so .btst reason is recorded directly rather than
    // price-inferred. Empty => infer from exit price vs stop/target.
    void SetExitReasonTag(const std::string& tag) { m_exitReasonTag = tag; }

    // GAP 27: Entry-time regime snapshot for mid-trade target tightening
    void SetEntryRegime(HMMStateEnum hmmState, MarketClimate climate);
    void SetOriginalTargets(float t1, float t2, float t3);
    HMMStateEnum GetEntryHMMState() const { return m_entryHMMState; }
    MarketClimate GetEntryClimate() const { return m_entryClimate; }
    float GetOriginalTarget1() const { return m_originalTarget1; }
    float GetOriginalTarget2() const { return m_originalTarget2; }
    float GetOriginalTarget3() const { return m_originalTarget3; }

    // Serialization (FlatBuffer - zero-copy)
    std::vector<uint8_t> CreateTradeRequestFlatBuffer() const;
    std::vector<uint8_t> CreateTradeCloseFlatBuffer() const;

    // --- Getters ---
    int GetParentOrderId() const { return m_parent_order_id; }
    TradeStatusEnum GetStatus() const { return m_status; }
    TradeSideEnum GetSide() const { return m_side; }
    double GetStop() const { return m_stop; }
    double GetTarget() const { return m_target; }
    int GetTradeGrade() const { return m_trade_grade; }
    const std::string& GetSymbol() const;
    int GetStopInternalOrderID() const { return m_stop_internal_order_id; }
    int GetTargetInternalOrderID() const { return m_target_internal_order_id; }
    double GetSize() const;
    double GetEntryPrice() const;
    double GetExitPrice() const;
    SCString GetEntryDate() const { return m_entry_date; }
    SCString GetExitDate() const { return m_exit_date; }
    const std::string& GetFirestoreDocId() const;
    double GetUnrealizedPnL() const;
    double GetRealizedPnL() const;
    int GetEntryGrade() const;
    int GetExitGrade() const;
    const std::string& GetPatternEnum() const { return m_pattern_enum; }
    int GetPatternId() const { return m_pattern_id; }
    const std::string& GetPatternName() const { return m_pattern_name; }
    double GetMAETicks() const { return m_mae_ticks; }
    double GetMFETicks() const { return m_mfe_ticks; }
    float GetConfidence() const { return m_confidence; }
    double GetEntryHigh() const { return m_entry_high; }
    double GetEntryLow() const { return m_entry_low; }
    double GetHighestPrice() const { return m_highest_price; }
    double GetLowestPrice() const { return m_lowest_price; }
    int GetEntryIndex() const { return m_entry_index; }
    int GetExitIndex() const { return m_exit_index; }
    int GetBarsHeld() const { return (m_exit_index >= 0 && m_entry_index >= 0) ? (m_exit_index - m_entry_index) : 0; }
    const std::string& GetExitReasonTag() const { return m_exitReasonTag; }


private:
    void UpdateGrades(SCStudyInterfaceRef sc);
    void CalculatePnL(double current_price);
    double CalculateGradeValue(double numerator, double denominator) const;

    // --- Member Variables ---
    int m_parent_order_id{ 0 };
    int m_stop_internal_order_id{ 0 };
    int m_target_internal_order_id{ 0 };
    std::string m_symbol{ "" };
    std::string m_firestore_doc_id{ "" };
    std::string m_exitReasonTag{ "" };  // Triple-Barrier deterministic exit reason (empty => price-inferred)
    TradeStatusEnum m_status{ TradeStatusEnum::NO_TRADE };
    TradeSideEnum m_side{ TradeSideEnum::FLAT };
    double m_size{ 0.0 };
    double m_entry_price{ 0.0 };
    int m_entry_index{ -1 };
    double m_entry_timestamp{ 0.0 }; // ELITE: Exact fill time for Tail Risk
    int m_exit_index{ -1 };  // Set when position closed (for bars_held calculation)
    double m_exit_price{ 0.0 };
    double m_stop{ 0.0 };
    double m_target{ 0.0 };
    SCString m_entry_date;
    SCString m_exit_date;
    double m_unrealized_pnl{ 0.0 };
    double m_realized_pnl{ 0.0 };
    double m_channel{ 0 };
    int m_entry_grade{ 0 };
    int m_exit_grade{ 0 };
    int m_trade_grade{ 0 };
    double m_entry_high{ 0.0 };
    double m_entry_low{ 0.0 };
    double m_exit_high{ 0.0 };
    double m_exit_low{ 0.0 };
    
    // Pattern context - set on entry, used for pattern-specific logic
    std::string m_pattern_enum{ "" };     // "RaschkeTacticalTrigger" or "RaschkeStrategySetup"
    int m_pattern_id{ -1 };                // Numeric enum value
    std::string m_pattern_name{ "" };      // Human-readable name
    
    // Performance attribution fields (for Python Performance Attribution Engine)
    double m_mae_ticks{ 0.0 };             // Maximum Adverse Excursion in ticks
    double m_mfe_ticks{ 0.0 };             // Maximum Favorable Excursion in ticks
    float m_confidence{ 0.0f };            // AI signal confidence (0.0-1.0)
    double m_highest_price{ 0.0 };         // Track for MFE calculation
    double m_lowest_price{ 0.0 };          // Track for MAE calculation

    // GAP 27: Entry-time regime snapshot + original target prices for mid-trade tightening
    HMMStateEnum m_entryHMMState{ HMM_NO_PRIOR };
    MarketClimate m_entryClimate{ MarketClimate::GAUSSIAN_STABLE };
    float m_originalTarget1{ 0.0f };
    float m_originalTarget2{ 0.0f };
    float m_originalTarget3{ 0.0f };
};
