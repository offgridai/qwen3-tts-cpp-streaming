#pragma once

#include "CoreMinimal.h"
#include "OffgridAIMetaHumanFaceDriverComponent.h"

class FJsonValue;
class FJsonObject;

struct OFFGRIDAI_API FOffgridAIEmotionSpeechModifiers
{
    float JawOpenBaseline = 0.0f;
    float JawOpenScale = 1.0f;
    float TeethShowScale = 1.0f;
    float LipCloseScale = 1.0f;
    float LipRoundScale = 1.0f;
    float LipWideScale = 1.0f;
    float UpperLipRaiseBias = 0.0f;
    float LowerLipDepressBias = 0.0f;
    float MouthCornerBias = 0.0f;
    float MouthTightenBias = 0.0f;
    float JawForwardBias = 0.0f;
    float JawClenchBias = 0.0f;
    float AttackSpeedScale = 1.0f;
    float ReleaseSpeedScale = 1.0f;

    // Authored lower-face expression allowance while lipsync is active.
    // These are sampled from MetaHumanEmotionLibrary.json alongside each emotion stage,
    // so C++ owns the arbitration mechanism but the expression asset owns the amount.
    float SpeechCriticalMouthExpressionScale = 0.04f;
    float SharedMouthExpressionScale = 0.08f;
    float MouthCornerExpressionScale = 0.18f;
    float MouthCornerBilabialExpressionScale = 0.04f;
};

/**
 * Runtime emotional-expression resolver.
 *
 * This is intentionally parallel to the lipsync utilities: LineCoach/FaceDriver submit an
 * abstract emotion family + magnitude in 0..1, and this utility resolves the authored
 * MetaHuman expression library into face-driver control targets. It does not publish to
 * the rig and it does not arbitrate against lipsync; FaceDriver owns final composition.
 */
class OFFGRIDAI_API FOffgridAIEmotionExpression
{
public:
    bool LoadLibrary(const FString& AbsolutePath);

    void SetEnabled(bool bInEnabled);
    bool IsEnabled() const { return bEnabled; }

    void SubmitEmotion(FName EmotionName, float Magnitude, float OverallWeight, float FamilyTransitionBlendSeconds, bool bLogDiagnostics);
    void ClearEmotion(bool bLogDiagnostics);
    void ForceNeutral();

    void Tick(float DeltaTimeSeconds, float BlendInSeconds, float BlendOutSeconds, float FamilyTransitionBlendSeconds);

    void ResolvePoseSamples(TArray<FOffgridAIFacePoseSample>& OutSamples, bool bLogDiagnostics) const;
    void BuildControlTargets(const TArray<FOffgridAIFacePoseSample>& Samples, float Strength, TMap<FName, FOffgridAIFaceDriverControlValue>& OutControls) const;
    FOffgridAIEmotionSpeechModifiers ResolveSpeechModifiers(bool bLogDiagnostics) const;
    void AppendNeutralControls(TMap<FName, FOffgridAIFaceDriverControlValue>& InOutZeros) const;

    FName GetTargetEmotionName() const { return TargetEmotionName; }
    float GetTargetMagnitude() const { return TargetEmotionMagnitude; }
    float GetCurrentMagnitude() const { return CurrentEmotionMagnitude; }
    float GetTargetOverallWeight() const { return TargetEmotionOverallWeight; }
    float GetCurrentOverallWeight() const { return CurrentEmotionOverallWeight; }
    int32 GetFamilyCount() const { return EmotionBandsByFamily.Num(); }
    int32 GetCompoundCount() const { return CompoundEmotionPoses.Num(); }
    bool IsFamilyTransitionActive() const { return EmotionFamilyTransitionSecondsRemaining > 0.0f; }

    static FName NormalizeEmotionFamily(FName EmotionName);

private:
    struct FRuntimePose
    {
        FName PoseID = NAME_None;
        TArray<FOffgridAIFaceDriverControlValue> Controls;
        FOffgridAIEmotionSpeechModifiers SpeechModifiers;
    };

    struct FEmotionBand
    {
        FName Family = NAME_None;
        FName PoseID = NAME_None;
        float Min = 0.0f;
        float Peak = 0.0f;
        float Max = 1.0f;
        TArray<FOffgridAIFaceDriverControlValue> Controls;
        FOffgridAIEmotionSpeechModifiers SpeechModifiers;
    };

    bool ParseControlsArray(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FOffgridAIFaceDriverControlValue>& OutControls) const;
    FOffgridAIEmotionSpeechModifiers ParseSpeechModifiers(const TSharedPtr<FJsonObject>& Obj) const;
    void AddWeightedControls(const TArray<FOffgridAIFaceDriverControlValue>& Controls, float Weight, TMap<FName, FOffgridAIFaceDriverControlValue>& InOutValues) const;
    void ResolvePoseSamplesForEmotion(FName EmotionName, float Magnitude, float OverallWeight, TArray<FOffgridAIFacePoseSample>& OutSamples) const;
    const FEmotionBand* FindBandByPoseID(FName PoseID) const;

    TMap<FName, TArray<FEmotionBand>> EmotionBandsByFamily;
    TMap<FName, FRuntimePose> CompoundEmotionPoses;

    bool bEnabled = true;
    FName TargetEmotionName = NAME_None;
    float TargetEmotionMagnitude = 0.0f;
    float CurrentEmotionMagnitude = 0.0f;
    float TargetEmotionOverallWeight = 0.0f;
    float CurrentEmotionOverallWeight = 0.0f;

    // Snapshot used for true cross-family blending. Magnitude interpolation alone
    // is insufficient because changing family swaps the authored control set; keep
    // the old family alive while the new family blends in.
    FName TransitionFromEmotionName = NAME_None;
    float TransitionFromMagnitude = 0.0f;
    float TransitionFromOverallWeight = 0.0f;
    float EmotionFamilyTransitionElapsedSeconds = 0.0f;
    float EmotionFamilyTransitionDurationSeconds = 0.0f;
    float EmotionFamilyTransitionSecondsRemaining = 0.0f;
};
