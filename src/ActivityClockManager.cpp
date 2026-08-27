#include "ActivityClockManager.h"

ActivityClockManager& ActivityClockManager::Instance() {
    static ActivityClockManager instance;
    return instance;
}

void ActivityClockManager::Init(SCStudyInterfaceRef sc) {
    (void)sc;
    Reset();
}

void ActivityClockManager::Reset() {
    m_engine.Reset();
}

void ActivityClockManager::Update(SCStudyInterfaceRef sc) {
    m_engine.OnTickWithPrice(sc.Index,
                             static_cast<float>(sc.AskVolume[sc.Index]),
                             static_cast<float>(sc.BidVolume[sc.Index]),
                             static_cast<float>(sc.Close[sc.Index]));
}
