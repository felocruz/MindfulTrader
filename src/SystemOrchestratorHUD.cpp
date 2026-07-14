/*
 * SystemOrchestrator HUD - Elite Real-Time Monitoring
 * 
 * Displays Zero-Latency State Cache on chart for instant system health visibility.
 * Uses lock-free atomic reads for sub-microsecond latency.
 * 
 * Subgraphs:
 *   0: System State (0=UNINITIALIZED, 6=ACTIVE_TRADING, 8=DISCONNECTED)
 *   1: AI Heartbeat (1=ALIVE, 0=STALE)
 *   2: Strike Count (0-3)
 *   3: Transport Stream Health (1=HEALTHY, 0=DOWN)
 *   4: Emergency Halt (1=ACTIVE, 0=CLEAR)
 *   5: Current Position (contracts, can be negative for shorts)
 */

#include "sierrachart.h"
#include "SystemOrchestrator.h"

SCDLLName("MindfulTrader SystemOrchestrator HUD")

SCSFExport scsf_SystemOrchestratorHUD(SCStudyInterfaceRef sc) {
    // Subgraph definitions
    SCSubgraphRef SG_SystemState = sc.Subgraph[0];
    SCSubgraphRef SG_AIHeartbeat = sc.Subgraph[1];
    SCSubgraphRef SG_StrikeCount = sc.Subgraph[2];
    SCSubgraphRef SG_TransportStream = sc.Subgraph[3];
    SCSubgraphRef SG_EmergencyHalt = sc.Subgraph[4];
    SCSubgraphRef SG_Position = sc.Subgraph[5];
    
    if (sc.SetDefaults) {
        sc.GraphName = "SystemOrchestrator HUD";
        sc.StudyDescription = "Elite real-time monitoring of SystemOrchestrator Zero-Latency Cache";
        sc.AutoLoop = 1;
        sc.GraphRegion = 1;  // Separate region below chart
        sc.FreeDLL = 0;
        
        // Subgraph 0: System State
        SG_SystemState.Name = "System State";
        SG_SystemState.DrawStyle = DRAWSTYLE_STAIR;
        SG_SystemState.PrimaryColor = RGB(0, 200, 0);
        SG_SystemState.LineWidth = 2;
        SG_SystemState.DrawZeros = 1;
        
        // Subgraph 1: AI Heartbeat
        SG_AIHeartbeat.Name = "AI Heartbeat";
        SG_AIHeartbeat.DrawStyle = DRAWSTYLE_STAIR;
        SG_AIHeartbeat.PrimaryColor = RGB(0, 255, 0);
        SG_AIHeartbeat.SecondaryColor = RGB(255, 0, 0);
        SG_AIHeartbeat.SecondaryColorUsed = 1;
        SG_AIHeartbeat.LineWidth = 3;
        SG_AIHeartbeat.DrawZeros = 1;
        
        // Subgraph 2: Strike Count
        SG_StrikeCount.Name = "Strike Count";
        SG_StrikeCount.DrawStyle = DRAWSTYLE_BAR;
        SG_StrikeCount.PrimaryColor = RGB(255, 200, 0);
        SG_StrikeCount.SecondaryColor = RGB(255, 0, 0);
        SG_StrikeCount.SecondaryColorUsed = 1;
        SG_StrikeCount.LineWidth = 5;
        SG_StrikeCount.DrawZeros = 1;
        
        // Subgraph 3: Transport Stream
        SG_TransportStream.Name = "Transport Stream";
        SG_TransportStream.DrawStyle = DRAWSTYLE_STAIR;
        SG_TransportStream.PrimaryColor = RGB(0, 200, 255);
        SG_TransportStream.SecondaryColor = RGB(255, 0, 0);
        SG_TransportStream.SecondaryColorUsed = 1;
        SG_TransportStream.LineWidth = 2;
        SG_TransportStream.DrawZeros = 1;
        
        // Subgraph 4: Emergency Halt
        SG_EmergencyHalt.Name = "Emergency Halt";
        SG_EmergencyHalt.DrawStyle = DRAWSTYLE_FILLRECTTOP;
        SG_EmergencyHalt.PrimaryColor = RGB(255, 0, 0);
        SG_EmergencyHalt.LineWidth = 1;
        SG_EmergencyHalt.DrawZeros = 1;
        
        // Subgraph 5: Position
        SG_Position.Name = "Net Position";
        SG_Position.DrawStyle = DRAWSTYLE_LINE;
        SG_Position.PrimaryColor = RGB(255, 255, 0);
        SG_Position.LineWidth = 2;
        SG_Position.DrawZeros = 1;
        
        return;
    }
    
    // ===== ELITE: ZERO-LATENCY READS (Sub-microsecond) =====
    SystemOrchestrator& orchestrator = SystemOrchestrator::Instance();
    
    // Read all cache values (lock-free atomics)
    SystemState state = orchestrator.GetState();
    bool heartbeatAlive = orchestrator.IsAIHeartbeatAlive();
    int strikeCount = orchestrator.GetStrikeCount();
    bool transportStreamAlive = orchestrator.IsTransportStreamAlive();
    bool emergencyHalt = orchestrator.IsEmergencyHalt();
    int position = orchestrator.GetCurrentPosition();
    
    // Update subgraphs with current values
    int idx = sc.Index;
    
    // System State (0-8 enum)
    SG_SystemState[idx] = static_cast<float>(state);
    
    // AI Heartbeat (1=alive, 0=stale)
    SG_AIHeartbeat[idx] = heartbeatAlive ? 1.0f : 0.0f;
    SG_AIHeartbeat.DataColor[idx] = heartbeatAlive ? 
        SG_AIHeartbeat.PrimaryColor : SG_AIHeartbeat.SecondaryColor;
    
    // Strike Count (0-3)
    SG_StrikeCount[idx] = static_cast<float>(strikeCount);
    SG_StrikeCount.DataColor[idx] = (strikeCount >= 2) ? 
        SG_StrikeCount.SecondaryColor : SG_StrikeCount.PrimaryColor;
    
    // Transport Stream (1=healthy, 0=down)
    SG_TransportStream[idx] = transportStreamAlive ? 1.0f : 0.0f;
    SG_TransportStream.DataColor[idx] = transportStreamAlive ? 
        SG_TransportStream.PrimaryColor : SG_TransportStream.SecondaryColor;
    
    // Emergency Halt (1=active, 0=clear)
    SG_EmergencyHalt[idx] = emergencyHalt ? 1.0f : 0.0f;
    
    // Net Position (contracts)
    SG_Position[idx] = static_cast<float>(position);
    
    // ===== TEXT OVERLAY (Detailed Info) =====
    if (sc.Index == sc.ArraySize - 1) {  // Only on last bar
        s_UseTool Tool;
        Tool.Clear();
        Tool.ChartNumber = sc.ChartNumber;
        Tool.DrawingType = DRAWING_TEXT;
        Tool.LineNumber = 99998;  // Unique ID
        Tool.BeginDateTime = sc.BaseDateTimeIn[sc.ArraySize - 1];
        Tool.BeginValue = sc.High[sc.ArraySize - 1] + 10 * sc.TickSize;  // Above price
        Tool.Region = sc.GraphRegion;
        Tool.Color = RGB(255, 255, 255);
        Tool.FontSize = 10;
        Tool.FontBold = 1;
        
        // Build status text
        SCString statusText;
        statusText.Format(
            "System: %s | Heartbeat: %s | Strikes: %d/3 | Position: %+d | %s",
            orchestrator.GetStateString().c_str(),
            heartbeatAlive ? "✓" : "✗",
            strikeCount,
            position,
            emergencyHalt ? "🛑 HALT" : "✓ OK"
        );
        
        Tool.Text = statusText;
        Tool.AddMethod = UTAM_ADD_OR_ADJUST;
        sc.UseTool(Tool);
    }
}
