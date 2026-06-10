#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "OffgridAILipsyncSettingsDataAsset.generated.h"

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAILipsyncSettingsDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    /*
     * Runtime Audio Timing Assistant
     *
     * Audio may adjust timing, but it must not change viseme identity, insert or
     * remove visemes, or reorder the text-authored plan.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Runtime Audio Timing")
    bool bEnableRuntimeAudioTimingAssistant = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Runtime Audio Timing", meta = (ClampMin = "0.0", ClampMax = "0.10"))
    float RuntimeAudioTimingAssistantMaxShiftSeconds = 0.040f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Runtime Audio Timing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RuntimeAudioTimingAssistantBlendWeight = 0.35f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Runtime Audio Timing", meta = (ClampMin = "0.0", ClampMax = "1.5"))
    float RuntimeAudioStressStrengthScale = 0.12f;

    /*
     * Text Plan
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Text Plan", meta = (ClampMin = "6.0", ClampMax = "28.0"))
    float EstimatedCharactersPerSecond = 14.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Text Plan", meta = (ClampMin = "0.1", ClampMax = "5.0"))
    float MinTextPlanDurationSeconds = 0.45f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Text Plan", meta = (ClampMin = "0.0", ClampMax = "0.15"))
    float TextPlanLookaheadSeconds = 0.006f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Text Plan", meta = (ClampMin = "0.0", ClampMax = "0.2"))
    float TextPlanCoarticulationSeconds = 0.075f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Text Plan", meta = (ClampMin = "0.0", ClampMax = "0.15"))
    float TextPlanAnticipationSeconds = 0.004f;

    /*
     * Pose Shape Timing
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Pose Timing", meta = (ClampMin = "1.0", ClampMax = "100.0"))
    float TextPlanMBPAttackMs = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Pose Timing", meta = (ClampMin = "1.0", ClampMax = "150.0"))
    float TextPlanMBPReleaseMs = 48.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Pose Timing", meta = (ClampMin = "1.0", ClampMax = "200.0"))
    float TextPlanVowelAttackMs = 28.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Pose Timing", meta = (ClampMin = "1.0", ClampMax = "250.0"))
    float TextPlanVowelReleaseMs = 80.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Pose Timing", meta = (ClampMin = "1.0", ClampMax = "200.0"))
    float TextPlanRoundAttackMs = 24.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Pose Timing", meta = (ClampMin = "1.0", ClampMax = "250.0"))
    float TextPlanRoundReleaseMs = 70.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Pose Timing", meta = (ClampMin = "1.0", ClampMax = "150.0"))
    float TextPlanAccentAttackMs = 12.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Pose Timing", meta = (ClampMin = "1.0", ClampMax = "180.0"))
    float TextPlanAccentReleaseMs = 52.0f;

    /*
     * Speech Onset Detection
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Speech Onset", meta = (ClampMin = "0.0001", ClampMax = "0.05"))
    float TextPlanSpeechOnsetRMSThreshold = 0.014f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Speech Onset", meta = (ClampMin = "0.0", ClampMax = "0.5"))
    float TextPlanInitialNoiseIgnoreSeconds = 0.16f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Speech Onset", meta = (ClampMin = "0.0", ClampMax = "0.25"))
    float TextPlanSpeechOnsetSustainSeconds = 0.070f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Speech Onset", meta = (ClampMin = "1.0", ClampMax = "6.0"))
    float TextPlanNoiseGateMultiplier = 2.60f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Speech Onset", meta = (ClampMin = "1.0", ClampMax = "8.0"))
    float TextPlanInitialStrongSpeechMultiplier = 3.0f;

    /*
     * Energy Envelope
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Energy Envelope", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TextPlanSpeakingEnvelopeFloor = 0.68f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Energy Envelope", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TextPlanEnergyModulationDepth = 0.22f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Energy Envelope", meta = (ClampMin = "1.0", ClampMax = "250.0"))
    float TextPlanEnergyAttackMs = 12.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Energy Envelope", meta = (ClampMin = "1.0", ClampMax = "500.0"))
    float TextPlanEnergyReleaseMs = 85.0f;

    /*
     * Output
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Output", meta = (ClampMin = "0.25", ClampMax = "2.0"))
    float TextPlanOutputStrengthScale = 1.00f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Output", meta = (ClampMin = "0.0", ClampMax = "0.3"))
    float TextPlanRestClosedWeight = 0.030f;

    /*
     * Debug
     *
     * Enables per-frame lipsync CSV logging and per-line input WAV dumps under
     * Saved/Logs. Keep disabled during normal demo/runtime use.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Debug")
    bool bEnableLipsyncDebugLogging = false;
};
