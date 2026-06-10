#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/UniquePtr.h"

#include "Core/OffgridAITypes.h"
#include "Core/OffgridAILatencyTelemetry.h"
#include "Core/OffgridAIServiceGateway.h"
#include "Data/OffgridAIConversationPromptDataAsset.h"
#include "OffgridAIOrchestrator.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOffgridAIOrchestratorStateChanged, EOffgridAIOrchestratorState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOffgridAIConversationStateChanged, EOffgridAIConversationState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOffgridAIConversationCreated);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOffgridAIConversationEnded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOffgridAITranscriptLineAdded, FOffgridAITranscriptLine, TranscriptLine);

class UOffgridAIASRServiceSettingsDataAsset;
class UOffgridAILLMServiceSettingsDataAsset;
class UOffgridAITTSServiceSettingsDataAsset;
class UOffgridAIConversationManager;
class UOffgridAIBoomOperator;
class UOffgridAILineCoach;

UCLASS()
class OFFGRIDAI_API UOffgridAIOrchestrator : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual ~UOffgridAIOrchestrator() override;

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void ReplayCurrentState();

    void InstallServices(TUniquePtr<IOffgridAIServiceGateway> InServices);
    void HandleServiceStatusChanged(const FOffgridAIServiceStatus& ServiceStatus);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void RegisterBoomOperator(UOffgridAIBoomOperator* BoomOperator);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void UnregisterBoomOperator(FName PlayerID, UOffgridAIBoomOperator* BoomOperator);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void RegisterLineCoach(UOffgridAILineCoach* LineCoach);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void UnregisterLineCoach(FName NPCID, UOffgridAILineCoach* LineCoach);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void SetASRServiceSettings(UOffgridAIASRServiceSettingsDataAsset* InASRServiceSettings);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void SetLLMServiceSettings(UOffgridAILLMServiceSettingsDataAsset* InLLMServiceSettings);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void SetTTSServiceSettings(UOffgridAITTSServiceSettingsDataAsset* InTTSServiceSettings);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    bool CreateConversation(
        const TArray<FName>& PlayerIDs,
        const TArray<FName>& NPCIDs,
        UOffgridAIConversationPromptDataAsset* ConversationPromptAsset);

    // Convenience entry point for scene-driven setup: every registered BoomOperator
    // and every registered LineCoach becomes part of the conversation. This keeps
    // the multi-NPC path automatic once an additional LineCoach is dropped into
    // the scene and given a unique NPCID/VoiceID.
    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    bool CreateConversationFromRegisteredParticipants(
        UOffgridAIConversationPromptDataAsset* ConversationPromptAsset);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void ResetRuntime();

    bool BeginPlayerAudioInput(FName PlayerID);
    void SubmitPlayerAudioChunk(FName PlayerID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels);
    void EndPlayerAudioInput(FName PlayerID);

    // RTT metric anchor used by BoomOperator before it finalizes captured player audio.
    // Implementation may live in OffgridAIOrchestrator_RTTLinkFix.cpp in existing projects.
    void NotifyBoomOperatorFinalizeRequested(FName InputSourceID);

    void SubmitConversationTurnToLLM(const FGuid& ConversationID, const FText& PlayerText);
    void SubmitPlayerImpulseClassifierToLLM(const FGuid& ConversationID, const FText& PlayerText);
    void SubmitDialogueTurnToLLM(const FGuid& ConversationID, const FText& PlayerText);
    bool GetNPCStartingPADSState(FName NPCID, FOffgridAIPADSState& OutState) const;
    float GetEmotionStateStep() const;
    bool ResetLLMSessionForActiveConversation(const FGuid& ConversationID);
    void DispatchNPCLine(const FGuid& ConversationID, const FOffgridAILinePerformanceRequest& LineRequest);
    void BeginNPCLineSequence(const FGuid& ConversationID, const TArray<FOffgridAILinePerformanceRequest>& LineRequests);
    void SubmitHiddenPlayerTurnThroughNormalPath(const FGuid& ConversationID, FName PlayerID, const FText& HiddenPlayerText);

    void HandleConversationStateChanged(const FGuid& ConversationID, EOffgridAIConversationState NewState);
    void PlayerTextTranscriptReady(const FGuid& ConversationID, const FOffgridAITranscriptLine& TranscriptLine);
    void NPCTextTranscriptReady(const FGuid& ConversationID, const FOffgridAITranscriptLine& TranscriptLine);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void NotifyLineFirstAudioSample(const FGuid& ConversationID, FName LineID, int32 BytesQueued);
    void NotifyLineOutputPlaybackStarted(const FGuid& ConversationID, FName LineID);

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void NotifyLinePerformanceCompleted(const FGuid& ConversationID, FName LineID);

    UFUNCTION(BlueprintPure, Category = "OffgridAI")
    bool HasActiveConversation() const;

    UFUNCTION(BlueprintPure, Category = "OffgridAI")
    FGuid GetActiveConversationID() const;

    UFUNCTION(BlueprintPure, Category = "OffgridAI")
    TArray<FName> GetRegisteredPlayerIDs() const;

    UFUNCTION(BlueprintPure, Category = "OffgridAI")
    TArray<FName> GetRegisteredNPCIDs() const;

    UFUNCTION(BlueprintPure, Category = "OffgridAI")
    EOffgridAIOrchestratorState GetOrchestratorState() const { return CurrentOrchestratorState; }

    UFUNCTION(BlueprintPure, Category = "OffgridAI")
    EOffgridAIConversationState GetConversationState() const { return CurrentConversationState; }

    bool TryGetVoiceIDForNPC(FName NPCID, FName& OutVoiceID) const;
    bool GetNPCSupportedEmotionNames(FName NPCID, TArray<FName>& OutSupportedEmotionNames) const;
    void DriveNPCEmotionExpression(FName NPCID, FName Emotion, float Magnitude);
    TArray<FName> GetSupportedEmotionNamesForNPCs(const TArray<FName>& NPCIDs) const;

    void HandleASRFinalTranscript(const FGuid& ConversationID, FName PlayerID, const FText& Transcript);
    void HandleLLMResponse(const FGuid& ConversationID, const FString& JSONPayload);
    void HandleTTSStreamStarted(const FGuid& ConversationID, FName NPCID, FName LineID, int32 SampleRate, int32 NumChannels);
    void HandleTTSStreamChunk(const FGuid& ConversationID, FName NPCID, FName LineID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample = 0, int32 ChunkSampleCount = 0);
    void HandleTTSStreamCompleted(const FGuid& ConversationID, FName NPCID, FName LineID);
    void HandleServiceError(const FGuid& ConversationID, const FString& Context);

    void NoteTurnLatencyEvent(const FString& EventName, const FString& Detail = FString());

    UFUNCTION(BlueprintPure, Category = "OffgridAI")
    bool AreRequiredServicesReady() const;

    UFUNCTION(BlueprintPure, Category = "OffgridAI")
    FOffgridAIServiceStatus GetServiceStatus(EOffgridAIServiceKind ServiceKind) const;

    UPROPERTY(BlueprintAssignable, Category = "OffgridAI")
    FOffgridAIOrchestratorStateChanged OnOrchestratorStateChanged;

    UPROPERTY(BlueprintAssignable, Category = "OffgridAI")
    FOffgridAIConversationStateChanged OnConversationStateChanged;

    UPROPERTY(BlueprintAssignable, Category = "OffgridAI")
    FOffgridAIConversationCreated OnConversationCreated;

    UPROPERTY(BlueprintAssignable, Category = "OffgridAI")
    FOffgridAIConversationEnded OnConversationEnded;

    UPROPERTY(BlueprintAssignable, Category = "OffgridAI")
    FOffgridAITranscriptLineAdded OnTranscriptLineAdded;

protected:
    void SetOrchestratorState(EOffgridAIOrchestratorState NewState);
    void SetConversationState(EOffgridAIConversationState NewState);
    void BroadcastTranscriptLine(const FOffgridAITranscriptLine& TranscriptLine);

    bool HasRequiredRegistrants() const;
    bool ValidateConversationParticipants(const TArray<FName>& PlayerIDs, const TArray<FName>& NPCIDs) const;
    bool IsActiveConversationID(const FGuid& ConversationID) const;

    UOffgridAIBoomOperator* FindBoomOperatorByID(FName PlayerID) const;
    UOffgridAILineCoach* FindLineCoachByID(FName NPCID) const;

private:
    bool HandleDeferredStateReplay(float DeltaTimeSeconds);
    bool HandleDeferredConversationPrime(float DeltaTimeSeconds);
    bool HandleDeferredHiddenPlayerTurnFinalize(float DeltaTimeSeconds);
    bool HandleDeferredHiddenPlayerTurnTranscript(float DeltaTimeSeconds);
    void QueueDeferredStateReplay();
    void QueueDeferredConversationPrime();
    void ClearDeferredConversationPrime();
    void ClearDeferredHiddenPlayerTurn();

    bool HandleDeferredNPCLineDispatch(float DeltaTimeSeconds);
    bool HandleEmotionTransitionTick(float DeltaTimeSeconds);
    void QueueDeferredNPCLineDispatch(const FGuid& ConversationID, const FOffgridAILinePerformanceRequest& LineRequest);
    void ClearDeferredNPCLineDispatch();
    bool IsTTSReadyToAcceptRequest() const;

    struct FBufferedTTSLine
    {
        FOffgridAILinePerformanceRequest Request;
        bool bRequestAccepted = false;
        bool bStreamStarted = false;
        bool bStreamCompleted = false;
        int32 SampleRate = 0;
        int32 NumChannels = 0;
        struct FBufferedPCMChunk
        {
            TArray<uint8> PCM;
            int64 ChunkStartSample = 0;
            int32 ChunkSampleCount = 0;
        };
        TArray<FBufferedPCMChunk> PCMChunks;
    };

    void ClearNPCLineSequence();
    void PumpNPCLinePipeline();
    bool RequestTTSSynthesisForLineIndex(int32 LineIndex);
    bool StartPlaybackForLineIndex(int32 LineIndex);
    int32 FindBufferedLineIndexByLineID(FName LineID) const;
    void DeliverBufferedTTSLineToLineCoach(FBufferedTTSLine& BufferedLine, UOffgridAILineCoach* LineCoach);

    void TearDownActiveConversation(bool bPreserveRegistrations);
    void RefreshIdleRuntimeState();
    void ReinstallLocalServices();
    void EnsureLocalServicesInstalledIfNeeded();
    void BeginTurnLatencyTraceIfNeeded(const FGuid& ConversationID, FName PlayerID);
    void FinalizeTurnLatencyTrace(const FString& Outcome);
    void PublishPerformanceMetricWindows();

    void StartRoundTripMetric(const FGuid& ConversationID, FName PlayerID);
    void CompleteRoundTripMetricIfActive(const FGuid& ConversationID, FName LineID);
    void ClearRoundTripMetric();

    UPROPERTY()
    TObjectPtr<UOffgridAIConversationManager> ActiveConversationManager;

    UPROPERTY()
    TMap<FName, TObjectPtr<UOffgridAIBoomOperator>> RegisteredBoomOperatorsByID;

    UPROPERTY()
    TMap<FName, TObjectPtr<UOffgridAILineCoach>> RegisteredLineCoachesByID;

    TUniquePtr<IOffgridAIServiceGateway> Services;

    UPROPERTY()
    TObjectPtr<UOffgridAIASRServiceSettingsDataAsset> ASRServiceSettings;

    UPROPERTY()
    TObjectPtr<UOffgridAILLMServiceSettingsDataAsset> LLMServiceSettings;

    UPROPERTY()
    TObjectPtr<UOffgridAITTSServiceSettingsDataAsset> TTSServiceSettings;

    EOffgridAIOrchestratorState CurrentOrchestratorState = EOffgridAIOrchestratorState::Inactive;
    EOffgridAIConversationState CurrentConversationState = EOffgridAIConversationState::Inactive;
    FName ActiveInputPlayerID = NAME_None;
    FTSTicker::FDelegateHandle DeferredStateReplayHandle;
    FTSTicker::FDelegateHandle DeferredConversationPrimeHandle;
    FTSTicker::FDelegateHandle DeferredHiddenPlayerTurnFinalizeHandle;
    FTSTicker::FDelegateHandle DeferredHiddenPlayerTurnTranscriptHandle;
    bool bHasDeferredHiddenPlayerTurn = false;
    FGuid DeferredHiddenPlayerConversationID;
    FName DeferredHiddenPlayerID = NAME_None;
    FText DeferredHiddenPlayerText;
    FTSTicker::FDelegateHandle DeferredNPCLineDispatchHandle;
    FTSTicker::FDelegateHandle EmotionTransitionTickHandle;
    bool bHasDeferredNPCLineDispatch = false;
    FGuid DeferredNPCLineConversationID;
    FOffgridAILinePerformanceRequest DeferredNPCLineRequest;

    bool bAwaitingPlayerImpulseClassifierResponse = false;
    FGuid PendingPlayerImpulseConversationID;
    FText PendingPlayerImpulseText;

    TArray<FBufferedTTSLine> NPCLinePipeline;
    FGuid NPCLinePipelineConversationID;
    int32 ActiveNPCPlaybackIndex = INDEX_NONE;
    int32 ActiveTTSGenerationIndex = INDEX_NONE;

    FOffgridAITurnLatencyTrace ActiveTurnLatencyTrace;
    FOffgridAIMetricAccumulator PerformanceMetricAccumulator;
    int32 TurnLatencyTraceCounter = 0;

    bool bRoundTripMetricActive = false;
    FGuid RoundTripMetricConversationID;
    FName RoundTripMetricPlayerID = NAME_None;
    double RoundTripMetricStartSeconds = 0.0;
};
