#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "OffgridAILineCoachSettingsDataAsset.generated.h"

class UOffgridAILineCoachAudioSettingsDataAsset;
class UOffgridAILipsyncSettingsDataAsset;
class UOffgridAIEmotionSettingsDataAsset;

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAILineCoachSettingsDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    /*
     * Feature Gates
     *
     * LineCoach is the per-NPC presentation coordinator.
     * These gates enable or disable each presentation channel independently.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Features")
    bool bEnableAudioPlayback = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Features")
    bool bEnableFacialEmotion = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Features")
    bool bEnableFacialLipsync = true;

    /*
     * Referenced Settings
     *
     * LineCoach does not own audio, emotion, or lipsync policy directly.
     * It references the settings assets for each channel it coordinates.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Settings")
    TObjectPtr<UOffgridAILineCoachAudioSettingsDataAsset> AudioSettings;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Settings")
    TObjectPtr<UOffgridAIEmotionSettingsDataAsset> EmotionSettings;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Settings")
    TObjectPtr<UOffgridAILipsyncSettingsDataAsset> LipsyncSettings;
};
