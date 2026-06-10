#pragma once

#include "CoreMinimal.h"
#include "OffgridAILineCoach.h"

// Phase 4. Owns envelope sampling only. It must not move centers, change pose ids,
// suppress authored events, or inspect text/audio evidence.
class OFFGRIDAI_API FOffgridAIVisemePerformer
{
public:
    static TArray<FOffgridAISubmittedVisemeSample> Sample(const FOffgridAIAlignedVisemeTrack& Track, float PlaybackSeconds, bool bGateBeforeSpeechStart = true);
    static TMap<FName, float> CollapseByPoseID(const TArray<FOffgridAISubmittedVisemeSample>& Samples);
    static TArray<FOffgridAIPerformedVisemeFrame> BuildFrames(const FOffgridAIAlignedVisemeTrack& Track, float FPS);
};
