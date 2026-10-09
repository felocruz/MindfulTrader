cmake_minimum_required(VERSION 3.20)
project(MindfulTrader LANGUAGES CXX)

# Set the C++ standard to 17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the output directory for the final DLL
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# vcpkg x64-windows artifacts (zmq/sodium), native-filesystem copy -- avoids the /mnt/c 9p mount
# (docs/CROSS_COMPILE_SYSROOT_MIGRATION.md Phase A)
set(VCPKG_SYSROOT "$ENV{HOME}/.local/sysroots/x86_64-pc-windows-msvc/vcpkg/x64-windows")

# --- Rust (rust/ffi, the mts_ffi staticlib) -------------------------------------------------
# OFF by default; flip ON via -DMTS_WITH_RUST=ON (or the wsl-clang-cl-release-rust preset) once
# built. See docs/superpowers/plans/2026-10-09-mindfultrader-rust-migration-plan.md §6.2.
# Path note: today rust/ is a
# direct child of this repo root (pre-merge); post-merge, once this C++ moves into cpp/ and rust/
# becomes a top-level sibling, this path gains a "../" -- update in the same commit as that move.
option(MTS_WITH_RUST "Link rust/ffi (mts_ffi.lib) into the DLL" OFF)
if(MTS_WITH_RUST)
    set(MTS_FFI_LIB "${CMAKE_SOURCE_DIR}/rust/target/x86_64-pc-windows-msvc/release/mts_ffi.lib")
    if(NOT EXISTS "${MTS_FFI_LIB}")
        message(FATAL_ERROR "Missing ${MTS_FFI_LIB} -- run scripts/build_rust.sh (or build_dll.sh, which will call it) first.")
    endif()
    add_library(mts_ffi STATIC IMPORTED GLOBAL)
    set_target_properties(mts_ffi PROPERTIES
        IMPORTED_LOCATION "${MTS_FFI_LIB}"
        # `cargo rustc -p mts_ffi --target x86_64-pc-windows-msvc --release -- --print
        # native-static-libs` -- re-run whenever a Rust dependency changes.
        INTERFACE_LINK_LIBRARIES "kernel32;ntdll;userenv;ws2_32;dbghelp")
endif()

# Add necessary include directories
include_directories(
    include
    sierra_chart_dependencies
    ${VCPKG_SYSROOT}/include
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
    ${VCPKG_SYSROOT}/lib
    ${VCPKG_SYSROOT}/debug/lib
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
    src/RegimeManager.cpp
    src/RiskManager.cpp
    src/Scoring.cpp
    src/StructureEngine.cpp
    src/ActivityClockManager.cpp
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
    src/ImbalanceScreen1.cpp
    src/ImbalanceScreen2.cpp
    src/ImbalanceScreen3.cpp
    src/ImbalanceEventDataCollectorStudy.cpp
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
    ${VCPKG_SYSROOT}/include
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
    ${VCPKG_SYSROOT}/lib/libzmq-mt-4_3_5.lib
    ${VCPKG_SYSROOT}/lib/libsodium.lib
    ws2_32
    iphlpapi
    gdi32
)

if(MTS_WITH_RUST)
    target_link_libraries(MindfulTrader PRIVATE mts_ffi)
    target_compile_definitions(MindfulTrader PRIVATE MTS_WITH_RUST=1)
    # Rust links the dynamic CRT (/MD); the toolchain forces /DEFAULTLIB:libcmt.lib. Proven twice:
    # Atratus/cpp/CMakeLists.txt and the 2026-10-07/08 Sierra spikes (consolidation spec §12.8).
    target_link_options(MindfulTrader PRIVATE "/NODEFAULTLIB:libcmt.lib")
endif()

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
