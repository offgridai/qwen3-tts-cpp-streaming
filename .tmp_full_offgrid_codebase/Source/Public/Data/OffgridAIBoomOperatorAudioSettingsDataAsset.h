#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "OffgridAIBoomOperatorAudioSettingsDataAsset.generated.h"

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAIBoomOperatorAudioSettingsDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    /*
     * Capture Device Preferences
     *
     * These values are preferences used when opening the input device.
     * The actual device format may fall back to another supported rate/channel count.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BoomOperator|Capture", meta = (ClampMin = "8000", ClampMax = "192000"))
    int32 PreferredCaptureSampleRate = 16000;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BoomOperator|Capture", meta = (ClampMin = "1", ClampMax = "2"))
    int32 PreferredCaptureNumChannels = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BoomOperator|Capture", meta = (ClampMin = "64", ClampMax = "8192"))
    int32 FramesPerBuffer = 512;

    /*
     * Input Conditioning
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BoomOperator|Input Conditioning", meta = (ClampMin = "0.0", ClampMax = "8.0"))
    float InputGain = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BoomOperator|Input Conditioning")
    bool bEnableHighPassFilter = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BoomOperator|Input Conditioning", meta = (ClampMin = "20.0", ClampMax = "1000.0"))
    float HighPassCutoffHz = 90.0f;

    /*
     * Utterance Finalization
     *
     * StopTailCaptureSeconds keeps capturing briefly after capture stop is requested.
     * FinalizeSilencePaddingSeconds adds trailing silence to the submitted ASR utterance.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BoomOperator|Finalization", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float StopTailCaptureSeconds = 0.15f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BoomOperator|Finalization", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float FinalizeSilencePaddingSeconds = 0.30f;
};
