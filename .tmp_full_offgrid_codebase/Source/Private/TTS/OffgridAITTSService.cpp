#include "TTS/OffgridAITTSService.h"

#include "Data/OffgridAITTSServiceSettingsDataAsset.h"
#include "OffgridAI.h"

namespace
{
    constexpr int32 DefaultPreferredChunkMs = 40;
}

FOffgridAITTSStub::FOffgridAITTSStub(const UOffgridAITTSServiceSettingsDataAsset* InSettings)
    : Settings(InSettings)
{
}

bool FOffgridAITTSStub::EnsureServiceReady(FString& OutError)
{
    OutError.Reset();
    return true;
}

void FOffgridAITTSStub::Reset()
{
    PendingRequest = FPendingRequest();
}

void FOffgridAITTSStub::Tick(double NowSeconds)
{
    LastTickNowSeconds = NowSeconds;
}

bool FOffgridAITTSStub::BeginSynthesis(
    const FString& RequestID,
    const FOffgridAILinePerformanceRequest& LineRequest,
    const TArray<uint8>& LoopbackPCM,
    int32 LoopbackSampleRate,
    int32 LoopbackNumChannels)
{
    PendingRequest = FPendingRequest();
    PendingRequest.bActive = true;
    PendingRequest.RequestID = RequestID;
    PendingRequest.LineRequest = LineRequest;
    PendingRequest.NextEventAtSeconds = LastTickNowSeconds;
    PendingRequest.SampleRate = LoopbackSampleRate;
    PendingRequest.NumChannels = LoopbackNumChannels;

    if (!LoopbackPCM.IsEmpty() && LoopbackSampleRate > 0 && LoopbackNumChannels > 0)
    {
        const int32 BytesPerFrame = FMath::Max(LoopbackNumChannels * static_cast<int32>(sizeof(int16)), static_cast<int32>(sizeof(int16)));
        const int32 BytesPerSecond = LoopbackSampleRate * BytesPerFrame;
        const int32 PreferredChunkBytesRaw = FMath::Max((BytesPerSecond * GetPreferredChunkMs()) / 1000, BytesPerFrame);
        const int32 ChunkBytes = FMath::Max((PreferredChunkBytesRaw / BytesPerFrame) * BytesPerFrame, BytesPerFrame);
        const int32 LoopbackPlayableBytes = (LoopbackPCM.Num() / BytesPerFrame) * BytesPerFrame;

        for (int32 Offset = 0; Offset < LoopbackPlayableBytes; Offset += ChunkBytes)
        {
            const int32 RemainingBytes = LoopbackPlayableBytes - Offset;
            const int32 ThisChunkBytes = FMath::Min(ChunkBytes, RemainingBytes);
            TArray<uint8> PCMChunk;
            PCMChunk.Append(LoopbackPCM.GetData() + Offset, ThisChunkBytes);
            PendingRequest.PCMChunks.Add(MoveTemp(PCMChunk));
        }

        UE_LOG(LogOffgridAI, Log,
            TEXT("[TTS][Loopback] request=%s replaying captured player audio bytes=%d sample_rate=%d channels=%d chunks=%d"),
            *RequestID,
            LoopbackPCM.Num(),
            LoopbackSampleRate,
            LoopbackNumChannels,
            PendingRequest.PCMChunks.Num());
    }
    else
    {
        PendingRequest.SampleRate = 24000;
        PendingRequest.NumChannels = 1;

        UE_LOG(LogOffgridAI, Warning,
            TEXT("[TTS][Loopback] request=%s has no captured player audio to replay; stream will complete silently. npc=%s line=%s"),
            *RequestID,
            *LineRequest.NPCID.ToString(),
            *LineRequest.LineID.ToString());
    }

    return true;
}

void FOffgridAITTSStub::Cancel(const FGuid& ConversationID, FName NPCID, FName LineID)
{
    if (!PendingRequest.bActive)
    {
        return;
    }

    if (PendingRequest.LineRequest.ConversationID == ConversationID &&
        PendingRequest.LineRequest.NPCID == NPCID &&
        PendingRequest.LineRequest.LineID == LineID)
    {
        PendingRequest = FPendingRequest();
    }
}

bool FOffgridAITTSStub::TryPopStreamEvent(FOffgridAITTSStreamEvent& OutStreamEvent)
{
    if (!PendingRequest.bActive || LastTickNowSeconds < PendingRequest.NextEventAtSeconds)
    {
        return false;
    }

    OutStreamEvent = FOffgridAITTSStreamEvent();
    OutStreamEvent.RequestID = PendingRequest.RequestID;
    OutStreamEvent.ConversationID = PendingRequest.LineRequest.ConversationID;
    OutStreamEvent.NPCID = PendingRequest.LineRequest.NPCID;
    OutStreamEvent.LineID = PendingRequest.LineRequest.LineID;
    OutStreamEvent.SampleRate = PendingRequest.SampleRate;
    OutStreamEvent.NumChannels = PendingRequest.NumChannels;

    if (!PendingRequest.bStarted)
    {
        PendingRequest.bStarted = true;
        PendingRequest.NextEventAtSeconds = LastTickNowSeconds;
        OutStreamEvent.Type = EOffgridAITTSStreamEventType::StreamStarted;
        return true;
    }

    if (PendingRequest.NextChunkIndex < PendingRequest.PCMChunks.Num())
    {
        OutStreamEvent.Type = EOffgridAITTSStreamEventType::AudioChunk;
        OutStreamEvent.PCMChunk = PendingRequest.PCMChunks[PendingRequest.NextChunkIndex++];
        PendingRequest.NextEventAtSeconds = LastTickNowSeconds + static_cast<double>(GetChunkIntervalSecondsForBytes(
            OutStreamEvent.PCMChunk.Num(),
            PendingRequest.SampleRate,
            PendingRequest.NumChannels));
        return true;
    }

    OutStreamEvent.Type = EOffgridAITTSStreamEventType::Completed;
    PendingRequest = FPendingRequest();
    return true;
}

int32 FOffgridAITTSStub::GetPreferredChunkMs() const
{
    return (Settings && Settings->PreferredChunkMs > 0)
        ? Settings->PreferredChunkMs
        : DefaultPreferredChunkMs;
}

float FOffgridAITTSStub::GetChunkIntervalSecondsForBytes(int32 ChunkBytes, int32 SampleRate, int32 NumChannels) const
{
    // Retained for ABI/source compatibility with older callers; the loopback path now emits
    // chunks as fast as poll_event drains them to avoid debug passthrough underruns.
    return 0.0f;
}
