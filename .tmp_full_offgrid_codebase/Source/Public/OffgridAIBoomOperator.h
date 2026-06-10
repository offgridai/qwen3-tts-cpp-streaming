#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Templates/UniquePtr.h"
#include "Templates/Atomic.h"
#include "AudioCaptureCore.h"
#include "TimerManager.h"
#include "OffgridAIBoomOperator.generated.h"

class UOffgridAIBoomOperatorAudioSettingsDataAsset;
class UOffgridAIOrchestrator;

USTRUCT()
struct FOffgridAIResolvedAudioCaptureSettings
{
    GENERATED_BODY()

    int32 SampleRate = 48000;
    int32 NumChannels = 1;
    int32 FramesPerBuffer = 480;
    float InputGain = 1.0f;
    bool bEnableHighPassFilter = true;
    float HighPassCutoffHz = 90.0f;
    float HighPassAlpha = 0.0f;
    float StopTailCaptureSeconds = 0.07f;
    float FinalizeSilencePaddingSeconds = 0.04f;
};

UCLASS(ClassGroup = (OffgridAI), meta = (BlueprintSpawnableComponent))
class OFFGRIDAI_API UOffgridAIBoomOperator : public UActorComponent
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void PlayerPTTInputStart();

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void PlayerPTTInputEnd();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI")
    FName PlayerID = TEXT("YourPlayerName");

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OffgridAI|Audio")
    TObjectPtr<UOffgridAIBoomOperatorAudioSettingsDataAsset> BoomAudioSettingsAsset = nullptr;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    UOffgridAIOrchestrator* GetOrchestrator() const;
    FOffgridAIResolvedAudioCaptureSettings ResolveAudioCaptureSettings() const;

    bool StartAudioCapture();
    void StopAudioCapture();
    void OnAudioChunkCaptured(const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels);

    void ResetInputDSPState();
    void ProcessCapturedAudioToPCM16(const float* FloatSamples, int32 NumFrames, int32 NumChannels, int32 SampleRate, TArray<uint8>& OutPCMChunk);

    void HandleCaptureDrainTimerFired();
    void FinalizeCaptureIfDrained();
    void SubmitFinalizeSilencePadding();

    TUniquePtr<Audio::FAudioCapture> AudioCapture;
    bool bIsCapturing = false;
    bool bHasLoggedCaptureFormat = false;
    bool bPendingFinalizeAfterDrain = false;
    bool bFinalizeInProgress = false;

    TAtomic<int32> PendingCaptureCallbacks{ 0 };
    FTimerHandle CaptureDrainTimerHandle;

    FOffgridAIResolvedAudioCaptureSettings ActiveCaptureSettings;

    float PreviousMonoInputSample = 0.0f;
    float PreviousMonoOutputSample = 0.0f;
};