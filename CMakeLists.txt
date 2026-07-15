cmake_minimum_required(VERSION 3.20)
project(MindfulTrader LANGUAGES CXX)

# Set the C++ standard to 17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the output directory for the final DLL
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add necessary include directories
include_directories(
    include
    sierra_chart_dependencies
    /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include
    /usr/include/eigen3
)

# High-performance flags for Clang/WSL (Elite/Institutional Grade)
if(NOT MSVC)
    add_compile_options(
        -O3
        -march=native
        -ffast-math
        -mavx2
    )
endif()

# FlatBuffer-generated schema headers (Elite v2.4)
# These are headers only - rebuild triggered if mts_schema_generated.h changes
set(GENERATED_HEADERS
    include/generated/mts_schema_generated.h
    include/generated/mts_schema_contract_generated.h
    include/generated/indicator_binding_policy_generated.h
    include/generated/indicator_key_registry_generated.h
    include/generated/event_shared_writers_generated.h
    include/generated/training_shared_writers_generated.h
)

set(GENERATED_SOURCES
    include/generated/indicator_binding_policy_generated.cpp
    include/generated/indicator_key_registry_generated.cpp
    include/generated/event_shared_writers_generated.cpp
    include/generated/training_shared_writers_generated.cpp
)

# Link directories for libraries
link_directories(
    /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/lib
    /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/debug/lib
)

# Source files for the project
set(SOURCE_FILES
    src/AIConnectionMonitor.cpp
    src/transport/AIHeartbeatMonitor.cpp
    src/transport/TransportStream.cpp
    src/messaging/EliteFlatBufferHelper.cpp
    src/messaging/EventSerializer.cpp
    src/execution/ExecutionGate.cpp
    src/BackTesterStudy.cpp
    src/TripleBarrierExitManager.cpp
    src/ContextManager.cpp
    src/CrossMarketStudies.cpp
    src/DailyHighLowLoader.cpp
    src/EventDataCollectorStudy.cpp
    src/HMMClient.cpp
    src/Indicator.cpp
    src/IndicatorManager.cpp
    src/InferenceManager.cpp
    src/KellyCalculator.cpp
    src/Logger.cpp
    src/NhNlDataLoader.cpp
    src/PositionManager.cpp
    src/PositionManagerPatterns.cpp
    src/RiskManager.cpp
    src/Scoring.cpp
    src/StructureEngine.cpp
    src/SCStudies.cpp
    src/StudyHelperFunctions.cpp
    src/SystemOrchestrator.cpp
    src/Trade.cpp
    src/TradeExecutionServer.cpp
    src/TradeSignalManager.cpp
    src/LBRFileManager.cpp
    src/TripleScreen1.cpp
    src/TripleScreen2.cpp
    src/TripleScreen3.cpp
)

# Header files (for IDE support and explicit tracking)
set(HEADER_FILES
    ${GENERATED_HEADERS}
)

# WS-07: Static shared-root write ownership audit.
# Fails build when manual shared-root writes bypass generated writer helpers.
add_custom_target(MindfulTraderSharedRootWriteAudit
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_shared_root_writes.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running WS-07 shared-root write ownership audit"
)

# WS-03: Dedicated schema contract compile target.
# This target compiles generated policy artifacts and compile-time contract checks.
add_library(MindfulTraderSchemaContract OBJECT
    src/SchemaContractChecks.cpp
    ${GENERATED_SOURCES}
    ${GENERATED_HEADERS}
)

add_dependencies(MindfulTraderSchemaContract MindfulTraderSharedRootWriteAudit)

target_include_directories(MindfulTraderSchemaContract PRIVATE
    include
    sierra_chart_dependencies
    /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include
    /usr/include/eigen3
)

if(MSVC)
    target_compile_options(MindfulTraderSchemaContract PRIVATE /W4 /WX)
else()
    target_compile_options(MindfulTraderSchemaContract PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Add the library
add_library(MindfulTrader SHARED ${SOURCE_FILES} ${HEADER_FILES})

# Contract compile target is a required build dependency for DLL generation.
add_dependencies(MindfulTrader MindfulTraderSchemaContract)
add_dependencies(MindfulTrader MindfulTraderSharedRootWriteAudit)

# Declare FlatBuffer-generated headers as build dependency
# Forces rebuild of all sources if schema header changes
set_target_properties(MindfulTrader PROPERTIES
    OBJECT_DEPENDS "${GENERATED_HEADERS}"
)

# Define the module definition file for Windows exports
set_target_properties(MindfulTrader PROPERTIES
    VS_DEF_FILE MindfulTrader.def
)

# Linker flags
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /INCREMENTAL:NO")

# Link libraries
target_link_libraries(MindfulTrader
    PRIVATE
    /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/lib/libzmq-mt-4_3_5.lib
    /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/lib/libsodium.lib
    ws2_32
    iphlpapi
    gdi32
)

# Add compiler options
if(MSVC)
    target_compile_options(MindfulTrader PRIVATE /W4 /WX)
else()
    target_compile_options(MindfulTrader PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Precompiled header
target_precompile_headers(MindfulTrader
    PRIVATE
    include/MindfulTrader_Precompiled.h
)
