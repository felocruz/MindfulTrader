#pragma once

#include "sierrachart.h"
#include "ImbalanceBarEngine.h"

class ActivityClockManager {
public:
    static ActivityClockManager& Instance();

    void Init(SCStudyInterfaceRef sc);
    void Reset();
    void Update(SCStudyInterfaceRef sc);

    const ImbalanceBarEngine& Engine() const { return m_engine; }

private:
    ActivityClockManager() = default;
    ImbalanceBarEngine m_engine;
};
