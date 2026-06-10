#pragma once

#include "CoreMinimal.h"
#include "Lipsync/OffgridAITextVisemePlanner.h"

// Phase 2. Owns salience/realization only.
// It may change Strength and event-level budget metadata, but it must not invent,
// reorder, retime, or replace symbolic viseme identity.
class OFFGRIDAI_API FOffgridAIArticulationBudgeter
{
public:
    static FOffgridAITextVisemePlan Budget(const FOffgridAITextVisemePlan& InPlan);
};
