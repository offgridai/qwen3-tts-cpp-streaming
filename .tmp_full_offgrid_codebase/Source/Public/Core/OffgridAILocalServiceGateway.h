#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/UniquePtr.h"

#include "Core/OffgridAIServiceGateway.h"

class IOffgridAIASRService;
class IOffgridAILLMService;
class IOffgridAITTSService;
class UOffgridAIASRServiceSettingsDataAsset;
class UOffgridAILLMServiceSettingsDataAsset;
class UOffgridAITTSServiceSettingsDataAsset;
class UOffgridAIConversationPromptDataAsset;
class UOffgridAIOrchestrator;
class FOffgridAIServiceSupervisor;

/*
Local services implementation used by the demo today.

This owns exactly three runtime channels:
- ASR
- LLM
- TTS

Each runtime owns lifecycle state. Each service beneath it owns the actual work.
For 1.0 the test path is preserved by selecting None in the per-service data asset,
or by falling back to Stub in the legacy ServiceSelection.
*/
class OFFGRIDAI_API FOffgridAILocalServiceGateway : public IOffgridAIServiceGateway
{
public:
    explicit FOffgridAILocalServiceGateway(
        const FOffgridAIServiceSelection& InSelection = FOffgridAIServiceSelection(),
        const FOffgridAIServiceSupervisorSettings& InDefaultSettings = FOffgridAIServiceSupervisorSettings(),
        const UOffgridAIASRServiceSettingsDataAsset* InASRServiceSettings = nullptr,
        const UOffgridAILLMServiceSettingsDataAsset* InLLMServiceSettings = nullptr,
        const UOffgridAITTSServiceSettingsDataAsset* InTTSServiceSettings = nullptr);
    virtual ~FOffgridAILocalServiceGateway() override;

    FOffgridAILocalServiceGateway(const FOffgridAILocalServiceGateway&) = delete;
    FOffgridAILocalServiceGateway& operator=(const FOffgridAILocalServiceGateway&) = delete;

    virtual void Initialize(UOffgridAIOrchestrator* InOrchestrator) override;
    virtual void Shutdown() override;
    virtual void StartupServices() override;
    virtual void Tick(float DeltaTimeSeconds) override;

    virtual bool AreRequiredServicesReady() const override;
    virtual FOffgridAIServiceStatus GetServiceStatus(EOffgridAIServiceKind ServiceKind) const override;

    virtual bool BeginPlayerAudioInput(const FGuid& ConversationID, FName PlayerID) override;
    virtual void SubmitPlayerAudioChunk(const FGuid& ConversationID, FName PlayerID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels) override;
    virtual void EndPlayerAudioInput(const FGuid& ConversationID, FName PlayerID) override;

    virtual bool InitializeLLMSession(
        const FGuid& ConversationID,
        const TArray<FName>& NPCIDs,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord) override;

    virtual bool SubmitLLMRequest(
        const FGuid& ConversationID,
        const TArray<FName>& NPCIDs,
        const FText& PlayerText,
        EOffgridAILLMRequestKind RequestKind,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
        const FString& PersistentEmotionState,
        const TArray<FName>& SupportedEmotionNames) override;

    virtual void CancelLLMRequest(const FGuid& ConversationID) override;
    virtual void ClearLLMSession(const FGuid& ConversationID) override;

    virtual bool BeginTTSRequest(const FOffgridAILinePerformanceRequest& LineRequest) override;
    virtual void CancelTTS(const FGuid& ConversationID, FName NPCID, FName LineID) override;

private:

    struct FOffgridAIASRActiveInput
    {
        bool bActive = false;
        FString RequestID;
        FGuid ConversationID;
        FName PlayerID = NAME_None;
    };

    bool HandleTicker(float DeltaTimeSeconds);
    static FString MakeRequestID(EOffgridAIServiceKind ServiceKind);
    FOffgridAIServiceSupervisor* FindRuntime(EOffgridAIServiceKind ServiceKind) const;
    void HandleServiceEvent(const FOffgridAIServiceEvent& ServiceEvent);
    void HandleServiceStatusChanged(const FOffgridAIServiceStatus& ServiceStatus);
    void ResetServiceState(EOffgridAIServiceKind ServiceKind);
    EOffgridAIServiceImplementation ResolveASRImplementation() const;
    EOffgridAIServiceImplementation ResolveLLMImplementation() const;
    EOffgridAIServiceImplementation ResolveTTSImplementation() const;

    TUniquePtr<IOffgridAIASRService> CreateASRService(EOffgridAIServiceImplementation Implementation) const;
    TUniquePtr<IOffgridAILLMService> CreateLLMService(EOffgridAIServiceImplementation Implementation) const;
    TUniquePtr<IOffgridAITTSService> CreateTTSService(EOffgridAIServiceImplementation Implementation) const;

    void TickASR(double NowSeconds);
    void TickServiceStartup(double NowSeconds);
    void TickLLM(double NowSeconds);
    void TickTTS(double NowSeconds);

    TWeakObjectPtr<UOffgridAIOrchestrator> Orchestrator;
    FOffgridAIServiceSelection ServiceSelection;
    FOffgridAIServiceSupervisorSettings DefaultRuntimeSettings;
    const UOffgridAIASRServiceSettingsDataAsset* ASRServiceSettings = nullptr;
    const UOffgridAILLMServiceSettingsDataAsset* LLMServiceSettings = nullptr;
    const UOffgridAITTSServiceSettingsDataAsset* TTSServiceSettings = nullptr;

    TUniquePtr<FOffgridAIServiceSupervisor> ASRRuntime;
    TUniquePtr<FOffgridAIServiceSupervisor> LLMRuntime;
    TUniquePtr<FOffgridAIServiceSupervisor> TTSRuntime;

    TUniquePtr<IOffgridAIASRService> ASRService;
    TUniquePtr<IOffgridAILLMService> LLMService;
    TUniquePtr<IOffgridAITTSService> TTSService;

    TMap<EOffgridAIServiceKind, FOffgridAIServiceStatus> CachedStatuses;
    FOffgridAIASRActiveInput ActiveASRInput;
    FTSTicker::FDelegateHandle TickHandle;
};
