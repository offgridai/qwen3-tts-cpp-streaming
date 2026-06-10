#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "OffgridAIConversationPromptDataAsset.generated.h"

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAIConversationPromptDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    // Dialogue line-writer prompt. This is the creative prompt used for the fast
    // unconstrained / lightly constrained call that writes speaker-prefixed NPC lines.
    // Do not ask this pass to output emotion. Runtime emotion is assigned by the
    // separate EmotionPrompt classifier after dialogue text is generated.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Dialogue", meta=(MultiLine=true))
    FText SystemPrompt;

    // Optional GBNF for the dialogue section. Leave disabled for the fastest, most expressive
    // line-writer behavior. Enable only if you want to constrain speaker-line formatting.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Dialogue")
    bool bUseDialogueGBNF = false;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Dialogue", meta=(MultiLine=true, EditCondition="bUseDialogueGBNF"))
    FText DialogueGBNF;

    // Hard cap for the fast dialogue line-writer call. Keep this low for real-time NPC response.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Dialogue", meta=(ClampMin="1", ClampMax="256"))
    int32 MaxDialogueTokens = 32;

    // Player-line emotional impulse classifier prompt. Before dialogue is written,
    // the LLM service asks which base emotional impulse the latest player line should
    // cause in each NPC. ConversationManager maps that label into runtime PADS.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Emotion", meta=(MultiLine=true))
    FText EmotionPrompt = FText::FromString(
        TEXT("Given the latest player line and recent context, choose the emotional impulse it should cause in each NPC:\n")
        TEXT("joy, anger, sadness, fear, surprise, disgust, neutral\n")
        TEXT("respond with one word only"));

    // Number of recent canonical conversation turns to include in the emotion-classifier prompt.
    // Keep this small to preserve latency; 0 disables transcript context for emotion labeling.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Emotion", meta=(ClampMin="0", ClampMax="20"))
    int32 EmotionContextTurnCount = 10;

    // Hard character cap for recent emotion context after turn-count selection. This prevents
    // long transcripts from expanding the tiny classifier prompt and damaging latency.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Emotion", meta=(ClampMin="0", ClampMax="8000"))
    int32 MaxEmotionContextCharacters = 1200;

    // Optional GBNF for the emotion section. Disabled by default for speed; the service still
    // validates the label and falls back safely if the classifier returns an invalid value.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Emotion")
    bool bUseEmotionGBNF = false;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Emotion", meta=(MultiLine=true, EditCondition="bUseEmotionGBNF"))
    FText EmotionGBNF = FText::FromString(
        TEXT("root ::= item | item \"\\n\" item | item \"\\n\" item \"\\n\" item | item \"\\n\" item \"\\n\" item \"\\n\" item\n")
        TEXT("item ::= [1-4] \"=\" emotion\n")
        TEXT("emotion ::= \"neutral\" | \"joy\" | \"anger\" | \"sadness\" | \"fear\" | \"surprise\" | \"disgust\""));

    // Persistent emotion memory is owned and updated only by ConversationManager, per NPC.
    // Runtime registers initialize to zero for each supported non-neutral emotion.
    // Runtime update rule: chosen line emotion rises one step; every other tracked
    // emotion for that NPC decays one step toward zero.
    // Persistent emotion is passed to LineCoach as per-line expression magnitude; LineCoach does not own emotional state.
    // InitialPersistentEmotionWeights is retained for asset compatibility but is ignored by the current runtime.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Persistent Emotion", meta=(ClampMin="0.0", ClampMax="1.0"))
    TMap<FName, float> InitialPersistentEmotionWeights;

    // Legacy tuning fields retained for asset compatibility. The sparse ordinal runtime
    // currently uses fixed one-step up / one-step down transitions rather than float math.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Persistent Emotion", meta=(ClampMin="0.0", ClampMax="1.0"))
    float PersistentEmotionIncrementPerSpokenLine = 0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Persistent Emotion", meta=(ClampMin="0.0", ClampMax="1.0"))
    float PersistentEmotionTurnDecay = 0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Persistent Emotion", meta=(ClampMin="0.0", ClampMax="1.0"))
    float PersistentEmotionSignificantThreshold = 0.50f;

    // Number of recent player turns to replay verbatim in the dialogue prompt.
    // A turn begins with a player line and includes every NPC line that follows before
    // the next player line. The current player line is already in the canonical record
    // before the LLM request is built, so it counts as one of these turns.
    // Set to 0 to send no transcript history.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Dialogue", meta=(ClampMin="0", ClampMax="64"))
    int32 RecentDialogueTurnCount = 16;


    // Canonical response schema. The LLM service packages this in C++; the model should not
    // be asked to author the final JSON payload in the high-speed path.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Schema", meta=(MultiLine=true))
    FText ResponseSchemaJson;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prompt|Schema", meta=(MultiLine=true))
    FText ExampleJson;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Auto Start")
    bool bAutoStartNPCGreeting = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Auto Start", meta=(MultiLine=true, EditCondition="bAutoStartNPCGreeting"))
    FText AutoStartHiddenPlayerTurn = FText::FromString(TEXT("The player has just approached the counter. Greet them in character with one short sentence."));
};
