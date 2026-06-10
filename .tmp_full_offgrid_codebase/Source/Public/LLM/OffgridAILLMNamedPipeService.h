#pragma once

#include "CoreMinimal.h"
#include "LLM/OffgridAILLMService.h"
#include "LLM/OffgridAILLMPipeProtocol.h"
#include "Templates/UniquePtr.h"

class FOffgridAILLMPipeClient;
class UOffgridAILLMServiceSettingsDataAsset;

class FOffgridAILLMNamedPipeService final : public IOffgridAILLMService
{
public:
    explicit FOffgridAILLMNamedPipeService(const UOffgridAILLMServiceSettingsDataAsset* InSettings);
    virtual ~FOffgridAILLMNamedPipeService() override;

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
    bool EnsureServiceConnected();
    bool StartupServiceIfNeeded(FString* OutFatalError = nullptr);
    bool ValidateStartupSettings(FString& OutError) const;
    bool SendRequest(const FOffgridAILLMRequest& Request, FOffgridAILLMResponse* OutResponse = nullptr, bool bExpectResponse = true);
    void PopulateCommonRequestFields(FOffgridAILLMRequest& OutRequest) const;
    void PopulatePromptFields(
        FOffgridAILLMRequest& OutRequest,
        const TArray<FName>& NPCIDs,
        const UOffgridAIConversationPromptDataAsset* PromptAsset,
        const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
        const TArray<FName>& SupportedEmotionNames) const;
    FString BuildRecentTurnTranscript(const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord, int32 MaxRecentTurns) const;
    FString BuildLaunchArguments() const;

private:
    const UOffgridAILLMServiceSettingsDataAsset* Settings = nullptr;
    TUniquePtr<FOffgridAILLMPipeClient> PipeClient;
    FProcHandle ServiceProcessHandle;
    uint32 ServiceProcessId = 0;
    bool bStartupCompleted = false;
    TQueue<FOffgridAILLMCompletedRequest> CompletedQueue;
};
