#include "MindfulTrader_Precompiled.h"

/*=============================================================================
 * CrossMarketStudies.cpp
 * 
 * ACSIL studies for cross-market correlation detection (ZN, DX)
 * Deploy on hidden 60-min charts to detect divergences with ES
 * 
 * Studies:
 *   - scsf_CrossMarket_ZN: 10Y Treasury (bonds) correlation & trend
 *   - scsf_CrossMarket_DX: Dollar Index correlation & trend
 * 
 * Updates IndicatorManager automatically:
 *   - CORR_ES_ZN / CORR_ES_DX (rolling 20-bar correlation)
 *   - ZN_TREND / DX_TREND (trend direction: -1/0/1)
 * 
 * Exports to TransformerData.jsonl via existing payload system (no ZMQ needed)
 *============================================================================*/

namespace CrossMarketHelpers {

// Calculate Pearson correlation coefficient between two arrays
// Returns value between -1.0 (perfect inverse) and +1.0 (perfect alignment)
double CalculateCorrelation(SCFloatArrayRef arrayA, SCFloatArrayRef arrayB, int startIndex, int length) {
    if (length < 2) return 0.0;
    
    // Calculate means
    double sumA = 0.0, sumB = 0.0;
    for (int i = 0; i < length; i++) {
        int idx = startIndex - i;
        if (idx < 0) return 0.0;  // Insufficient data
        sumA += arrayA[idx];
        sumB += arrayB[idx];
    }
    double meanA = sumA / length;
    double meanB = sumB / length;
    
    // Calculate correlation components
    double numerator = 0.0;
    double denomA = 0.0;
    double denomB = 0.0;
    
    for (int i = 0; i < length; i++) {
        int idx = startIndex - i;
        double devA = arrayA[idx] - meanA;
        double devB = arrayB[idx] - meanB;
        numerator += devA * devB;
        denomA += devA * devA;
        denomB += devB * devB;
    }
    
    // Avoid division by zero
    if (denomA < 0.0001 || denomB < 0.0001) return 0.0;
    
    return numerator / sqrt(denomA * denomB);
}

// Detect trend direction using simple EMA crossover (fast vs slow)
// Returns: -1 (downtrend), 0 (flat), +1 (uptrend)
int GetTrendDirection(SCStudyInterfaceRef sc, SCFloatArrayRef emaFast, SCFloatArrayRef emaSlow) {
    int idx = sc.Index;
    if (idx < 1) return 0;
    
    float fastNow = emaFast[idx];
    float slowNow = emaSlow[idx];
    float fastPrev = emaFast[idx - 1];
    
    // Strong uptrend: fast above slow and rising
    if (fastNow > slowNow && fastNow > fastPrev) return 1;
    
    // Strong downtrend: fast below slow and falling
    if (fastNow < slowNow && fastNow < fastPrev) return -1;
    
    // Otherwise flat/unclear
    return 0;
}

} // namespace CrossMarketHelpers

/*=============================================================================
 * scsf_CrossMarket_ZN
 * 
 * Deploy on: ZN (10-Year Treasury) 60-min chart (hidden)
 * Purpose: Detect bond market divergence from ES
 * 
 * Logic:
 *   - Strong negative ES-ZN correlation = risk-off (bonds up, stocks down)
 *   - Positive ES-ZN correlation = unusual, potential reversal warning
 *   - Bond downtrend + ES uptrend = overconfidence (bearish divergence)
 *============================================================================*/
SCSFExport scsf_CrossMarket_ZN(SCStudyInterfaceRef sc)
{
    // Subgraph indices
    enum { SG_EMA_FAST = 0, SG_EMA_SLOW = 1 };
    
    // Input indices
    enum { 
        IN_ES_STUDY_ID = 0,        // Link to ES chart
        IN_CORR_LENGTH = 1,        // Correlation window (default 20)
        IN_EMA_FAST = 2,           // Fast EMA period (default 10)
        IN_EMA_SLOW = 3            // Slow EMA period (default 30)
    };
    
    SCSubgraphRef EmaFast = sc.Subgraph[SG_EMA_FAST];
    SCSubgraphRef EmaSlow = sc.Subgraph[SG_EMA_SLOW];
    
    SCInputRef Input_ESStudyID = sc.Input[IN_ES_STUDY_ID];
    SCInputRef Input_CorrLength = sc.Input[IN_CORR_LENGTH];
    SCInputRef Input_EmaFast = sc.Input[IN_EMA_FAST];
    SCInputRef Input_EmaSlow = sc.Input[IN_EMA_SLOW];
    
    if (sc.SetDefaults)
    {
        sc.GraphName = "CrossMarket: ZN (10Y Treasury)";
        sc.StudyDescription = "Detects ES-ZN correlation and bond trend (updates IndicatorManager)";
        sc.AutoLoop = 1;
        sc.UpdateAlways = 1;  // Recalculate on every tick for real-time correlation
        sc.GraphRegion = 0;
        sc.FreeDLL = 0;
        
        EmaFast.Name = "EMA Fast";
        EmaFast.DrawStyle = DRAWSTYLE_LINE;
        EmaFast.PrimaryColor = RGB(0, 255, 0);
        
        EmaSlow.Name = "EMA Slow";
        EmaSlow.DrawStyle = DRAWSTYLE_LINE;
        EmaSlow.PrimaryColor = RGB(255, 0, 0);
        
        Input_ESStudyID.Name = "ES Study ID (for correlation)";
        Input_ESStudyID.SetStudyID(0);
        
        Input_CorrLength.Name = "Correlation Window (bars)";
        Input_CorrLength.SetInt(20);
        Input_CorrLength.SetIntLimits(5, 100);
        
        Input_EmaFast.Name = "Fast EMA Period";
        Input_EmaFast.SetInt(10);
        
        Input_EmaSlow.Name = "Slow EMA Period";
        Input_EmaSlow.SetInt(30);
        
        return;
    }
    
    // Persistent storage keys for delta/accel state (survives study reload)
    const int KEY_ZN_PREV_CORR = 1;
    const int KEY_ZN_PREV_DELTA = 2;
    const int KEY_ZN_INITIALIZED = 3;
    
    // Calculate EMAs for trend detection
    sc.ExponentialMovAvg(sc.Close, EmaFast, Input_EmaFast.GetInt());
    sc.ExponentialMovAvg(sc.Close, EmaSlow, Input_EmaSlow.GetInt());
    
    // Get ES close prices for correlation
    SCFloatArray esCloseArray;
    int esStudyID = Input_ESStudyID.GetStudyID();
    
    if (esStudyID == 0) {
        // No ES study linked yet - skip correlation
        return;
    }
    
    // Get ES close prices (subgraph 0 = close)
    if (!sc.GetStudyArrayUsingID(esStudyID, 0, esCloseArray)) {
        return;  // ES study not available
    }
    
    // Calculate correlation (rolling 20-bar window)
    int corrLength = Input_CorrLength.GetInt();
    if (sc.Index < corrLength) return;  // Need sufficient history
    
    double correlation = CrossMarketHelpers::CalculateCorrelation(
        sc.Close, esCloseArray, sc.Index, corrLength);
    
    // Calculate correlation derivatives using persistent storage (survives study reload)
    double prevCorrelation = sc.GetPersistentDouble(KEY_ZN_PREV_CORR);
    double prevDelta = sc.GetPersistentDouble(KEY_ZN_PREV_DELTA);
    int initialized = sc.GetPersistentInt(KEY_ZN_INITIALIZED);
    
    // On first bar, initialize trackers to avoid spurious delta/accel
    if (initialized == 0) {
        prevCorrelation = correlation;
        prevDelta = 0.0;
        sc.SetPersistentInt(KEY_ZN_INITIALIZED, 1);
    }
    
    double correlationDelta = correlation - prevCorrelation;
    double correlationAccel = correlationDelta - prevDelta;
    
    // Detect ZN trend direction
    int znTrend = CrossMarketHelpers::GetTrendDirection(sc, EmaFast, EmaSlow);
    
    // Update IndicatorManager (automatic ZMQ export + TransformerData.jsonl)
    auto* corrIndicator = IndicatorManager::Instance().GetIndicator<CorrelationIndicator>(
        IndicatorKey::CORR_ES_ZN);
    if (corrIndicator) {
        corrIndicator->Update(static_cast<float>(correlation));
    }
    
    auto* deltaIndicator = IndicatorManager::Instance().GetIndicator<CorrelationIndicator>(
        IndicatorKey::CORR_ES_ZN_DELTA);
    if (deltaIndicator) {
        deltaIndicator->Update(static_cast<float>(correlationDelta));
    }
    
    auto* accelIndicator = IndicatorManager::Instance().GetIndicator<CorrelationIndicator>(
        IndicatorKey::CORR_ES_ZN_ACCEL);
    if (accelIndicator) {
        accelIndicator->Update(static_cast<float>(correlationAccel));
    }
    
    auto* trendIndicator = IndicatorManager::Instance().GetIndicator<CrossMarketTrend>(
        IndicatorKey::ZN_TREND);
    if (trendIndicator) {
        trendIndicator->Update(static_cast<CrossMarketTrendEnum>(znTrend));
    }
    
    // Update persistent trackers for next bar (survives study reload)
    sc.SetPersistentDouble(KEY_ZN_PREV_CORR, correlation);
    sc.SetPersistentDouble(KEY_ZN_PREV_DELTA, correlationDelta);
    
    // Optional: Log significant correlation changes
    static float lastLoggedCorr = 0.0f;
    if (std::abs(correlation - lastLoggedCorr) > 0.15f) {
        lastLoggedCorr = static_cast<float>(correlation);
        
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2)
            << "CrossMarket ZN: Correlation changed to " << correlation
            << " (delta: " << correlationDelta << ", accel: " << correlationAccel << ")"
            << " | ZN Trend: " << (znTrend == 1 ? "UP" : znTrend == -1 ? "DOWN" : "FLAT")
            << " | ZN Price: " << sc.Close[sc.Index];
        Logger::getInstance().log(oss.str());
    }
}

/*=============================================================================
 * scsf_CrossMarket_DX
 * 
 * Deploy on: DX (Dollar Index) 60-min chart (hidden)
 * Purpose: Detect currency strength impact on ES
 * 
 * Logic:
 *   - Negative ES-DX correlation = normal (strong dollar = weak stocks)
 *   - Positive ES-DX correlation = risk-on (everything up together)
 *   - Dollar breakout + ES weakness = defensive positioning (bearish)
 *============================================================================*/
SCSFExport scsf_CrossMarket_DX(SCStudyInterfaceRef sc)
{
    // Subgraph indices
    enum { SG_EMA_FAST = 0, SG_EMA_SLOW = 1 };
    
    // Input indices
    enum { 
        IN_ES_STUDY_ID = 0,
        IN_CORR_LENGTH = 1,
        IN_EMA_FAST = 2,
        IN_EMA_SLOW = 3
    };
    
    SCSubgraphRef EmaFast = sc.Subgraph[SG_EMA_FAST];
    SCSubgraphRef EmaSlow = sc.Subgraph[SG_EMA_SLOW];
    
    SCInputRef Input_ESStudyID = sc.Input[IN_ES_STUDY_ID];
    SCInputRef Input_CorrLength = sc.Input[IN_CORR_LENGTH];
    SCInputRef Input_EmaFast = sc.Input[IN_EMA_FAST];
    SCInputRef Input_EmaSlow = sc.Input[IN_EMA_SLOW];
    
    if (sc.SetDefaults)
    {
        sc.GraphName = "CrossMarket: DX (Dollar Index)";
        sc.StudyDescription = "Detects ES-DX correlation and dollar trend (updates IndicatorManager)";
        sc.AutoLoop = 1;
        sc.UpdateAlways = 1;  // Recalculate on every tick for real-time correlation
        sc.GraphRegion = 0;
        sc.FreeDLL = 0;
        
        EmaFast.Name = "EMA Fast";
        EmaFast.DrawStyle = DRAWSTYLE_LINE;
        EmaFast.PrimaryColor = RGB(0, 255, 0);
        
        EmaSlow.Name = "EMA Slow";
        EmaSlow.DrawStyle = DRAWSTYLE_LINE;
        EmaSlow.PrimaryColor = RGB(255, 0, 0);
        
        Input_ESStudyID.Name = "ES Study ID (for correlation)";
        Input_ESStudyID.SetStudyID(0);
        
        Input_CorrLength.Name = "Correlation Window (bars)";
        Input_CorrLength.SetInt(20);
        Input_CorrLength.SetIntLimits(5, 100);
        
        Input_EmaFast.Name = "Fast EMA Period";
        Input_EmaFast.SetInt(10);
        
        Input_EmaSlow.Name = "Slow EMA Period";
        Input_EmaSlow.SetInt(30);
        
        return;
    }
    
    // Persistent storage keys for delta/accel state (survives study reload)
    const int KEY_DX_PREV_CORR = 1;
    const int KEY_DX_PREV_DELTA = 2;
    const int KEY_DX_INITIALIZED = 3;
    
    // Calculate EMAs for trend detection
    sc.ExponentialMovAvg(sc.Close, EmaFast, Input_EmaFast.GetInt());
    sc.ExponentialMovAvg(sc.Close, EmaSlow, Input_EmaSlow.GetInt());
    
    // Get ES close prices for correlation
    SCFloatArray esCloseArray;
    int esStudyID = Input_ESStudyID.GetStudyID();
    
    if (esStudyID == 0) {
        return;  // No ES study linked
    }
    
    if (!sc.GetStudyArrayUsingID(esStudyID, 0, esCloseArray)) {
        return;  // ES study not available
    }
    
    // Calculate correlation
    int corrLength = Input_CorrLength.GetInt();
    if (sc.Index < corrLength) return;
    
    double correlation = CrossMarketHelpers::CalculateCorrelation(
        sc.Close, esCloseArray, sc.Index, corrLength);
    
    // Calculate correlation derivatives using persistent storage (survives study reload)
    double prevCorrelation = sc.GetPersistentDouble(KEY_DX_PREV_CORR);
    double prevDelta = sc.GetPersistentDouble(KEY_DX_PREV_DELTA);
    int initialized = sc.GetPersistentInt(KEY_DX_INITIALIZED);
    
    // On first bar, initialize trackers to avoid spurious delta/accel
    if (initialized == 0) {
        prevCorrelation = correlation;
        prevDelta = 0.0;
        sc.SetPersistentInt(KEY_DX_INITIALIZED, 1);
    }
    
    double correlationDelta = correlation - prevCorrelation;
    double correlationAccel = correlationDelta - prevDelta;
    
    // Detect DX trend direction
    int dxTrend = CrossMarketHelpers::GetTrendDirection(sc, EmaFast, EmaSlow);
    
    // Update IndicatorManager
    auto* corrIndicator = IndicatorManager::Instance().GetIndicator<CorrelationIndicator>(
        IndicatorKey::CORR_ES_DX);
    if (corrIndicator) {
        corrIndicator->Update(static_cast<float>(correlation));
    }
    
    auto* deltaIndicator = IndicatorManager::Instance().GetIndicator<CorrelationIndicator>(
        IndicatorKey::CORR_ES_DX_DELTA);
    if (deltaIndicator) {
        deltaIndicator->Update(static_cast<float>(correlationDelta));
    }
    
    auto* accelIndicator = IndicatorManager::Instance().GetIndicator<CorrelationIndicator>(
        IndicatorKey::CORR_ES_DX_ACCEL);
    if (accelIndicator) {
        accelIndicator->Update(static_cast<float>(correlationAccel));
    }
    
    auto* trendIndicator = IndicatorManager::Instance().GetIndicator<CrossMarketTrend>(
        IndicatorKey::DX_TREND);
    if (trendIndicator) {
        trendIndicator->Update(static_cast<CrossMarketTrendEnum>(dxTrend));
    }
    
    // Update persistent trackers for next bar (survives study reload)
    sc.SetPersistentDouble(KEY_DX_PREV_CORR, correlation);
    sc.SetPersistentDouble(KEY_DX_PREV_DELTA, correlationDelta);
    
    // Optional: Log significant correlation changes
    static float lastLoggedCorr = 0.0f;
    if (std::abs(correlation - lastLoggedCorr) > 0.15f) {
        lastLoggedCorr = static_cast<float>(correlation);
        
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2)
            << "CrossMarket DX: Correlation changed to " << correlation
            << " (delta: " << correlationDelta << ", accel: " << correlationAccel << ")"
            << " | DX Trend: " << (dxTrend == 1 ? "UP" : dxTrend == -1 ? "DOWN" : "FLAT")
            << " | DX Price: " << sc.Close[sc.Index];
        Logger::getInstance().log(oss.str());
    }
}
