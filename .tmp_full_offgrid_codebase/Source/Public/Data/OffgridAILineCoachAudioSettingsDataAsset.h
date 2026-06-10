#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "OffgridAILineCoachAudioSettingsDataAsset.generated.h"

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAILineCoachAudioSettingsDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    /*
     * Source
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Audio|Source")
    FVector AudioSourceOffset = FVector::ZeroVector;

    /*
     * Playback
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Audio|Playback")
    bool bEnableSpatialization = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Audio|Playback", meta = (ClampMin = "0.0"))
    float PlaybackVolumeMultiplier = 2.0f;

    /*
     * Streaming Buffer
     *
     * InitialPrerollSeconds is intentionally owned by audio playback because it
     * controls when streamed speech becomes audible. Lipsync may use the same
     * elapsed audio stream as timing evidence, but it does not own playback start.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Audio|Streaming", meta = (ClampMin = "0.0"))
    float InitialPrerollSeconds = 0.50f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Audio|Streaming", meta = (ClampMin = "0.0"))
    float MaintainBufferedAudioFloorSeconds = 0.05f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Audio|Streaming", meta = (ClampMin = "0.0"))
    float CoalescedWriteSeconds = 0.02f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Audio|Streaming", meta = (ClampMin = "0.0"))
    float MaxWriteBurstSeconds = 0.30f;

    /*
     * Completion
     *
     * Extra hold after the procedural queue drains. This avoids ending the line
     * before Unreal's downstream audio renderer has audibly finished the final samples.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LineCoach|Audio|Completion", meta = (ClampMin = "0.0", ClampMax = "0.5"))
    float PlaybackPostDrainHoldSeconds = 0.18f;
};
