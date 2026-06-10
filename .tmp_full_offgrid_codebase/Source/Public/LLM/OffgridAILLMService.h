#pragma once

#include "CoreMinimal.h"
#include "Core/OffgridAITypes.h"
#include "LLM/OffgridAILLMPipeProtocol.h"

class UOffgridAIConversationPromptDataAsset;
class UOffgridAILLMServiceSettingsDataAsset;

struct FOffgridAILLMCompletedRequest
{
    FString RequestID;
    FGuid ConversationID;
    FString JSONPayload;
};

class OFFGRIDAI_API IOffgridAILLMService
{
public:
    virtual ~IOffgridAILLMService() = default;

    virtual void Reset() = 0;
    virtual void Tick(double NowSeconds) = 0;
    virtual bool EnsureServiceReady(FString& OutError) = 0;

    virtual bool InitializeSession(
        const FGuid& ConversationID,
        const TArray<FName>& NPCIDs,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord) = 0;

    virtual bool SubmitRequest(
        const FString& RequestID,
        const FGuid& ConversationID,
        const TArray<FName>& NPCIDs,
        const FText& PlayerText,
        EOffgridAILLMRequestKind RequestKind,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
        const FString& PersistentEmotionState,
        const TArray<FName>& SupportedEmotionNames) = 0;

    virtual void CancelActiveRequest(const FGuid& ConversationID) = 0;
    virtual void ClearSession(const FGuid& ConversationID) = 0;
    virtual bool TryPopCompletedRequest(FOffgridAILLMCompletedRequest& OutCompletedRequest) = 0;
};

class OFFGRIDAI_API FOffgridAILLMStub final : public IOffgridAILLMService
{
public:
    explicit FOffgridAILLMStub(const UOffgridAILLMServiceSettingsDataAsset* InSettings = nullptr);

    virtual void Reset() override;
    virtual void Tick(double NowSeconds) override;
    virtual bool EnsureServiceReady(FString& OutError) override;

    virtual bool InitializeSession(
        const FGuid& ConversationID,
        const TArray<FName>& NPCIDs,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord) override;

    virtual bool SubmitRequest(
        const FString& RequestID,
        const FGuid& ConversationID,
        const TArray<FName>& NPCIDs,
        const FText& PlayerText,
        EOffgridAILLMRequestKind RequestKind,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
        const FString& PersistentEmotionState,
        const TArray<FName>& SupportedEmotionNames) override;

    virtual void CancelActiveRequest(const FGuid& ConversationID) override;
    virtual void ClearSession(const FGuid& ConversationID) override;
    virtual bool TryPopCompletedRequest(FOffgridAILLMCompletedRequest& OutCompletedRequest) override;

private:
    static FString EscapeJSONString(const FString& InString);
    FName ResolveStubEmotion(const UOffgridAIConversationPromptDataAsset* PromptAsset, const TArray<FName>& SupportedEmotionNames) const;
    float GetCompletionDelaySeconds() const;

    struct FPendingRequest
    {
        bool bActive = false;
        FString RequestID;
        FGuid ConversationID;
        double CompleteAtSeconds = 0.0;
        FString JSONPayload;
    };

    const UOffgridAILLMServiceSettingsDataAsset* Settings = nullptr;
    TSet<FGuid> InitializedSessions;
    mutable int32 StubEmotionIndex = 0;
    double LastTickNowSeconds = 0.0;
    FPendingRequest PendingRequest;
};
