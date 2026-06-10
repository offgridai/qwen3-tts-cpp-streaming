#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/OffgridAITypes.h"
#include "Data/OffgridAIConversationPromptDataAsset.h"
#include "OffgridAIConversationManager.generated.h"

class UOffgridAIOrchestrator;

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAIConversationManager : public UObject
{
    GENERATED_BODY()

public:
    void InitializeConversation(
        UOffgridAIOrchestrator* InOrchestrator,
        const TArray<FName>& InPlayerIDs,
        const TArray<FName>& InNPCIDs,
        UOffgridAIConversationPromptDataAsset* InConversationPromptAsset);

    void MarkPrimedAndAwaitingInput();
    void SubmitHiddenPlayerTurnText(const FText& HiddenPlayerText);
    void SubmitHiddenPlayerTurnTextAfterSyntheticASR(const FText& HiddenPlayerText);
    void BeginRecording();
    void BeginProcessingASR();
    void SubmitPlayerTurnText(FName PlayerID, const FText& PlayerText);
    void SubmitPlayerTurnText(const FText& PlayerText);
    void SubmitNPCTurnJSON(const FString& JSONPayload);
    bool ApplyPlayerImpulseClassifierResult(const FString& RawClassifierResponse, const FText& PlayerText);
    void NPCLinePerformanceComplete(FName LineID);
    void CancelCurrentTurn();

    const FGuid& GetConversationID() const { return ConversationID; }
    EOffgridAIConversationState GetConversationState() const { return CurrentConversationState; }
    UOffgridAIConversationPromptDataAsset* GetConversationPromptAsset() const { return ConversationPromptAsset; }
    const TArray<FName>& GetPlayerIDs() const { return PlayerIDs; }
    const TArray<FName>& GetNPCIDs() const { return NPCIDs; }
    const TArray<FOffgridAIConversationRecordLine>& GetCanonicalConversationRecord() const { return CanonicalConversationRecord; }
    const TArray<FOffgridAILinePerformanceRequest>& GetPendingNPCLineRequests() const { return PendingNPCLineRequests; }
    FString GetSignificantPersistentEmotionStatePrompt() const;
    void TickEmotionTransitions(float DeltaTimeSeconds);

protected:
    void SetConversationState(EOffgridAIConversationState NewState);
    void SubmitPlayerTurnTextInternal(FName PlayerID, const FText& PlayerText, bool bBroadcastTranscript, const TCHAR* LatencyEventName, const TCHAR* LogLabel);
    void AppendToCanonicalRecord(const FOffgridAIConversationRecordLine& RecordLine);
    void InitializeEmotionTransitionState();
    void InitializeEmotionTransitionStateForNPC(FName NPCID);
    void InitializePADSStateForNPC(FName NPCID);
    FOffgridAIPADSState GetTargetPADSForLineEmotion(FName LineEmotion) const;
    void MovePADSTowardLineEmotionTarget(FName NPCID, FName LineEmotion);
    void RefreshEmotionTargetsFromPADS();
    void RelaxEmotionTargetsForActiveNPCs();
    static float SmoothStep01(float Alpha);
    static float EasedEmotionTransitionSpeed(float ElapsedSeconds, float TargetSpeedPerSecond);
    FString EmotionAdjectiveForPrompt(FName Emotion) const;
    FString TTSEmotionInstructionPhrase(FName Emotion, float Magnitude) const;
    TArray<FName> GetSupportedPerformanceEmotionNamesForNPC(FName NPCID, bool bIncludeNeutral) const;

private:
    bool TryBuildLineRequestsFromJSON(const FString& JSONPayload, TArray<FOffgridAILinePerformanceRequest>& OutLineRequests);
    bool TryBuildSingleLineRequestFromObject(const TSharedPtr<FJsonObject>& EntryObject, int32 LineIndex, TArray<FOffgridAILinePerformanceRequest>& OutLineRequests);
    FName MakeUniqueLineID(int32 LineIndex);

    UPROPERTY()
    TObjectPtr<UOffgridAIOrchestrator> Orchestrator;

    UPROPERTY()
    FGuid ConversationID;

    UPROPERTY()
    EOffgridAIConversationState CurrentConversationState = EOffgridAIConversationState::Inactive;

    UPROPERTY()
    TArray<FName> PlayerIDs;

    UPROPERTY()
    TArray<FName> NPCIDs;

    UPROPERTY()
    TObjectPtr<UOffgridAIConversationPromptDataAsset> ConversationPromptAsset;

    UPROPERTY()
    TArray<FOffgridAILinePerformanceRequest> PendingNPCLineRequests;

    int32 ActiveNPCLineIndex = INDEX_NONE;

    UPROPERTY()
    TArray<FOffgridAIConversationRecordLine> CanonicalConversationRecord;

    struct FEmotionTransitionState
    {
        FName CurrentEmotion = TEXT("neutral");
        FName TargetEmotion = TEXT("neutral");
        FString CurrentLabel = TEXT("neutral");
        FString TargetLabel = TEXT("neutral");
        FString TargetTTS = TEXT("neutral");
        float CurrentMagnitude = 0.0f;
        float TargetMagnitude = 0.0f;
        float TransitionElapsedSeconds = 0.0f;
        bool bHasAssignedTarget = false;
        bool bIsRelaxing = false;
    };

    struct FPADSEmotionMapping
    {
        FString Label = TEXT("neutral");
        FName Family = TEXT("neutral");
        FString TTS = TEXT("neutral");
        FVector4f Center = FVector4f(0.0f, 0.0f, 0.0f, 0.5f);
        float Radius = 1.0f;
        float Magnitude = 0.0f;
    };

    // ConversationManager owns runtime PADS and the resulting expression transition.
    // LineCoach supplies only designer-authored starting PADS values.
    TMap<FName, FOffgridAIPADSState> PADSStateByNPC;
    TMap<FName, FEmotionTransitionState> EmotionTransitionStateByNPC;
    TArray<FPADSEmotionMapping> PADEmotionMappings;

    static constexpr float EmotionApproachSpeedPerSecond = 3.0f;
    static constexpr float EmotionRelaxSpeedPerSecond = 2.4f;
    static constexpr float EmotionTransitionSpeedEaseInSeconds = 0.30f;
    static constexpr float EmotionRelaxTargetMagnitude = 0.20f;
    static constexpr float EmotionMagnitudeEpsilon = 0.01f;
    // Line emotion maps to a scholarly PAD target point. Runtime PADS moves toward
    // that target; Stability damps the amount of movement per classified line.
    static constexpr float PADSResponsivenessAtStabilityZero = 0.45f;
    static constexpr float PADSResponsivenessAtStabilityOne = 0.08f;

    UPROPERTY()
    bool bHasAutoStartedGreeting = false;

    UPROPERTY()
    int32 ConversationLineSerial = 0;
};
