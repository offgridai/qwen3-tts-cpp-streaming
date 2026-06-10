#pragma once

#include "CoreMinimal.h"
#include "IOffgridAIASRService.h"
#include "OffgridAIASRProtocol.h"
#include "OffgridAIASRRuntimeConfig.h"
#include "Data/OffgridAIASRServiceSettingsDataAsset.h"
#include "Templates/UniquePtr.h"

class FOffgridAIASRPipeClient;

class FOffgridAIASRNamedPipeService : public IOffgridAIASRService
{
public:
    explicit FOffgridAIASRNamedPipeService(const UOffgridAIASRServiceSettingsDataAsset* InSettings);
    virtual ~FOffgridAIASRNamedPipeService() override;

    virtual void Reset() override;
    virtual void Tick(double NowSeconds) override;

    virtual bool EnsureServiceReady(FString& OutError) override;
    virtual bool BeginPlayerAudioInput(const FGuid& ConversationID, FName PlayerID) override;

    virtual void SubmitPlayerAudioChunk(
        const FGuid& ConversationID,
        FName PlayerID,
        const TArray<uint8>& PCMData,
        int32 SampleRate,
        int32 NumChannels) override;

    virtual bool EndPlayerAudioInput(
        const FString& RequestID,
        const FGuid& ConversationID,
        FName PlayerID) override;

    virtual bool TryPopCompletedRequest(FOffgridAIASRCompletedRequest& OutResult) override;

    virtual bool GetMostRecentCapture(
        TArray<uint8>& OutPCM,
        int32& OutSampleRate,
        int32& OutNumChannels) const override;

private:
    struct FActiveSession
    {
        FString RequestID;
        FGuid ConversationID;
        FName PlayerID = NAME_None;
        bool bActive = false;
    };

    bool EnsureServiceConnected();
    bool StartupServiceIfNeeded(FString* OutFatalError = nullptr);
    bool ValidateStartupSettings(FString& OutError) const;
    bool BuildRuntimeConfig(FOffgridAIASRRuntimeConfig& OutConfig, FString& OutError) const;
    bool SendRequest(
        EOffgridAIASROp Op,
        const FString& RequestId,
        const TArray<uint8>& Payload,
        int32 SampleRate,
        int32 NumChannels,
        const FString& SampleFormat,
        FOffgridAIASRResponse* OutResponse = nullptr,
        bool bExpectResponse = true,
        const FOffgridAIASRRuntimeConfig* RuntimeConfig = nullptr);
    FString BuildLaunchArguments() const;
    FString GetConfiguredSampleFormat() const;

private:
    const UOffgridAIASRServiceSettingsDataAsset* Settings = nullptr;
    TUniquePtr<FOffgridAIASRPipeClient> PipeClient;
    FProcHandle ServiceProcessHandle;
    uint32 ServiceProcessId = 0;
    bool bStartupCompleted = false;

    FActiveSession ActiveSession;
    TQueue<FOffgridAIASRCompletedRequest> CompletedQueue;

    TArray<uint8> CapturedPCMBuffer;
    int32 CapturedSampleRate = 0;
    int32 CapturedNumChannels = 0;
    int32 SubmittedPCMBytes = 0;
};
