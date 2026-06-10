#include "LLM/OffgridAILLMService.h"

#include "Data/OffgridAIConversationPromptDataAsset.h"
#include "Data/OffgridAILLMServiceSettingsDataAsset.h"

namespace
{
    constexpr double DefaultCompletionDelaySeconds = 0.02;
}

FOffgridAILLMStub::FOffgridAILLMStub(const UOffgridAILLMServiceSettingsDataAsset* InSettings)
    : Settings(InSettings)
{
}

void FOffgridAILLMStub::Reset()
{
    InitializedSessions.Empty();
    PendingRequest = FPendingRequest();
}

void FOffgridAILLMStub::Tick(double NowSeconds)
{
    LastTickNowSeconds = NowSeconds;
}

bool FOffgridAILLMStub::EnsureServiceReady(FString& OutError)
{
    OutError.Reset();
    return true;
}

bool FOffgridAILLMStub::InitializeSession(
    const FGuid& ConversationID,
    const TArray<FName>& NPCIDs,
    const UOffgridAIConversationPromptDataAsset* PromptAsset,
    const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord)
{
    if (!ConversationID.IsValid() || NPCIDs.IsEmpty())
    {
        return false;
    }

    InitializedSessions.Add(ConversationID);
    return true;
}

bool FOffgridAILLMStub::SubmitRequest(
    const FString& RequestID,
    const FGuid& ConversationID,
    const TArray<FName>& NPCIDs,
    const FText& PlayerText,
    EOffgridAILLMRequestKind RequestKind,
    const UOffgridAIConversationPromptDataAsset* PromptAsset,
    const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
    const FString& PersistentEmotionState,
    const TArray<FName>& SupportedEmotionNames)
{
    if (NPCIDs.IsEmpty() || !InitializedSessions.Contains(ConversationID))
    {
        return false;
    }

    PendingRequest.bActive = true;
    PendingRequest.RequestID = RequestID;
    PendingRequest.ConversationID = ConversationID;
    PendingRequest.CompleteAtSeconds = LastTickNowSeconds + static_cast<double>(GetCompletionDelaySeconds());

    if (RequestKind == EOffgridAILLMRequestKind::EmotionImpactClassifier)
    {
        if (NPCIDs.Num() == 1)
        {
            PendingRequest.JSONPayload = TEXT("neutral");
        }
        else
        {
            TArray<FString> Lines;
            for (const FName& NPCID : NPCIDs)
            {
                Lines.Add(FString::Printf(TEXT("%s=neutral"), *NPCID.ToString()));
            }
            PendingRequest.JSONPayload = FString::Join(Lines, TEXT("\n"));
        }
        return true;
    }

    const FName Emotion = ResolveStubEmotion(PromptAsset, SupportedEmotionNames);
    const FString EscapedPlayerText = EscapeJSONString(PlayerText.ToString());
    PendingRequest.JSONPayload = FString::Printf(
        TEXT("{\"emotion\":\"%s\",\"dialogue\":\"I heard you say: %s\"}"),
        *Emotion.ToString(),
        *EscapedPlayerText);
    return true;
}


void FOffgridAILLMStub::CancelActiveRequest(const FGuid& ConversationID)
{
    if (PendingRequest.bActive && PendingRequest.ConversationID == ConversationID)
    {
        PendingRequest = FPendingRequest();
    }
}

void FOffgridAILLMStub::ClearSession(const FGuid& ConversationID)
{
    CancelActiveRequest(ConversationID);
    InitializedSessions.Remove(ConversationID);
}

bool FOffgridAILLMStub::TryPopCompletedRequest(FOffgridAILLMCompletedRequest& OutCompletedRequest)
{
    if (!PendingRequest.bActive || LastTickNowSeconds < PendingRequest.CompleteAtSeconds)
    {
        return false;
    }

    if (!InitializedSessions.Contains(PendingRequest.ConversationID))
    {
        PendingRequest = FPendingRequest();
        return false;
    }

    OutCompletedRequest.RequestID = PendingRequest.RequestID;
    OutCompletedRequest.ConversationID = PendingRequest.ConversationID;
    OutCompletedRequest.JSONPayload = PendingRequest.JSONPayload;
    PendingRequest = FPendingRequest();
    return true;
}

FString FOffgridAILLMStub::EscapeJSONString(const FString& InString)
{
    FString Escaped = InString;
    Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
    Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
    Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
    Escaped.ReplaceInline(TEXT("\r"), TEXT(""));
    return Escaped;
}

FName FOffgridAILLMStub::ResolveStubEmotion(const UOffgridAIConversationPromptDataAsset* PromptAsset, const TArray<FName>& SupportedEmotionNames) const
{
    static const TArray<FName> DefaultCandidateEmotions =
    {
        TEXT("neutral"),
        TEXT("joy"),
        TEXT("anger"),
        TEXT("sadness"),
        TEXT("fear"),
        TEXT("surprise"),
        TEXT("disgust")
    };

    const TArray<FName>& CandidateEmotions = SupportedEmotionNames.Num() > 0 ? SupportedEmotionNames : DefaultCandidateEmotions;
    const FName Result = CandidateEmotions[StubEmotionIndex % CandidateEmotions.Num()];
    ++StubEmotionIndex;
    return Result;
}

float FOffgridAILLMStub::GetCompletionDelaySeconds() const
{
    return (Settings && Settings->StubCompletionDelaySeconds > 0.0f)
        ? Settings->StubCompletionDelaySeconds
        : static_cast<float>(DefaultCompletionDelaySeconds);
}
