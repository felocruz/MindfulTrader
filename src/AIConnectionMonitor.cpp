#include "../include/AIConnectionMonitor.h"
#include "../include/SystemOrchestrator.h"
#include <sstream>
#include <fstream>

// ═══════════════════════════════════════════════════════════════
// MODEL HEALTH STATUS INTEGRATION (Step 1.6)
// ═══════════════════════════════════════════════════════════════

AIConnectionMonitor::ModelHealthStatus AIConnectionMonitor::CheckModelHealthStatus(SCStudyInterfaceRef sc) {
    SCDateTime currentTime = sc.CurrentSystemDateTime;
    
    // 1. Check cache (only read file every 60 seconds to reduce I/O)
    double secondsSinceCheck = (currentTime - m_lastHealthFileCheck).GetAsDouble() * 86400.0;
    if (secondsSinceCheck < MODEL_HEALTH_CACHE_SEC && m_lastHealthFileCheck != SCDateTime(0)) {
        return m_lastModelHealth;
    }
    
    // 2. Read model_health_status.json file
    std::ifstream file(m_healthFilePath.c_str());
    if (!file.is_open()) {
        // File missing or inaccessible - default to HEALTHY (fail-safe)
        if (m_lastHealthFileCheck == SCDateTime(0)) {
            // First check - log warning but don't spam
            Logger::getInstance().log("Model health file missing - proceeding with caution (HEALTHY)");
        }
        m_lastHealthFileCheck = currentTime;
        m_lastModelHealth = ModelHealthStatus::HEALTHY;
        return ModelHealthStatus::HEALTHY;
    }
    
    // 3. Parse JSON manually (simple string parsing to avoid nlohmann::json dependency issues)
    std::string line;
    std::string statusStr;
    float alphaSlippage = 0.0f;
    int sampleSize = 0;
    std::string lastUpdated;
    
    while (std::getline(file, line)) {
        // Parse "status": "HEALTHY"
        size_t statusPos = line.find("\"status\"");
        if (statusPos != std::string::npos) {
            size_t colonPos = line.find(":", statusPos);
            size_t firstQuote = line.find("\"", colonPos + 1);
            size_t secondQuote = line.find("\"", firstQuote + 1);
            if (firstQuote != std::string::npos && secondQuote != std::string::npos) {
                statusStr = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
            }
        }
        
        // Parse "alpha_slippage_pct": 12.5
        size_t alphaPos = line.find("\"alpha_slippage_pct\"");
        if (alphaPos != std::string::npos) {
            size_t colonPos = line.find(":", alphaPos);
            if (colonPos != std::string::npos) {
                std::string numStr = line.substr(colonPos + 1);
                // Remove whitespace and comma
                size_t commaPos = numStr.find(",");
                if (commaPos != std::string::npos) {
                    numStr = numStr.substr(0, commaPos);
                }
                alphaSlippage = static_cast<float>(atof(numStr.c_str()));
            }
        }
        
        // Parse "sample_size": 450
        size_t samplePos = line.find("\"sample_size\"");
        if (samplePos != std::string::npos) {
            size_t colonPos = line.find(":", samplePos);
            if (colonPos != std::string::npos) {
                std::string numStr = line.substr(colonPos + 1);
                size_t commaPos = numStr.find(",");
                if (commaPos != std::string::npos) {
                    numStr = numStr.substr(0, commaPos);
                }
                sampleSize = atoi(numStr.c_str());
            }
        }
        
        // Parse "last_updated": "2025-12-20T14:30:00Z"
        size_t updatePos = line.find("\"last_updated\"");
        if (updatePos != std::string::npos) {
            size_t colonPos = line.find(":", updatePos);
            size_t firstQuote = line.find("\"", colonPos + 1);
            size_t secondQuote = line.find("\"", firstQuote + 1);
            if (firstQuote != std::string::npos && secondQuote != std::string::npos) {
                lastUpdated = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
            }
        }
    }
    file.close();
    
    // 4. Validate required fields were parsed
    if (statusStr.empty()) {
        Logger::getInstance().log("Model health file corrupt (missing status) - defaulting to HEALTHY");
        m_lastHealthFileCheck = currentTime;
        m_lastModelHealth = ModelHealthStatus::HEALTHY;
        return ModelHealthStatus::HEALTHY;
    }
    
    // 5. Validate freshness (<24 hours old)
    if (!lastUpdated.empty()) {
        SCDateTime updateTime = ParseISO8601(lastUpdated);
        double secondsSinceUpdate = (currentTime - updateTime).GetAsDouble() * 86400.0;
        
        if (secondsSinceUpdate > MODEL_HEALTH_STALE_SEC) {
            // File is stale (>24h old) - force WARNING state
            SCString logMsg;
            logMsg.Format("⚠️ Model health file stale (%.1fh old) - forcing WARNING state", 
                         secondsSinceUpdate / 3600.0);
            Logger::getInstance().log(logMsg.GetChars());
            
            m_lastHealthFileCheck = currentTime;
            m_lastModelHealth = ModelHealthStatus::WARNING;
            return ModelHealthStatus::WARNING;
        }
    }
    
    // 6. Convert string status to enum
    ModelHealthStatus newHealth = ModelHealthStatus::HEALTHY;
    if (statusStr == "WARNING") {
        newHealth = ModelHealthStatus::WARNING;
    } else if (statusStr == "SOFT_LOCKED") {
        newHealth = ModelHealthStatus::SOFT_LOCKED;
    }
    
    // 7. Log status changes (state transitions only)
    if (newHealth != m_lastModelHealth) {
        SCString logMsg;
        if (newHealth == ModelHealthStatus::SOFT_LOCKED) {
            logMsg.Format("Model Health: SOFT_LOCKED (Alpha: %.1f%%, n=%d) - ALL AI signals disabled", 
                         alphaSlippage, sampleSize);
            Logger::getInstance().log(logMsg.GetChars());
        } else if (newHealth == ModelHealthStatus::WARNING) {
            logMsg.Format("Model Health: WARNING (Alpha: %.1f%%, n=%d) - HIGH confidence only (>=0.70)", 
                         alphaSlippage, sampleSize);
            Logger::getInstance().log(logMsg.GetChars());
        } else {
            logMsg.Format("Model Health: HEALTHY (Alpha: %.1f%%, n=%d) - Normal operation resumed", 
                         alphaSlippage, sampleSize);
            Logger::getInstance().log(logMsg.GetChars());
        }
    }
    
    // 8. Update cache
    m_lastModelHealth = newHealth;
    m_lastHealthFileCheck = currentTime;
    
    return newHealth;
}

// ═══════════════════════════════════════════════════════════════
// SIGNAL ACCEPTANCE WITH MODEL HEALTH
// ═══════════════════════════════════════════════════════════════

bool AIConnectionMonitor::ShouldAcceptSignalWithModelHealth(
    float confidence, 
    ModelHealthStatus health
) const {
    // ELITE DECISION TREE: Multi-dimensional signal acceptance gating
    // Considers: System State + Mental Profile + Health Status + Inference Latency + Queue Depth + Error Rate
    
    // ═══════════════════════════════════════════════════════════════
    // LAYER 0: SystemOrchestrator State Validation (Institutional Gate)
    // ═══════════════════════════════════════════════════════════════
    
    SystemOrchestrator& orchestrator = SystemOrchestrator::Instance();
    if (orchestrator.GetCurrentState() != SystemState::ACTIVE_TRADING) {
        return false;
    }
    
    if (!orchestrator.IsSystemHealthy()) {
        return false;
    }
    
    // ═══════════════════════════════════════════════════════════════
    // LAYER 0.5: Mental Profile Gate (Psychological Readiness)
    // ═══════════════════════════════════════════════════════════════
    
    MentalProfile mentalProfile = orchestrator.GetMentalProfile();
    
    if (mentalProfile == MentalProfile::ASSESSMENT_REQUIRED) {
        return false;
    }
    
    if (mentalProfile == MentalProfile::STAY_OUT) {
        return false;
    }
    
    if (mentalProfile == MentalProfile::CAUTION && confidence < 0.80f) {
        return false;
    }
    
    // ═══════════════════════════════════════════════════════════════
    // LAYER 1: Health Status Baseline Thresholds
    // ═══════════════════════════════════════════════════════════════
    
    float baseThreshold = 0.55f;
    std::string rejectionReason = "";
    
    switch (health) {
        case ModelHealthStatus::HEALTHY:
            baseThreshold = 0.55f;  // Normal operation
            break;
        
        case ModelHealthStatus::WARNING:
            baseThreshold = 0.70f;  // Strict mode - high confidence only
            break;
        
        case ModelHealthStatus::SOFT_LOCKED:
            // Reject ALL signals - model is locked out
            return false;
    }
    
    // ═══════════════════════════════════════════════════════════════
    // LAYER 2: Adaptive Threshold Based on System Health
    // ═══════════════════════════════════════════════════════════════
    
    float avgInference = GetAvgInferenceMs();
    int queueDepth = GetQueueDepth();
    int errorCount = GetErrorCount();
    
    // Boost threshold if inference is slow (signal might be stale by execution)
    if (avgInference > 50.0f) {  // >50ms = elevated latency
        baseThreshold += 0.05f;   // +0.05 boost (5% stricter)
        rejectionReason = "Elevated inference latency";
    }
    
    // Boost threshold if queue is backing up (system under stress)
    if (queueDepth > 5) {  // Queue depth threshold
        baseThreshold += 0.03f;   // +0.03 boost (3% stricter)
        rejectionReason = "Queue depth elevated";
    }
    
    // Boost threshold if error rate is high
    if (errorCount > 10) {  // >10 errors indicates trouble
        baseThreshold += 0.10f;   // +0.10 boost (10% stricter)
        rejectionReason = "High error count";
    }
    
    // Cap threshold at 0.95 (don't become impossible to beat)
    baseThreshold = std::min(baseThreshold, 0.95f);
    
    // ═══════════════════════════════════════════════════════════════
    // LAYER 3: Confidence Check (with Mental Profile Context)
    // ═══════════════════════════════════════════════════════════════
    
    if (mentalProfile == MentalProfile::CAUTION && confidence < 0.80f) {
        return false;
    }
    
    bool accepted = (confidence >= baseThreshold);
    
    // Log rejections (not acceptances - reduce spam)
    if (!accepted && !rejectionReason.empty()) {
        // Optional: Log rejection details for debugging
        // Only log the first rejection of each type per analysis period
    }
    
    return accepted;
}

// ═══════════════════════════════════════════════════════════════
// ELITE METRICS TRACKING (from Heartbeat FlatBuffer)
// ═══════════════════════════════════════════════════════════════

float AIConnectionMonitor::GetAvgInferenceMs() const {
    // Return average inference latency (milliseconds, 60s moving average)
    // Updated from Heartbeat.avg_inference_ms
    return m_avgInferenceMs;
}

int AIConnectionMonitor::GetQueueDepth() const {
    // Return current model queue depth
    // Updated from Heartbeat.queue_depth
    return m_queueDepth;
}

int AIConnectionMonitor::GetErrorCount() const {
    // Return recent error count
    // Updated from Heartbeat.error_count
    return m_errorCount;
}

bool AIConnectionMonitor::IsTransportDegraded() const {
    return m_transportDegraded;
}

float AIConnectionMonitor::GetTransportLagMs() const {
    return m_transportLagMs;
}

void AIConnectionMonitor::UpdateTransportHealth(bool degraded, float transport_lag_ms) {
    m_transportDegraded = degraded;
    m_transportLagMs = transport_lag_ms;
}

void AIConnectionMonitor::UpdateEliteMetrics(float last_inference_ms, float avg_inference_ms,
                                             int queue_depth, int error_count) {
    // Update elite model metrics from Heartbeat
    // Called by AIHeartbeatMonitor when FlatBuffer Heartbeat received
    m_lastInferenceMs = last_inference_ms;
    m_avgInferenceMs = avg_inference_ms;
    m_queueDepth = queue_depth;
    m_errorCount = error_count;
    
    // Update model health based on new metrics
    if (avg_inference_ms > 100.0f || queue_depth > 10 || error_count > 20) {
        // High latency, queue buildup, or high error rate -> WARNING
        m_modelHealth = ModelHealthStatus::WARNING;
    } else {
        // Nominal metrics -> HEALTHY
        m_modelHealth = ModelHealthStatus::HEALTHY;
    }
    
    // Propagate inference RTT to SystemOrchestrator for HUD and cross-socket guards
    SystemOrchestrator::Instance().UpdateInferenceRTT(static_cast<int64_t>(avg_inference_ms));
}
