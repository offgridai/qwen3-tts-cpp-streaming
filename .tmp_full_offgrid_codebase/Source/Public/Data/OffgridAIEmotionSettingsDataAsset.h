#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "OffgridAIEmotionSettingsDataAsset.generated.h"

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIEmotionPADTarget
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float Pleasure = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float Activation = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float Dominance = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Stability = 0.5f;
};

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIEmotionLabelSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion")
    FName Label = TEXT("neutral");

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion")
    FName Family = TEXT("neutral");

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion")
    FString TTSStyle = TEXT("neutral");

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion")
    FOffgridAIEmotionPADTarget PADTarget;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float Radius = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Magnitude = 0.0f;
};

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAIEmotionSettingsDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    /*
     * Classifier
     *
     * These control emotion-classifier prompt context. They belong here rather
     * than on LLM service settings because they describe emotion behavior, not
     * model/backend runtime behavior.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Classifier", meta = (ClampMin = "0", ClampMax = "20"))
    int32 EmotionContextLineCount = 10;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Classifier", meta = (ClampMin = "0", ClampMax = "8000"))
    int32 MaxEmotionContextCharacters = 1200;

    /*
     * Vocabulary
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Vocabulary")
    TArray<FName> SupportedEmotions =
    {
        TEXT("neutral"),
        TEXT("joy"),
        TEXT("anger"),
        TEXT("sadness"),
        TEXT("fear"),
        TEXT("surprise"),
        TEXT("disgust")
    };

    /*
     * PAD / Label Tuning
     *
     * Empty by default so existing assets are not forced to migrate a large table.
     * Runtime should fall back to built-in defaults when no explicit label setting
     * is found.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|PAD")
    TArray<FOffgridAIEmotionLabelSettings> EmotionLabels;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|PAD", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ResponsivenessAtStabilityZero = 0.45f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|PAD", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ResponsivenessAtStabilityOne = 0.08f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|PAD", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MinimumImpulseWeight = 0.05f;

    /*
     * Presentation Blend Defaults
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Presentation", meta = (ClampMin = "0.01", ClampMax = "5.0"))
    float EmotionBlendInSeconds = 0.12f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Presentation", meta = (ClampMin = "0.01", ClampMax = "10.0"))
    float EmotionFamilyTransitionBlendSeconds = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Presentation", meta = (ClampMin = "0.01", ClampMax = "5.0"))
    float EmotionBlendOutSeconds = 0.30f;

    /*
     * Speech-Time Mouth Suppression
     *
     * These values were previously stored on LineCoach audio settings. They are
     * emotion/face presentation policy, not audio playback policy.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression")
    bool bOverrideFaceDriverEmotionMouthAllowance = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EmotionSpeechControlScaleDuringSpeech = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EmotionMouthCornerScaleDuringSpeech = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EmotionMouthCornerScaleDuringBilabial = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EmotionSharedMouthScaleDuringSpeech = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression", meta = (ClampMin = "0.0", ClampMax = "10.0"))
    float EmotionSpeechHoldSeconds = 1.5f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression", meta = (ClampMin = "0.001", ClampMax = "10.0"))
    float EmotionMouthFadeInSeconds = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression", meta = (ClampMin = "0.001", ClampMax = "10.0"))
    float EmotionMouthFadeOutSeconds = 0.01f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emotion|Speech Mouth Suppression", meta = (ClampMin = "0.01", ClampMax = "10.0"))
    float EmotionFullMouthAfterSilenceSeconds = 1.5f;
};
