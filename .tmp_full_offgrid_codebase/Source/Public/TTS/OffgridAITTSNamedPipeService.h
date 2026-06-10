#pragma once

#include "CoreMinimal.h"
#include "TTS/OffgridAITTSService.h"
#include "TTS/OffgridAITTSPipeProtocol.h"
#include "Templates/UniquePtr.h"

class FOffgridAITTSPipeClient;
class UOffgridAITTSServiceSettingsDataAsset;

class FOffgridAITTSNamedPipeService final : public IOffgridAITTSService
{
public:
    explicit FOffgridAITTSNamedPipeService(const UOffgridAITTSServiceSettingsDataAsset* InSettings);
    virtual ~FOffgridAITTSNamedPipeService() override;

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
    bool EnsureServiceConnected();
    bool StartupServiceIfNeeded(FString* OutFatalError = nullptr);
    bool ValidateStartupSettings(FString& OutError) const;
    bool ValidateVoiceEmbeddingPath(FName VoiceID, FString& OutError) const;
    static FString VoiceModeToProtocolString(EOffgridAITTSVoiceMode VoiceMode);
    bool SendRequest(const FOffgridAITTSRequest& Request, FOffgridAITTSResponse* OutResponse = nullptr, bool bExpectResponse = true);
    void PopulateCommonRequestFields(FOffgridAITTSRequest& OutRequest) const;
    FString BuildLaunchArguments() const;
    FString GetConfiguredSampleFormat() const;

private:
    const UOffgridAITTSServiceSettingsDataAsset* Settings = nullptr;
    TUniquePtr<FOffgridAITTSPipeClient> PipeClient;
    FProcHandle ServiceProcessHandle;
    uint32 ServiceProcessId = 0;
    bool bStartupCompleted = false;
    TQueue<FOffgridAITTSStreamEvent> StreamEventQueue;
};
