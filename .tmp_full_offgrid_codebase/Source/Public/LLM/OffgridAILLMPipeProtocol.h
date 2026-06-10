#pragma once

#include "CoreMinimal.h"
#include "Data/OffgridAILLMServiceSettingsDataAsset.h"

enum class EOffgridAILLMOp : uint8
{
    Startup,
    InitializeSession,
    Generate,
    Cancel,
    ClearSession,
    Health,
    Shutdown
};

enum class EOffgridAILLMRequestKind : uint8
{
    Dialogue,
    EmotionImpactClassifier
};

struct FOffgridAILLMRequest
{
    EOffgridAILLMOp Op = EOffgridAILLMOp::Health;
    EOffgridAILLMRequestKind RequestKind = EOffgridAILLMRequestKind::Dialogue;
    FString RequestId;
    FString ConversationId;
    TArray<FString> NPCIds;
    FString PlayerText;
    FString SystemPrompt;
    FString EmotionPrompt;
    FString ResponseSchemaJson;
    FString ExampleJson;
    FString CanonicalTranscript;
    FString PersistentEmotionState;
    // Runtime contract for the dialogue line-writer pass. Emotion is intentionally excluded;
    // line performance emotion is assigned only by the separate emotion-classifier pass.
    FString DialogueOutputContract = TEXT("NPC|spoken line");
    TArray<FString> AllowedEmotionLabels;
    bool bUseDialogueGBNF = false;
    FString DialogueGBNF;
    bool bUseEmotionGBNF = false;
    FString EmotionGBNF;
    int32 MaxDialogueTokens = 32;
    int32 EmotionContextTurnCount = 10;
    int32 MaxEmotionContextCharacters = 1200;
    float EmotionStateStep = 0.1f;

    EOffgridAILLMMode Mode = EOffgridAILLMMode::Passthrough;
    FString ModelPath;
    int32 ContextWindowTokens = 4096;
    int32 MaxOutputTokens = 256;
    float Temperature = 0.2f;
    int32 MaxDialogueCharacters = 500;
    int32 GPULayers = 999;
    int32 ParallelSlots = 1;
    bool bVerboseBackendLogging = false;
};

struct FOffgridAILLMResponse
{
    bool bOk = false;
    FString RequestId;
    FString JSONPayload;
    FString ErrorMessage;
};

namespace OffgridAILLMProtocol
{
    OFFGRIDAI_API FString OpToString(EOffgridAILLMOp Op);
    OFFGRIDAI_API bool StringToOp(const FString& Value, EOffgridAILLMOp& OutOp);
    OFFGRIDAI_API bool SerializeRequest(const FOffgridAILLMRequest& Request, TArray<uint8>& OutBytes);
    OFFGRIDAI_API bool DeserializeRequest(const TArray<uint8>& Bytes, FOffgridAILLMRequest& OutRequest);
    OFFGRIDAI_API bool SerializeResponse(const FOffgridAILLMResponse& Response, TArray<uint8>& OutBytes);
    OFFGRIDAI_API bool DeserializeResponse(const TArray<uint8>& Bytes, FOffgridAILLMResponse& OutResponse);
}
