#pragma once

#include "CoreMinimal.h"
#include "Core/OffgridAITypes.h"
#include "LLM/OffgridAILLMPipeProtocol.h"

class UOffgridAIConversationPromptDataAsset;
class UOffgridAIOrchestrator;

/*
Single boundary between the orchestrator and all service work.

The orchestrator should not care whether ASR / LLM / TTS are stub or real, or
how startup / heartbeat / retry policy is enforced. It asks for services here
and the implementation below owns the rest.
*/
class OFFGRIDAI_API IOffgridAIServiceGateway
{
public:
    virtual ~IOffgridAIServiceGateway();

    virtual void Initialize(UOffgridAIOrchestrator* InOrchestrator) = 0;
    virtual void Shutdown() = 0;
    virtual void StartupServices() = 0;
    virtual void Tick(float DeltaTimeSeconds) = 0;

    virtual bool AreRequiredServicesReady() const = 0;
    virtual FOffgridAIServiceStatus GetServiceStatus(EOffgridAIServiceKind ServiceKind) const = 0;

    virtual bool BeginPlayerAudioInput(const FGuid& ConversationID, FName PlayerID) = 0;
    virtual void SubmitPlayerAudioChunk(const FGuid& ConversationID, FName PlayerID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels) = 0;
    virtual void EndPlayerAudioInput(const FGuid& ConversationID, FName PlayerID) = 0;

    virtual bool InitializeLLMSession(
        const FGuid& ConversationID,
        const TArray<FName>& NPCIDs,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord) = 0;

    virtual bool SubmitLLMRequest(
        const FGuid& ConversationID,
        const TArray<FName>& NPCIDs,
        const FText& PlayerText,
        EOffgridAILLMRequestKind RequestKind,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
        const FString& PersistentEmotionState,
        const TArray<FName>& SupportedEmotionNames) = 0;

    virtual void CancelLLMRequest(const FGuid& ConversationID) = 0;
    virtual void ClearLLMSession(const FGuid& ConversationID) = 0;

    virtual bool BeginTTSRequest(const FOffgridAILinePerformanceRequest& LineRequest) = 0;
    virtual void CancelTTS(const FGuid& ConversationID, FName NPCID, FName LineID) = 0;
};
