#pragma once

#include "CoreMinimal.h"
#include "OffgridAILineCoach.h"
#include "Lipsync/OffgridAITextVisemePlanner.h"
#include "Lipsync/OffgridAIStreamingSpeechDetector.h"

// Optional helper note: landmark detection remains available in
// OffgridAIStreamingLandmarkDetector.* but is not part of this runtime path.

struct FOffgridAIAudioOccupancySchedulerState
{
    void Reset() {}
};

struct FOffgridAIAudioOccupancyDiagnosticRow
{
    FName LineID = NAME_None;
    int32 UpdateOrdinal = 0;
    bool bFinalReplay = false;
    float CurrentPlaybackSec = 0.0f;
    float PrerollSec = 0.0f;

    int32 SourceEventIndex = INDEX_NONE;
    FString Word;
    FName PoseID = NAME_None;
    float PlannedCenterSec = 0.0f;
    float CommittedCenterSec = 0.0f;
    float RenderStartSec = 0.0f;
    float RenderEndSec = 0.0f;
    FName CommitReason = NAME_None;
    FName PlaybackMode = NAME_None; // speech_active, tail_drain, final_flush, fallback
    int32 SpeechIslandIndex = INDEX_NONE;
    float SpeechIslandStartSec = 0.0f;
    float SpeechIslandEndSec = 0.0f;
    float AudioActiveSec = 0.0f;
    float TextPlayheadSec = 0.0f;

    // N04 starvation diagnostics. RequiredActiveElapsedSec is the text-plan
    // speech-active time needed to naturally publish this event.
    // ObservedActiveElapsedSec is how much detected speech-active audio was
    // available to the scheduler at placement time. A positive deficit means
    // the event needed tail/final flush rescue instead of normal active-clock
    // mapping.
    float RequiredActiveElapsedSec = 0.0f;
    float ObservedActiveElapsedSec = 0.0f;
    float ActiveProgressDeficitSec = 0.0f;
    float RequiredProgressNorm = 0.0f;
    float ObservedProgressNorm = 0.0f;
    float ActiveProgressRatio = 1.0f;
    bool bMappedToObservedSpeech = false;

    bool bTailDrain = false;
    FName DiagnosticKind = NAME_None;
};

// M09 audio-occupancy scheduler.
//
// Runtime is punctuation-unaware. It maps the monotonic text viseme timeline
// onto detected speech-active audio. Audio pauses hold the mouth because no
// speech-active time advances. If detected speech-active audio ends before the
// text plan is exhausted, remaining text may drain briefly after the last
// observed speech island. No punctuation gate, coalescing, landmark matching,
// or anchor alignment is active here.
class OFFGRIDAI_API FOffgridAIAudioOccupancyScheduler
{
public:
    static void UpdateCommittedTrack(
        const FOffgridAITextVisemePlan& TextPlan,
        const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
        const TArray<FOffgridAIStreamingAudioFeatureFrame>* AudioFeatureFrames,
        float CurrentPlaybackSec,
        float PrerollSec,
        float IslandDurationScale,
        bool bInputStreamClosed,
        FOffgridAIAudioOccupancySchedulerState& SchedulerState,
        FOffgridAIAlignedVisemeTrack& InOutTrack,
        bool& bOutHasTrack);

    static void CollectAudioOccupancyDiagnosticRows(
        const FOffgridAITextVisemePlan& TextPlan,
        const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
        const TArray<FOffgridAIStreamingAudioFeatureFrame>* AudioFeatureFrames,
        const FOffgridAIAlignedVisemeTrack& Track,
        const FOffgridAIAudioOccupancySchedulerState& SchedulerState,
        float CurrentPlaybackSec,
        float PrerollSec,
        bool bFinalReplay,
        int32 UpdateOrdinal,
        TArray<FOffgridAIAudioOccupancyDiagnosticRow>& OutRows);
};

