// TripleBarrierExitManager.cpp — translation unit for the Phase 1 glue.
//
// Currently just defines the singleton accessor; the barrier logic is inline in
// the header (hot-path) and the math lives in the SC-free pure core
// (TripleBarrierEngine.h). This TU exists so the DLL build compiles the glue
// under clang-cl (the integration-level confirmation for the core + enum
// mappings). Not yet referenced by PositionManager — additive Phase 1 step.

#include "TripleBarrierExitManager.h"

TripleBarrierExitManager& TripleBarrierExitManager::getInstance() {
    static TripleBarrierExitManager instance;
    return instance;
}
