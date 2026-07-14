#pragma once

// Set the Windows version and prevent winsock.h from being included.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Target Windows 10
#endif

// This macro is the key to solving the 'min' and 'max' macro conflicts.
#ifndef NOMINMAX
#define NOMINMAX
#endif

// #include <sdkddkver.h>

// Core Sierra Chart dependency
#include "sierrachart.h"

// Common C++ Standard Library headers
#include <string>
#include <string_view>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <numeric>
#include <bitset>
#include <algorithm>
#include <vector>
#include <memory>
#include <map>
#include <unordered_set>
#include <utility>
#include <cstdint>
#include <cstring>
#include <exception>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>


// Third-party libraries (Boost)
#include <nlohmann/json.hpp>

// FlatBuffers (Elite v2.4 optimization)
#include "flatbuffers/flatbuffers.h"
#include "generated/mts_schema_generated.h"

// Your main project headers
#include "MindfulTraderConstants.h"
#include "TradeCommunication.h"
#include "Logger.h"
#include "AIConnectionMonitor.h"
#include "StudyHelperFunctions.h"
#include "Trade.h"
#include "Indicator.h"
#include "ContextManager.h"
#include "HMMClient.h"
#include "InferenceManager.h"
#include "IndicatorManager.h"
#include "PositionManager.h"
#include "RiskManager.h"
#include "TradeSignalManager.h"
#include "ZMQContextManager.h"
// Deprecated in Elite v2.4: MindfulSocketZMQ.h
// Replaced by: TransportStream (include/transport/TransportStream.h)
// Event publishing now uses: TransportStream::Instance().Emit(...)
#include "transport/TransportStream.h"
using MTS::Transport::TransportStream;
#include "DailyHighLowLoader.h"
#include "TradeExecutionServer.h"
#include "NhNlDataLoader.h"
#include "SystemOrchestrator.h"
#include "messaging/EventSerializer.h"
