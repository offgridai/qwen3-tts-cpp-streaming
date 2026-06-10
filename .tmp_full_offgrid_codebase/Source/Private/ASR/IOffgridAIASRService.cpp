#include "ASR/IOffgridAIASRService.h"

#include "OffgridAI.h"

namespace
{
    constexpr int32 MaxPCMBytes = 16 * 1024 * 1024;
    constexpr double CompletionDelaySeconds = 0.02;
}

void FOffgridAIASRStubService::Reset()
{
    CapturedPCMBuffer.Empty();
    CapturedSampleRate = 0;
    CapturedNumChannels = 0;
    ActiveCaptureSession = FCaptureSession();
    PendingRequest = FPendingRequest();
}

void FOffgridAIASRStubService::Tick(double NowSeconds)
{
    LastTickNowSeconds = NowSeconds;
}


bool FOffgridAIASRStubService::EnsureServiceReady(FString& OutError)
{
    OutError.Reset();
    return true;
}

bool FOffgridAIASRStubService::BeginPlayerAudioInput(const FGuid& ConversationID, FName PlayerID)
{
    Reset();
    ActiveCaptureSession.bActive = true;
    ActiveCaptureSession.ConversationID = ConversationID;
    ActiveCaptureSession.PlayerID = PlayerID;
    return true;
}

void FOffgridAIASRStubService::SubmitPlayerAudioChunk(const FGuid& ConversationID, FName PlayerID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels)
{
    if (!ActiveCaptureSession.bActive ||
        ActiveCaptureSession.ConversationID != ConversationID ||
        ActiveCaptureSession.PlayerID != PlayerID ||
        PCMChunk.IsEmpty() ||
        SampleRate <= 0 ||
        NumChannels <= 0)
    {
        return;
    }

    if (CapturedPCMBuffer.Num() + PCMChunk.Num() > MaxPCMBytes)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("ASR stub PCM buffer exceeded cap; truncating input for PlayerID=%s"), *PlayerID.ToString());
        return;
    }

    CapturedPCMBuffer.Append(PCMChunk);
    CapturedSampleRate = SampleRate;
    CapturedNumChannels = NumChannels;
}

bool FOffgridAIASRStubService::EndPlayerAudioInput(const FString& RequestID, const FGuid& ConversationID, FName PlayerID)
{
    if (!ActiveCaptureSession.bActive ||
        ActiveCaptureSession.ConversationID != ConversationID ||
        ActiveCaptureSession.PlayerID != PlayerID ||
        PendingRequest.bActive)
    {
        return false;
    }

    ActiveCaptureSession.bActive = false;

    PendingRequest.bActive = true;
    PendingRequest.RequestID = RequestID;
    PendingRequest.ConversationID = ConversationID;
    PendingRequest.PlayerID = PlayerID;
    PendingRequest.CompleteAtSeconds = LastTickNowSeconds + CompletionDelaySeconds;
    PendingRequest.Transcript = CapturedPCMBuffer.IsEmpty()
        ? FText::FromString(TEXT("Testing one two three."))
        : FText::FromString(TEXT("Hello there."));
    return true;
}

bool FOffgridAIASRStubService::TryPopCompletedRequest(FOffgridAIASRCompletedRequest& OutCompletedRequest)
{
    if (!PendingRequest.bActive || LastTickNowSeconds < PendingRequest.CompleteAtSeconds)
    {
        return false;
    }

    OutCompletedRequest.RequestID = PendingRequest.RequestID;
    OutCompletedRequest.ConversationID = PendingRequest.ConversationID;
    OutCompletedRequest.PlayerID = PendingRequest.PlayerID;
    OutCompletedRequest.Transcript = PendingRequest.Transcript;
    PendingRequest = FPendingRequest();
    return true;
}


bool FOffgridAIASRStubService::GetMostRecentCapture(TArray<uint8>& OutPCM, int32& OutSampleRate, int32& OutNumChannels) const
{
    if (CapturedPCMBuffer.IsEmpty() || CapturedSampleRate <= 0 || CapturedNumChannels <= 0)
    {
        return false;
    }

    OutPCM = CapturedPCMBuffer;
    OutSampleRate = CapturedSampleRate;
    OutNumChannels = CapturedNumChannels;
    return true;
}
