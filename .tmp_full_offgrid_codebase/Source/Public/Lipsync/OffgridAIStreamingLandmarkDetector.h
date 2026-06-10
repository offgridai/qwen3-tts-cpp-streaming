#pragma once

#include "CoreMinimal.h"

// Streaming acoustic landmark evidence extractor.
//
// This is not a general phoneme recognizer.  It emits sparse, visible acoustic
// landmark classes that can be matched against the active viseme plan.  The
// detector is causal over the audio that has arrived through the preroll buffer:
// it analyzes each eligible audio window once, applies per-class refractory
// suppression, and records raw-window diagnostics for tuning.
enum class EOffgridAIAudioLandmarkClass : uint8
{
    Unknown = 0,
    MBP,
    FV,
    WOO,
    SHCH,
    VowelOpen,
    VowelFront,
};

struct FOffgridAIAudioLandmarkCandidate
{
    EOffgridAIAudioLandmarkClass Class = EOffgridAIAudioLandmarkClass::Unknown;
    float TimeSec = 0.0f;
    float Confidence = 0.0f;
    float WindowStartSec = 0.0f;
    float WindowEndSec = 0.0f;
    float AvailableAtSec = 0.0f;
    bool bOracle = false;
};

struct FOffgridAIAudioLandmarkRawWindow
{
    float WindowStartSec = 0.0f;
    float WindowEndSec = 0.0f;
    float CenterSec = 0.0f;
    float Rms = 0.0f;
    float MeanAbs = 0.0f;
    float Zcr = 0.0f;
    float DiffMean = 0.0f;
    float LowDominance = 0.0f;
    EOffgridAIAudioLandmarkClass ProposedClass = EOffgridAIAudioLandmarkClass::Unknown;
    float ProposedConfidence = 0.0f;
    bool bAccepted = false;
    FName RejectReason = NAME_None;
};

class OFFGRIDAI_API FOffgridAIStreamingLandmarkDetector
{
public:
    void Reset();
    void Configure(float InPrerollSec);
    void PushPCM16(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample = -1);

    const TArray<FOffgridAIAudioLandmarkCandidate>& GetCandidates() const { return Candidates; }
    const TArray<FOffgridAIAudioLandmarkRawWindow>& GetRawWindows() const { return RawWindows; }
    float GetPrerollSeconds() const { return PrerollSec; }
    int32 GetSampleRate() const { return SampleRateHz; }

    static const char* ToString(EOffgridAIAudioLandmarkClass Class);
    static EOffgridAIAudioLandmarkClass LandmarkClassForPose(FName PoseID);
    static TArray<FOffgridAIAudioLandmarkCandidate> GenerateOfflineOracle(const TArray<int16>& MonoPCM16, int32 SampleRate, float PrerollSec);

private:
    void AppendMonoPCM16(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample);
    void AnalyzeNewFrames(bool bOracle, int32 AnalysisLimitFrame = INDEX_NONE);
    bool AddCandidate(EOffgridAIAudioLandmarkClass Class, float TimeSec, float Confidence, float WindowStartSec, float WindowEndSec, bool bOracle, FName& OutRejectReason);
    void AddRawWindow(const FOffgridAIAudioLandmarkRawWindow& RawWindow);

    TArray<int16> MonoPCM16;
    TArray<FOffgridAIAudioLandmarkCandidate> Candidates;
    TArray<FOffgridAIAudioLandmarkRawWindow> RawWindows;
    int32 SampleRateHz = 0;
    int32 LastAnalyzedFrame = 0;
    int64 StreamStartSample = 0;
    bool bHasStreamStartSample = false;
    float PrerollSec = 0.150f;
};
