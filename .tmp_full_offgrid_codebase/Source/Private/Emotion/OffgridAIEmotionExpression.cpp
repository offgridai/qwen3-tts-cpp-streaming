#include "Emotion/OffgridAIEmotionExpression.h"

#include "OffgridAI.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    static float Clamp01(float V) { return FMath::Clamp(V, 0.0f, 1.0f); }

    static FString ReadStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FString& Fallback = FString())
    {
        FString Out;
        return Obj.IsValid() && Obj->TryGetStringField(Field, Out) ? Out : Fallback;
    }

    static float ReadNumberField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, float Fallback = 0.0f)
    {
        double Out = 0.0;
        return Obj.IsValid() && Obj->TryGetNumberField(Field, Out) ? static_cast<float>(Out) : Fallback;
    }

    static float TriangularMembership(float X, float Min, float Peak, float Max)
    {
        X = Clamp01(X);
        Min = Clamp01(Min);
        Peak = Clamp01(Peak);
        Max = Clamp01(Max);

        if (X < Min || X > Max)
        {
            return 0.0f;
        }
        if (FMath::IsNearlyEqual(Min, Max))
        {
            return FMath::IsNearlyEqual(X, Peak) ? 1.0f : 0.0f;
        }
        if (FMath::IsNearlyZero(Min) && X <= Peak)
        {
            return 1.0f;
        }
        if (FMath::IsNearlyEqual(Max, 1.0f) && X >= Peak)
        {
            return 1.0f;
        }
        if (FMath::IsNearlyEqual(X, Peak))
        {
            return 1.0f;
        }
        if (X < Peak)
        {
            return FMath::IsNearlyEqual(Peak, Min) ? 1.0f : Clamp01((X - Min) / (Peak - Min));
        }
        return FMath::IsNearlyEqual(Max, Peak) ? 1.0f : Clamp01((Max - X) / (Max - Peak));
    }

    static FOffgridAIFacePoseSample MakePoseSample(FName PoseID, float Weight)
    {
        FOffgridAIFacePoseSample Sample;
        Sample.PoseID = PoseID;
        Sample.Weight = Weight;
        return Sample;
    }

    static void AccumulateControl(FOffgridAIFaceDriverControlValue& Existing, const FOffgridAIFaceDriverControlValue& Incoming, float Weight)
    {
        Existing.ControlName = Incoming.ControlName;
        Existing.bIsVector2D = Incoming.bIsVector2D;
        if (Incoming.bIsVector2D)
        {
            Existing.Vector2DValue += Incoming.Vector2DValue * Weight;
        }
        else
        {
            Existing.FloatValue += Incoming.FloatValue * Weight;
        }
    }

    static FOffgridAIFaceDriverControlValue MakeWeightedControlValue(const FOffgridAIFaceDriverControlValue& Incoming, float Weight)
    {
        FOffgridAIFaceDriverControlValue Out = Incoming;
        if (Incoming.bIsVector2D)
        {
            Out.Vector2DValue = Incoming.Vector2DValue * Weight;
            Out.FloatValue = 0.0f;
        }
        else
        {
            Out.FloatValue = Incoming.FloatValue * Weight;
            Out.Vector2DValue = FVector2D::ZeroVector;
        }
        return Out;
    }

    static float StepBlend(float Current, float Target, float DeltaTime, float InSeconds, float OutSeconds)
    {
        const float Tau = FMath::Max(Target > Current ? InSeconds : OutSeconds, 0.001f);
        const float Alpha = Clamp01(1.0f - FMath::Exp(-FMath::Max(DeltaTime, 0.0f) / Tau));
        return Clamp01(FMath::Lerp(Current, Target, Alpha));
    }

    static float SmoothStep01(float X)
    {
        X = Clamp01(X);
        return X * X * (3.0f - 2.0f * X);
    }

    static void AccumulateSpeechModifier(FOffgridAIEmotionSpeechModifiers& InOut, const FOffgridAIEmotionSpeechModifiers& Incoming, float Weight)
    {
        const float W = Clamp01(Weight);
        InOut.JawOpenBaseline += Incoming.JawOpenBaseline * W;
        InOut.JawOpenScale += (Incoming.JawOpenScale - 1.0f) * W;
        InOut.TeethShowScale += (Incoming.TeethShowScale - 1.0f) * W;
        InOut.LipCloseScale += (Incoming.LipCloseScale - 1.0f) * W;
        InOut.LipRoundScale += (Incoming.LipRoundScale - 1.0f) * W;
        InOut.LipWideScale += (Incoming.LipWideScale - 1.0f) * W;
        InOut.UpperLipRaiseBias += Incoming.UpperLipRaiseBias * W;
        InOut.LowerLipDepressBias += Incoming.LowerLipDepressBias * W;
        InOut.MouthCornerBias += Incoming.MouthCornerBias * W;
        InOut.MouthTightenBias += Incoming.MouthTightenBias * W;
        InOut.JawForwardBias += Incoming.JawForwardBias * W;
        InOut.JawClenchBias += Incoming.JawClenchBias * W;
        InOut.AttackSpeedScale += (Incoming.AttackSpeedScale - 1.0f) * W;
        InOut.ReleaseSpeedScale += (Incoming.ReleaseSpeedScale - 1.0f) * W;
        InOut.SpeechCriticalMouthExpressionScale += (Incoming.SpeechCriticalMouthExpressionScale - 0.04f) * W;
        InOut.SharedMouthExpressionScale += (Incoming.SharedMouthExpressionScale - 0.08f) * W;
        InOut.MouthCornerExpressionScale += (Incoming.MouthCornerExpressionScale - 0.18f) * W;
        InOut.MouthCornerBilabialExpressionScale += (Incoming.MouthCornerBilabialExpressionScale - 0.04f) * W;
    }

    static void ClampSpeechModifiers(FOffgridAIEmotionSpeechModifiers& InOut)
    {
        InOut.JawOpenBaseline = FMath::Clamp(InOut.JawOpenBaseline, -1.0f, 1.0f);
        InOut.JawOpenScale = FMath::Clamp(InOut.JawOpenScale, 0.0f, 3.0f);
        InOut.TeethShowScale = FMath::Clamp(InOut.TeethShowScale, 0.0f, 3.0f);
        InOut.LipCloseScale = FMath::Clamp(InOut.LipCloseScale, 0.0f, 3.0f);
        InOut.LipRoundScale = FMath::Clamp(InOut.LipRoundScale, 0.0f, 3.0f);
        InOut.LipWideScale = FMath::Clamp(InOut.LipWideScale, 0.0f, 3.0f);
        InOut.UpperLipRaiseBias = FMath::Clamp(InOut.UpperLipRaiseBias, -1.0f, 1.0f);
        InOut.LowerLipDepressBias = FMath::Clamp(InOut.LowerLipDepressBias, -1.0f, 1.0f);
        InOut.MouthCornerBias = FMath::Clamp(InOut.MouthCornerBias, -1.0f, 1.0f);
        InOut.MouthTightenBias = FMath::Clamp(InOut.MouthTightenBias, -1.0f, 1.0f);
        InOut.JawForwardBias = FMath::Clamp(InOut.JawForwardBias, -1.0f, 1.0f);
        InOut.JawClenchBias = FMath::Clamp(InOut.JawClenchBias, -1.0f, 1.0f);
        InOut.AttackSpeedScale = FMath::Clamp(InOut.AttackSpeedScale, 0.1f, 4.0f);
        InOut.ReleaseSpeedScale = FMath::Clamp(InOut.ReleaseSpeedScale, 0.1f, 4.0f);
        InOut.SpeechCriticalMouthExpressionScale = FMath::Clamp(InOut.SpeechCriticalMouthExpressionScale, 0.0f, 1.0f);
        InOut.SharedMouthExpressionScale = FMath::Clamp(InOut.SharedMouthExpressionScale, 0.0f, 1.0f);
        InOut.MouthCornerExpressionScale = FMath::Clamp(InOut.MouthCornerExpressionScale, 0.0f, 1.0f);
        InOut.MouthCornerBilabialExpressionScale = FMath::Clamp(InOut.MouthCornerBilabialExpressionScale, 0.0f, 1.0f);
    }
}

bool FOffgridAIEmotionExpression::LoadLibrary(const FString& AbsolutePath)
{
    EmotionBandsByFamily.Reset();
    CompoundEmotionPoses.Reset();

    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *AbsolutePath))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[EmotionExpression] failed to load emotion JSON: %s"), *AbsolutePath);
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[EmotionExpression] failed to parse emotion JSON: %s"), *AbsolutePath);
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Pure = nullptr;
    int32 PureSeen = 0;
    int32 PureAdded = 0;
    int32 PureSkippedNoControls = 0;
    int32 PureSkippedNoFamily = 0;
    int32 PureSkippedNoPose = 0;

    if (Root->TryGetArrayField(TEXT("pure_emotions"), Pure))
    {
        UE_LOG(LogOffgridAI, Log, TEXT("[EmotionExpression] emotion JSON pure_emotions entries=%d"), Pure->Num());
        for (const TSharedPtr<FJsonValue>& Value : *Pure)
        {
            ++PureSeen;
            const TSharedPtr<FJsonObject> Obj = Value->AsObject();
            if (!Obj.IsValid()) { continue; }

            FEmotionBand Band;
            Band.PoseID = FName(*ReadStringField(Obj, TEXT("id"), ReadStringField(Obj, TEXT("pose_name"))));

            FString FamilyString = ReadStringField(Obj, TEXT("emotion"));
            if (FamilyString.IsEmpty())
            {
                FamilyString = ReadStringField(Obj, TEXT("family"));
            }
            Band.Family = NormalizeEmotionFamily(FName(*FamilyString));
            if (Band.Family == NAME_None)
            {
                const FString PoseStr = Band.PoseID.ToString();
                FString Left, Right;
                if (PoseStr.Split(TEXT("-"), &Left, &Right))
                {
                    Band.Family = NormalizeEmotionFamily(FName(*Left));
                }
            }

            const TSharedPtr<FJsonObject>* Intensity = nullptr;
            if (Obj->TryGetObjectField(TEXT("intensity"), Intensity))
            {
                Band.Min = ReadNumberField(*Intensity, TEXT("min"), 0.0f);
                Band.Peak = ReadNumberField(*Intensity, TEXT("peak"), 0.5f);
                Band.Max = ReadNumberField(*Intensity, TEXT("max"), 1.0f);
            }
            else
            {
                Band.Min = ReadNumberField(Obj, TEXT("intensity_min"), 0.0f);
                Band.Peak = ReadNumberField(Obj, TEXT("intensity_peak"), 0.5f);
                Band.Max = ReadNumberField(Obj, TEXT("intensity_max"), 1.0f);
            }

            const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
            const bool bHasControlsArray = Obj->TryGetArrayField(TEXT("controls"), Controls);
            const bool bParsedControls = bHasControlsArray && ParseControlsArray(*Controls, Band.Controls);
            Band.SpeechModifiers = ParseSpeechModifiers(Obj);

            if (Band.PoseID == NAME_None)
            {
                ++PureSkippedNoPose;
            }
            else if (Band.Family == NAME_None)
            {
                ++PureSkippedNoFamily;
            }
            else if (!bParsedControls)
            {
                ++PureSkippedNoControls;
            }
            else
            {
                EmotionBandsByFamily.FindOrAdd(Band.Family).Add(Band);
                ++PureAdded;
                if (PureAdded <= 6)
                {
                    UE_LOG(LogOffgridAI, Log, TEXT("[EmotionExpression] band added pose=%s raw_family=%s family=%s controls=%d min=%.3f peak=%.3f max=%.3f"),
                        *Band.PoseID.ToString(), *FamilyString, *Band.Family.ToString(), Band.Controls.Num(), Band.Min, Band.Peak, Band.Max);
                }
            }
        }
    }
    else
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[EmotionExpression] emotion JSON missing root array 'pure_emotions' path=%s"), *AbsolutePath);
    }

    UE_LOG(LogOffgridAI, Warning, TEXT("[EmotionExpression] parse summary path=%s schema=%s pure_seen=%d pure_added=%d skipped_no_pose=%d skipped_no_family=%d skipped_no_controls=%d"),
        *AbsolutePath,
        *ReadStringField(Root, TEXT("schema"), TEXT("<missing>")),
        PureSeen, PureAdded, PureSkippedNoPose, PureSkippedNoFamily, PureSkippedNoControls);

    const TArray<TSharedPtr<FJsonValue>>* Compounds = nullptr;
    if (Root->TryGetArrayField(TEXT("compound_emotions"), Compounds))
    {
        for (const TSharedPtr<FJsonValue>& Value : *Compounds)
        {
            const TSharedPtr<FJsonObject> Obj = Value->AsObject();
            if (!Obj.IsValid()) { continue; }
            FRuntimePose Pose;
            Pose.PoseID = FName(*ReadStringField(Obj, TEXT("id"), ReadStringField(Obj, TEXT("pose_name"))));
            const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
            Pose.SpeechModifiers = ParseSpeechModifiers(Obj);
            if (Pose.PoseID != NAME_None && Obj->TryGetArrayField(TEXT("controls"), Controls) && ParseControlsArray(*Controls, Pose.Controls))
            {
                CompoundEmotionPoses.Add(Pose.PoseID, Pose);
            }
        }
    }

    for (TPair<FName, TArray<FEmotionBand>>& Pair : EmotionBandsByFamily)
    {
        Pair.Value.Sort([](const FEmotionBand& A, const FEmotionBand& B)
        {
            return A.Peak < B.Peak;
        });
    }

    FString FamilySummary;
    for (const TPair<FName, TArray<FEmotionBand>>& Pair : EmotionBandsByFamily)
    {
        FamilySummary += FString::Printf(TEXT("%s:%d "), *Pair.Key.ToString(), Pair.Value.Num());
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[EmotionExpression] library path=%s families=%d compounds=%d [%s]"),
        *AbsolutePath, EmotionBandsByFamily.Num(), CompoundEmotionPoses.Num(), *FamilySummary);

    return EmotionBandsByFamily.Num() > 0 || CompoundEmotionPoses.Num() > 0;
}

void FOffgridAIEmotionExpression::SetEnabled(bool bInEnabled)
{
    bEnabled = bInEnabled;
    if (!bEnabled)
    {
        ForceNeutral();
    }
}

void FOffgridAIEmotionExpression::SubmitEmotion(FName EmotionName, float Magnitude, float OverallWeight, float FamilyTransitionBlendSeconds, bool bLogDiagnostics)
{
    if (!bEnabled)
    {
        ForceNeutral();
        return;
    }

    const FName NormalizedFamily = NormalizeEmotionFamily(EmotionName);
    const bool bHasFamily = NormalizedFamily != NAME_None && EmotionBandsByFamily.Contains(NormalizedFamily);
    const bool bHasCompound = CompoundEmotionPoses.Contains(EmotionName) || CompoundEmotionPoses.Contains(NormalizedFamily);

    const FName PreviousEmotionName = TargetEmotionName;
    const FName NewTargetEmotionName = NormalizedFamily != NAME_None ? NormalizedFamily : EmotionName;
    const float NewTargetMagnitude = Clamp01(Magnitude);
    const float NewTargetOverallWeight = Clamp01(OverallWeight);

    const bool bEmotionFamilyTransition =
        PreviousEmotionName != NAME_None &&
        PreviousEmotionName != NewTargetEmotionName &&
        (CurrentEmotionOverallWeight > 0.001f || NewTargetOverallWeight > 0.001f);

    if (bEmotionFamilyTransition)
    {
        // Capture the currently visible family before replacing the target. This is
        // the important bit: family changes must blend authored control sets, not
        // just magnitudes, otherwise upper-face controls can snap when a new family
        // starts using different pose IDs.
        TransitionFromEmotionName = PreviousEmotionName;
        TransitionFromMagnitude = CurrentEmotionMagnitude;
        TransitionFromOverallWeight = CurrentEmotionOverallWeight;
        EmotionFamilyTransitionDurationSeconds = FMath::Max(FamilyTransitionBlendSeconds, 0.001f);
        EmotionFamilyTransitionElapsedSeconds = 0.0f;
        EmotionFamilyTransitionSecondsRemaining = EmotionFamilyTransitionDurationSeconds;
    }

    const bool bTargetChanged = PreviousEmotionName != NewTargetEmotionName
        || !FMath::IsNearlyEqual(TargetEmotionMagnitude, NewTargetMagnitude, 0.001f)
        || !FMath::IsNearlyEqual(TargetEmotionOverallWeight, NewTargetOverallWeight, 0.001f);

    TargetEmotionName = NewTargetEmotionName;
    TargetEmotionMagnitude = NewTargetMagnitude;
    TargetEmotionOverallWeight = NewTargetOverallWeight;

    if (bLogDiagnostics && (bTargetChanged || bEmotionFamilyTransition))
    {
        UE_LOG(LogOffgridAI, Log,
            TEXT("[EmotionExpression] target raw=%s family=%s magnitude=%.3f overall=%.3f has_family=%s has_compound=%s transition=%s from=%s"),
            *EmotionName.ToString(),
            *TargetEmotionName.ToString(),
            TargetEmotionMagnitude,
            TargetEmotionOverallWeight,
            bHasFamily ? TEXT("true") : TEXT("false"),
            bHasCompound ? TEXT("true") : TEXT("false"),
            bEmotionFamilyTransition ? TEXT("true") : TEXT("false"),
            *TransitionFromEmotionName.ToString());
    }
}

void FOffgridAIEmotionExpression::ClearEmotion(bool bLogDiagnostics)
{
    if (!bEnabled)
    {
        ForceNeutral();
        return;
    }

    if (bLogDiagnostics)
    {
        UE_LOG(LogOffgridAI, Log,
            TEXT("[EmotionExpression] ClearEmotion previous=%s magnitude=%.3f target_overall=%.3f current_overall=%.3f"),
            *TargetEmotionName.ToString(), TargetEmotionMagnitude, TargetEmotionOverallWeight, CurrentEmotionOverallWeight);
    }

    TargetEmotionMagnitude = 0.0f;
    TargetEmotionOverallWeight = 0.0f;
}

void FOffgridAIEmotionExpression::ForceNeutral()
{
    TargetEmotionName = NAME_None;
    TargetEmotionMagnitude = 0.0f;
    CurrentEmotionMagnitude = 0.0f;
    TargetEmotionOverallWeight = 0.0f;
    CurrentEmotionOverallWeight = 0.0f;
    TransitionFromEmotionName = NAME_None;
    TransitionFromMagnitude = 0.0f;
    TransitionFromOverallWeight = 0.0f;
    EmotionFamilyTransitionElapsedSeconds = 0.0f;
    EmotionFamilyTransitionDurationSeconds = 0.0f;
    EmotionFamilyTransitionSecondsRemaining = 0.0f;
}

void FOffgridAIEmotionExpression::Tick(float DeltaTimeSeconds, float BlendInSeconds, float BlendOutSeconds, float FamilyTransitionBlendSeconds)
{
    if (!bEnabled)
    {
        ForceNeutral();
        return;
    }

    const float SafeDelta = FMath::Max(DeltaTimeSeconds, 0.0f);
    const bool bUseFamilyTransitionBlend = EmotionFamilyTransitionSecondsRemaining > 0.0f;
    const float ActiveBlendInSeconds = bUseFamilyTransitionBlend ? FMath::Max(FamilyTransitionBlendSeconds, 0.001f) : BlendInSeconds;
    const float ActiveBlendOutSeconds = bUseFamilyTransitionBlend ? FMath::Max(FamilyTransitionBlendSeconds, 0.001f) : BlendOutSeconds;

    CurrentEmotionOverallWeight = StepBlend(CurrentEmotionOverallWeight, TargetEmotionOverallWeight, SafeDelta, ActiveBlendInSeconds, ActiveBlendOutSeconds);
    CurrentEmotionMagnitude = StepBlend(CurrentEmotionMagnitude, TargetEmotionMagnitude, SafeDelta, ActiveBlendInSeconds, ActiveBlendOutSeconds);

    if (EmotionFamilyTransitionSecondsRemaining > 0.0f)
    {
        EmotionFamilyTransitionElapsedSeconds = FMath::Min(
            EmotionFamilyTransitionElapsedSeconds + SafeDelta,
            FMath::Max(EmotionFamilyTransitionDurationSeconds, 0.001f));
        EmotionFamilyTransitionSecondsRemaining = FMath::Max(0.0f, EmotionFamilyTransitionSecondsRemaining - SafeDelta);

        if (EmotionFamilyTransitionSecondsRemaining <= 0.0f)
        {
            TransitionFromEmotionName = NAME_None;
            TransitionFromMagnitude = 0.0f;
            TransitionFromOverallWeight = 0.0f;
            EmotionFamilyTransitionElapsedSeconds = 0.0f;
            EmotionFamilyTransitionDurationSeconds = 0.0f;
        }
    }

    if (TargetEmotionOverallWeight <= 0.001f && CurrentEmotionOverallWeight <= 0.001f && CurrentEmotionMagnitude <= 0.001f)
    {
        TargetEmotionName = NAME_None;
        CurrentEmotionOverallWeight = 0.0f;
        CurrentEmotionMagnitude = 0.0f;
        TransitionFromEmotionName = NAME_None;
        TransitionFromMagnitude = 0.0f;
        TransitionFromOverallWeight = 0.0f;
        EmotionFamilyTransitionElapsedSeconds = 0.0f;
        EmotionFamilyTransitionDurationSeconds = 0.0f;
        EmotionFamilyTransitionSecondsRemaining = 0.0f;
    }
}

void FOffgridAIEmotionExpression::ResolvePoseSamplesForEmotion(FName EmotionName, float Magnitude, float OverallWeight, TArray<FOffgridAIFacePoseSample>& OutSamples) const
{
    const float ClampedOverall = Clamp01(OverallWeight);
    if (!bEnabled || ClampedOverall <= 0.001f || EmotionName == NAME_None)
    {
        return;
    }

    if (CompoundEmotionPoses.Contains(EmotionName))
    {
        OutSamples.Add(MakePoseSample(EmotionName, ClampedOverall));
        return;
    }

    const FName Family = NormalizeEmotionFamily(EmotionName);
    const TArray<FEmotionBand>* Bands = EmotionBandsByFamily.Find(Family);
    if (!Bands)
    {
        return;
    }

    // Authored emotion entries are ordered stages on a continuous 0..1 ramp.
    TArray<FOffgridAIFacePoseSample> Raw;
    const float X = Clamp01(Magnitude);

    if (Bands->Num() == 1)
    {
        Raw.Add(MakePoseSample((*Bands)[0].PoseID, X));
    }
    else if (X <= (*Bands)[0].Peak)
    {
        const float FirstPeak = FMath::Max((*Bands)[0].Peak, 0.001f);
        Raw.Add(MakePoseSample((*Bands)[0].PoseID, Clamp01(X / FirstPeak)));
    }
    else
    {
        bool bResolvedBetweenStages = false;
        for (int32 Index = 0; Index < Bands->Num() - 1; ++Index)
        {
            const FEmotionBand& A = (*Bands)[Index];
            const FEmotionBand& B = (*Bands)[Index + 1];
            if (X <= B.Peak)
            {
                const float Span = FMath::Max(B.Peak - A.Peak, 0.001f);
                const float T = Clamp01((X - A.Peak) / Span);
                Raw.Add(MakePoseSample(A.PoseID, 1.0f - T));
                Raw.Add(MakePoseSample(B.PoseID, T));
                bResolvedBetweenStages = true;
                break;
            }
        }

        if (!bResolvedBetweenStages)
        {
            Raw.Add(MakePoseSample((*Bands).Last().PoseID, 1.0f));
        }
    }

    for (FOffgridAIFacePoseSample& Sample : Raw)
    {
        Sample.Weight = Clamp01(Sample.Weight * ClampedOverall);
        if (Sample.Weight > 0.001f)
        {
            OutSamples.Add(Sample);
        }
    }
}

void FOffgridAIEmotionExpression::ResolvePoseSamples(TArray<FOffgridAIFacePoseSample>& OutSamples, bool bLogDiagnostics) const
{
    OutSamples.Reset();

    if (!bEnabled || CurrentEmotionOverallWeight <= 0.001f || TargetEmotionName == NAME_None)
    {
        return;
    }

    const bool bFamilyTransitionActive =
        TransitionFromEmotionName != NAME_None &&
        EmotionFamilyTransitionDurationSeconds > 0.001f &&
        EmotionFamilyTransitionSecondsRemaining > 0.0f;

    const float LinearAlpha = bFamilyTransitionActive
        ? Clamp01(EmotionFamilyTransitionElapsedSeconds / FMath::Max(EmotionFamilyTransitionDurationSeconds, 0.001f))
        : 1.0f;
    const float SmoothAlpha = SmoothStep01(LinearAlpha);

    if (bFamilyTransitionActive)
    {
        ResolvePoseSamplesForEmotion(
            TransitionFromEmotionName,
            TransitionFromMagnitude,
            TransitionFromOverallWeight * (1.0f - SmoothAlpha),
            OutSamples);
    }

    ResolvePoseSamplesForEmotion(
        TargetEmotionName,
        CurrentEmotionMagnitude,
        CurrentEmotionOverallWeight * SmoothAlpha,
        OutSamples);

    if (bLogDiagnostics)
    {
        FString SamplesSummary;
        for (const FOffgridAIFacePoseSample& Sample : OutSamples)
        {
            SamplesSummary += FString::Printf(TEXT("%s=%.2f "), *Sample.PoseID.ToString(), Sample.Weight);
        }

        UE_LOG(LogOffgridAI, Log,
            TEXT("[EmotionExpression] ResolvePoseSamples target=%s current_magnitude=%.3f current_overall=%.3f transition_from=%s transition_alpha=%.3f samples=[%s]"),
            *TargetEmotionName.ToString(),
            CurrentEmotionMagnitude,
            CurrentEmotionOverallWeight,
            *TransitionFromEmotionName.ToString(),
            SmoothAlpha,
            *SamplesSummary);
    }
}

const FOffgridAIEmotionExpression::FEmotionBand* FOffgridAIEmotionExpression::FindBandByPoseID(FName PoseID) const
{
    if (PoseID == NAME_None)
    {
        return nullptr;
    }

    for (const TPair<FName, TArray<FEmotionBand>>& Pair : EmotionBandsByFamily)
    {
        for (const FEmotionBand& Band : Pair.Value)
        {
            if (Band.PoseID == PoseID)
            {
                return &Band;
            }
        }
    }

    return nullptr;
}

void FOffgridAIEmotionExpression::BuildControlTargets(const TArray<FOffgridAIFacePoseSample>& Samples, float Strength, TMap<FName, FOffgridAIFaceDriverControlValue>& OutControls) const
{
    OutControls.Reset();

    for (const FOffgridAIFacePoseSample& Sample : Samples)
    {
        if (const FRuntimePose* Compound = CompoundEmotionPoses.Find(Sample.PoseID))
        {
            AddWeightedControls(Compound->Controls, Sample.Weight * Strength, OutControls);
            continue;
        }

        if (const FEmotionBand* Band = FindBandByPoseID(Sample.PoseID))
        {
            AddWeightedControls(Band->Controls, Sample.Weight * Strength, OutControls);
        }
    }
}


FOffgridAIEmotionSpeechModifiers FOffgridAIEmotionExpression::ResolveSpeechModifiers(bool bLogDiagnostics) const
{
    FOffgridAIEmotionSpeechModifiers Out;

    TArray<FOffgridAIFacePoseSample> Samples;
    ResolvePoseSamples(Samples, false);

    for (const FOffgridAIFacePoseSample& Sample : Samples)
    {
        if (const FRuntimePose* Compound = CompoundEmotionPoses.Find(Sample.PoseID))
        {
            AccumulateSpeechModifier(Out, Compound->SpeechModifiers, Sample.Weight);
            continue;
        }

        if (const FEmotionBand* Band = FindBandByPoseID(Sample.PoseID))
        {
            AccumulateSpeechModifier(Out, Band->SpeechModifiers, Sample.Weight);
        }
    }

    ClampSpeechModifiers(Out);

    if (bLogDiagnostics && Samples.Num() > 0)
    {
        UE_LOG(LogOffgridAI, Verbose,
            TEXT("[EmotionExpression] speech_modifiers emotion=%s mag=%.3f jaw_base=%.3f jaw_scale=%.3f teeth=%.3f close=%.3f round=%.3f wide=%.3f upper_lip=%.3f lower_lip=%.3f corner=%.3f tighten=%.3f jaw_fwd=%.3f clench=%.3f attack=%.3f release=%.3f mouth_crit=%.3f mouth_shared=%.3f mouth_corner=%.3f mouth_bilabial=%.3f"),
            *TargetEmotionName.ToString(), CurrentEmotionMagnitude, Out.JawOpenBaseline, Out.JawOpenScale, Out.TeethShowScale, Out.LipCloseScale, Out.LipRoundScale, Out.LipWideScale, Out.UpperLipRaiseBias, Out.LowerLipDepressBias, Out.MouthCornerBias, Out.MouthTightenBias, Out.JawForwardBias, Out.JawClenchBias, Out.AttackSpeedScale, Out.ReleaseSpeedScale, Out.SpeechCriticalMouthExpressionScale, Out.SharedMouthExpressionScale, Out.MouthCornerExpressionScale, Out.MouthCornerBilabialExpressionScale);
    }

    return Out;
}

void FOffgridAIEmotionExpression::AppendNeutralControls(TMap<FName, FOffgridAIFaceDriverControlValue>& InOutZeros) const
{
    auto AddZeros = [&InOutZeros](const TArray<FOffgridAIFaceDriverControlValue>& Controls)
    {
        for (const FOffgridAIFaceDriverControlValue& Control : Controls)
        {
            if (Control.ControlName == NAME_None)
            {
                continue;
            }
            FOffgridAIFaceDriverControlValue Zero = Control;
            Zero.FloatValue = 0.0f;
            Zero.Vector2DValue = FVector2D::ZeroVector;
            InOutZeros.FindOrAdd(Zero.ControlName) = Zero;
        }
    };

    for (const TPair<FName, TArray<FEmotionBand>>& Pair : EmotionBandsByFamily)
    {
        for (const FEmotionBand& Band : Pair.Value)
        {
            AddZeros(Band.Controls);
        }
    }
    for (const TPair<FName, FRuntimePose>& Pair : CompoundEmotionPoses)
    {
        AddZeros(Pair.Value.Controls);
    }
}

FName FOffgridAIEmotionExpression::NormalizeEmotionFamily(FName EmotionName)
{
    const FString S = EmotionName.ToString().TrimStartAndEnd().ToLower();
    if (S.IsEmpty() || S == TEXT("none") || S == TEXT("neutral"))
    {
        return NAME_None;
    }
    if (S == TEXT("joy") || S == TEXT("happy") || S == TEXT("happiness") || S == TEXT("joyful") || S == TEXT("amused") || S == TEXT("amusement"))
    {
        return TEXT("joy");
    }
    if (S == TEXT("sadness") || S == TEXT("sad") || S == TEXT("sorrow") || S == TEXT("unhappy"))
    {
        return TEXT("sadness");
    }
    if (S == TEXT("anger") || S == TEXT("angry") || S == TEXT("mad") || S == TEXT("rage") || S == TEXT("furious"))
    {
        return TEXT("anger");
    }
    if (S == TEXT("fear") || S == TEXT("fearful") || S == TEXT("afraid") || S == TEXT("scared") || S == TEXT("anxious"))
    {
        return TEXT("fear");
    }
    if (S == TEXT("surprise") || S == TEXT("surprised") || S == TEXT("shock") || S == TEXT("shocked"))
    {
        return TEXT("surprise");
    }
    if (S == TEXT("disgust") || S == TEXT("disgusted") || S == TEXT("revulsion") || S == TEXT("revulsed"))
    {
        return TEXT("disgust");
    }
    return FName(*S);
}

bool FOffgridAIEmotionExpression::ParseControlsArray(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FOffgridAIFaceDriverControlValue>& OutControls) const
{
    OutControls.Reset();
    for (const TSharedPtr<FJsonValue>& Value : Values)
    {
        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid()) { continue; }

        FOffgridAIFaceDriverControlValue Control;
        Control.ControlName = FName(*ReadStringField(Obj, TEXT("control"), ReadStringField(Obj, TEXT("name"))));
        if (Control.ControlName == NAME_None) { continue; }

        const TArray<TSharedPtr<FJsonValue>>* Vec = nullptr;
        const FString Type = ReadStringField(Obj, TEXT("type")).ToLower();
        if (Obj->TryGetArrayField(TEXT("value"), Vec) && Vec->Num() >= 2)
        {
            Control.bIsVector2D = true;
            Control.Vector2DValue = FVector2D(static_cast<float>((*Vec)[0]->AsNumber()), static_cast<float>((*Vec)[1]->AsNumber()));
            Control.FloatValue = 0.0f;
        }
        else if (Type == TEXT("vector2d") || Obj->HasField(TEXT("x")) || Obj->HasField(TEXT("y")))
        {
            Control.bIsVector2D = true;
            Control.Vector2DValue = FVector2D(ReadNumberField(Obj, TEXT("x"), 0.0f), ReadNumberField(Obj, TEXT("y"), 0.0f));
            Control.FloatValue = 0.0f;
        }
        else
        {
            Control.bIsVector2D = false;
            Control.FloatValue = ReadNumberField(Obj, TEXT("value"), 0.0f);
            Control.Vector2DValue = FVector2D::ZeroVector;
        }
        OutControls.Add(Control);
    }
    return OutControls.Num() > 0;
}


FOffgridAIEmotionSpeechModifiers FOffgridAIEmotionExpression::ParseSpeechModifiers(const TSharedPtr<FJsonObject>& Obj) const
{
    FOffgridAIEmotionSpeechModifiers Out;
    const TSharedPtr<FJsonObject>* Modifiers = nullptr;
    if (!Obj.IsValid() || !Obj->TryGetObjectField(TEXT("speech_modifiers"), Modifiers) || !Modifiers || !Modifiers->IsValid())
    {
        return Out;
    }

    Out.JawOpenBaseline = ReadNumberField(*Modifiers, TEXT("jaw_open_baseline"), Out.JawOpenBaseline);
    Out.JawOpenScale = ReadNumberField(*Modifiers, TEXT("jaw_open_scale"), Out.JawOpenScale);
    Out.TeethShowScale = ReadNumberField(*Modifiers, TEXT("teeth_show_scale"), Out.TeethShowScale);
    Out.LipCloseScale = ReadNumberField(*Modifiers, TEXT("lip_close_scale"), Out.LipCloseScale);
    Out.LipRoundScale = ReadNumberField(*Modifiers, TEXT("lip_round_scale"), Out.LipRoundScale);
    Out.LipWideScale = ReadNumberField(*Modifiers, TEXT("lip_wide_scale"), Out.LipWideScale);
    Out.UpperLipRaiseBias = ReadNumberField(*Modifiers, TEXT("upper_lip_raise_bias"), Out.UpperLipRaiseBias);
    Out.LowerLipDepressBias = ReadNumberField(*Modifiers, TEXT("lower_lip_depress_bias"), Out.LowerLipDepressBias);
    Out.MouthCornerBias = ReadNumberField(*Modifiers, TEXT("mouth_corner_bias"), Out.MouthCornerBias);
    Out.MouthTightenBias = ReadNumberField(*Modifiers, TEXT("mouth_tighten_bias"), Out.MouthTightenBias);
    Out.JawForwardBias = ReadNumberField(*Modifiers, TEXT("jaw_forward_bias"), Out.JawForwardBias);
    Out.JawClenchBias = ReadNumberField(*Modifiers, TEXT("jaw_clench_bias"), Out.JawClenchBias);
    Out.AttackSpeedScale = ReadNumberField(*Modifiers, TEXT("attack_speed_scale"), Out.AttackSpeedScale);
    Out.ReleaseSpeedScale = ReadNumberField(*Modifiers, TEXT("release_speed_scale"), Out.ReleaseSpeedScale);
    Out.SpeechCriticalMouthExpressionScale = ReadNumberField(*Modifiers, TEXT("speech_critical_mouth_expression_scale"), Out.SpeechCriticalMouthExpressionScale);
    Out.SharedMouthExpressionScale = ReadNumberField(*Modifiers, TEXT("shared_mouth_expression_scale"), Out.SharedMouthExpressionScale);
    Out.MouthCornerExpressionScale = ReadNumberField(*Modifiers, TEXT("mouth_corner_expression_scale"), Out.MouthCornerExpressionScale);
    Out.MouthCornerBilabialExpressionScale = ReadNumberField(*Modifiers, TEXT("mouth_corner_bilabial_expression_scale"), Out.MouthCornerBilabialExpressionScale);
    ClampSpeechModifiers(Out);
    return Out;
}

void FOffgridAIEmotionExpression::AddWeightedControls(const TArray<FOffgridAIFaceDriverControlValue>& Controls, float Weight, TMap<FName, FOffgridAIFaceDriverControlValue>& InOutValues) const
{
    const float W = Clamp01(Weight);
    if (W <= 0.001f)
    {
        return;
    }

    for (const FOffgridAIFaceDriverControlValue& Control : Controls)
    {
        if (Control.ControlName == NAME_None)
        {
            continue;
        }
        FOffgridAIFaceDriverControlValue& Existing = InOutValues.FindOrAdd(Control.ControlName);
        if (Existing.ControlName == NAME_None)
        {
            Existing = MakeWeightedControlValue(Control, W);
        }
        else
        {
            AccumulateControl(Existing, Control, W);
        }
    }
}
