#pragma once

#include "CoreMinimal.h"
#include "Core/OffgridAITypes.h"

class UOffgridAITTSServiceSettingsDataAsset;

UENUM()
enum class EOffgridAITTSStreamEventType : uint8
{
    StreamStarted,
    AudioChunk,
    Completed,
    Error
};

struct FOffgridAITTSStreamEvent
{
    FString RequestID;
    FGuid ConversationID;
    FName NPCID = NAME_None;
    FName LineID = NAME_None;
    EOffgridAITTSStreamEventType Type = EOffgridAITTSStreamEventType::StreamStarted;
    int32 SampleRate = 0;
    int32 NumChannels = 0;

    // Phase 1A speech timeline metadata. AudioChunk events carry the exact
    // sample range of the chunk inside the synthesized utterance. StreamStarted
    // and Completed events carry the total synthesized samples known at that
    // point when available. Values are mono/sample-frame counts, not bytes.
    int64 ChunkStartSample = 0;
    int32 ChunkSampleCount = 0;
    int64 StreamTotalSamples = 0;

    TArray<uint8> PCMChunk;
    FString ErrorMessage;
};

class OFFGRIDAI_API IOffgridAITTSService
{
public:
    virtual ~IOffgridAITTSService() = default;

    virtual void Reset() = 0;
    virtual void Tick(double NowSeconds) = 0;
    virtual bool EnsureServiceReady(FString& OutError) = 0;

    virtual bool BeginSynthesis(
        const FString& RequestID,
        const FOffgridAILinePerformanceRequest& LineRequest,
        const TArray<uint8>& LoopbackPCM,
        int32 LoopbackSampleRate,
        int32 LoopbackNumChannels) = 0;

    virtual void Cancel(const FGuid& ConversationID, FName NPCID, FName LineID) = 0;
    virtual bool TryPopStreamEvent(FOffgridAITTSStreamEvent& OutStreamEvent) = 0;
};

// Debug TTS implementation used when TTS BackendKind=None.
// It intentionally performs no synthesis: it replays the most recent BoomOperator
// input audio captured through ASR. This keeps the service boundary optional while
// preserving the LineCoach streaming/playback path for debugging.
class OFFGRIDAI_API FOffgridAITTSStub final : public IOffgridAITTSService
{
public:
    explicit FOffgridAITTSStub(const UOffgridAITTSServiceSettingsDataAsset* InSettings = nullptr);

    virtual void Reset() override;
    virtual void Tick(double NowSeconds) override;
    virtual bool EnsureServiceReady(FString& OutError) override;

    virtual bool BeginSynthesis(
        const FString& RequestID,
        const FOffgridAILinePerformanceRequest& LineRequest,
        const TArray<uint8>& LoopbackPCM,
        int32 LoopbackSampleRate,
        int32 LoopbackNumChannels) override;

    virtual void Cancel(const FGuid& ConversationID, FName NPCID, FName LineID) override;
    virtual bool TryPopStreamEvent(FOffgridAITTSStreamEvent& OutStreamEvent) override;

private:
    struct FPendingRequest
    {
        bool bActive = false;
        bool bStarted = false;
        FString RequestID;
        FOffgridAILinePerformanceRequest LineRequest;
        double NextEventAtSeconds = 0.0;
        int32 SampleRate = 0;
        int32 NumChannels = 0;
        int32 NextChunkIndex = 0;
        TArray<TArray<uint8>> PCMChunks;
    };

    int32 GetPreferredChunkMs() const;
    float GetChunkIntervalSecondsForBytes(int32 ChunkBytes, int32 SampleRate, int32 NumChannels) const;

    const UOffgridAITTSServiceSettingsDataAsset* Settings = nullptr;
    double LastTickNowSeconds = 0.0;
    FPendingRequest PendingRequest;
};
