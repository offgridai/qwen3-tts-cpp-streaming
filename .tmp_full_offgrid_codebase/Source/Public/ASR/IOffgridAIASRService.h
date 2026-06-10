#pragma once

#include "CoreMinimal.h"

struct FOffgridAIASRCompletedRequest
{
    FString RequestID;
    FGuid ConversationID;
    FName PlayerID = NAME_None;
    FText Transcript;
};

class OFFGRIDAI_API IOffgridAIASRService
{
public:
    virtual ~IOffgridAIASRService() = default;

    virtual void Reset() = 0;
    virtual void Tick(double NowSeconds) = 0;

    virtual bool EnsureServiceReady(FString& OutError) = 0;
    virtual bool BeginPlayerAudioInput(const FGuid& ConversationID, FName PlayerID) = 0;
    virtual void SubmitPlayerAudioChunk(const FGuid& ConversationID, FName PlayerID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels) = 0;
    virtual bool EndPlayerAudioInput(const FString& RequestID, const FGuid& ConversationID, FName PlayerID) = 0;
    virtual bool TryPopCompletedRequest(FOffgridAIASRCompletedRequest& OutCompletedRequest) = 0;
    virtual bool GetMostRecentCapture(TArray<uint8>& OutPCM, int32& OutSampleRate, int32& OutNumChannels) const = 0;
};

class OFFGRIDAI_API FOffgridAIASRStubService final : public IOffgridAIASRService
{
public:
    virtual void Reset() override;
    virtual void Tick(double NowSeconds) override;

    virtual bool EnsureServiceReady(FString& OutError) override;
    virtual bool BeginPlayerAudioInput(const FGuid& ConversationID, FName PlayerID) override;
    virtual void SubmitPlayerAudioChunk(const FGuid& ConversationID, FName PlayerID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels) override;
    virtual bool EndPlayerAudioInput(const FString& RequestID, const FGuid& ConversationID, FName PlayerID) override;
    virtual bool TryPopCompletedRequest(FOffgridAIASRCompletedRequest& OutCompletedRequest) override;
    virtual bool GetMostRecentCapture(TArray<uint8>& OutPCM, int32& OutSampleRate, int32& OutNumChannels) const override;

private:
    struct FCaptureSession
    {
        bool bActive = false;
        FGuid ConversationID;
        FName PlayerID = NAME_None;
    };

    struct FPendingRequest
    {
        bool bActive = false;
        FString RequestID;
        FGuid ConversationID;
        FName PlayerID = NAME_None;
        double CompleteAtSeconds = 0.0;
        FText Transcript;
    };

    TArray<uint8> CapturedPCMBuffer;
    int32 CapturedSampleRate = 0;
    int32 CapturedNumChannels = 0;
    double LastTickNowSeconds = 0.0;
    FCaptureSession ActiveCaptureSession;
    FPendingRequest PendingRequest;
};
