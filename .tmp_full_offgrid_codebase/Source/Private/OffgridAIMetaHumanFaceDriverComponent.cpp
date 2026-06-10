#include "OffgridAIMetaHumanFaceDriverComponent.h"

#include "Emotion/OffgridAIEmotionExpression.h"

#include "OffgridAI.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"


namespace
{
static FString ResolveOffgridAIFaceLibraryPath(const FString& RelativeOrAbsolutePath)
{
    TArray<FString> Candidates;

    if (FPaths::IsRelative(RelativeOrAbsolutePath))
    {
        Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), RelativeOrAbsolutePath)));
        Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("OffgridAI/Content"), RelativeOrAbsolutePath)));
        Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/OffgridAI/Content"), RelativeOrAbsolutePath)));
        Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("OffgridAI/Content"), RelativeOrAbsolutePath)));
    }
    else
    {
        Candidates.Add(FPaths::ConvertRelativePathToFull(RelativeOrAbsolutePath));
    }

    for (const FString& Candidate : Candidates)
    {
        if (FPaths::FileExists(Candidate))
        {
            return Candidate;
        }
    }

    // Return the first candidate so the caller's load failure log shows the most likely path.
    return Candidates.Num() > 0 ? Candidates[0] : RelativeOrAbsolutePath;
}
}
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
        // The authored emotion bands are triangular, but the first and last bands
        // intentionally cover the hard endpoints. A pure mathematical triangle
        // returns 0 at Max, which makes magnitude=1.0 resolve to no emotion.
        // Treat the outer endpoints as saturated shoulders for the edge bands.
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

        // Endpoint shoulders. These make the lowest/highest authored bands usable
        // at exactly 0.0 and 1.0 while preserving triangular blending internally.
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

    static bool IsCentralJawControlName(FName ControlName)
    {
        return ControlName.ToString().Equals(TEXT("CTRL_C_jaw"), ESearchCase::CaseSensitive);
    }

    static FOffgridAIFaceDriverControlValue ClampVisemeJawOpenForSpeech(const FOffgridAIFaceDriverControlValue& InValue)
    {
        // v26: The trusted text plan can correctly submit strong exact vowel poses,
        // but the MetaHuman jaw control authored in those poses is too open when
        // played literally over a dense sentence. v28 lowers the cap a bit
        // further after v27 improved clarity but left the mouth slightly ajar.
        // Keep lip/round/stretch controls intact and only cap the central jaw-open axis for viseme speech. This is
        // a layer-4 rig-safety cap, not a layer-1/2/3 timing or strength change.
        constexpr float MaxVisemeJawOpenY = 0.29f;

        FOffgridAIFaceDriverControlValue Out = InValue;
        if (!IsCentralJawControlName(Out.ControlName))
        {
            return Out;
        }

        if (Out.bIsVector2D)
        {
            Out.Vector2DValue.Y = FMath::Min(Out.Vector2DValue.Y, MaxVisemeJawOpenY);
        }
        else
        {
            Out.FloatValue = FMath::Min(Out.FloatValue, MaxVisemeJawOpenY);
        }
        return Out;
    }

    static float GetActiveSpeechVisemeBlendInSeconds()
    {
        // v27: During active speech, FaceDriver should closely follow the
        // trusted layer-3 event targets. The old two-stage 35/70 ms smoothing
        // made strong mid-line visemes arrive late and coexist with stale prior
        // visemes, which read as muddy/ajar mouth.
        return 0.010f;
    }

    static float GetActiveSpeechVisemeBlendOutSeconds()
    {
        return 0.024f;
    }

    static float GetPostSpeechVisemeReleaseSeconds(float ConfiguredReleaseSeconds)
    {
        // End-of-speech should not hard-pop to neutral. Use roughly twice the
        // authored viseme release as the explicit line-end ease-off, while active
        // speech still uses the much faster ownership release above.
        return FMath::Max(ConfiguredReleaseSeconds * 2.0f, 0.120f);
    }


    static bool IsRoundOrFunnelVisemePoseName(FName PoseIDOrAlias)
    {
        const FString S = PoseIDOrAlias.ToString();
        return S.Contains(TEXT("09_Oh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("10_Or"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("11_Oo"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("12_Ww"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("16_Ww"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ww"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Oo"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Oh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Or"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ew"), ESearchCase::IgnoreCase);
    }

    static bool IsAyOrWideVisemePoseName(FName PoseIDOrAlias)
    {
        const FString S = PoseIDOrAlias.ToString();
        return S.Contains(TEXT("05_Ay"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("03_Ee"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("04_Ih"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("06_Eh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ay"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ee"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ih"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Eh"), ESearchCase::IgnoreCase);
    }

    static bool IsOpenOrVowelVisemePoseName(FName PoseIDOrAlias)
    {
        const FString S = PoseIDOrAlias.ToString();
        return IsAyOrWideVisemePoseName(PoseIDOrAlias)
            || IsRoundOrFunnelVisemePoseName(PoseIDOrAlias)
            || S.Contains(TEXT("07_Aa"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("08_Ah"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("18_Uh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Aa"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ah"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Uh"), ESearchCase::IgnoreCase);
    }

    static bool IsHardLandmarkVisemePoseName(FName PoseIDOrAlias)
    {
        const FString S = PoseIDOrAlias.ToString();
        return S.Contains(TEXT("22_MBP"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("MBP"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("01_TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("02_TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Tongue"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("FV"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase);
    }

    static bool IsTDSOrTHVisemePoseName(FName PoseIDOrAlias)
    {
        const FString S = PoseIDOrAlias.ToString();
        return S.Contains(TEXT("01_TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("02_TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Tongue_Th"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("24_Tongue_Th"), ESearchCase::IgnoreCase);
    }

    static bool IsJawOrOpenMouthControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("jaw"))
            || S.Contains(TEXT("lowerlipdepress"))
            || S.Contains(TEXT("lower_lip_depress"))
            || S.Contains(TEXT("upperlipraise"))
            || S.Contains(TEXT("upper_lip_raise"))
            || S.Contains(TEXT("mouth_towards"))
            || S.Contains(TEXT("mouthtowards"));
    }

    static bool IsTeethExpressionControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("teeth"));
    }

    static bool IsLipCloseExpressionControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("lipstogether"))
            || S.Contains(TEXT("lip_together"))
            || S.Contains(TEXT("lippress"))
            || S.Contains(TEXT("lip_press"))
            || S.Contains(TEXT("pressu"))
            || S.Contains(TEXT("pressd"))
            || S.Contains(TEXT("lipbite"))
            || S.Contains(TEXT("lip_bite"));
    }

    static bool IsLipRoundExpressionControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("funnel"))
            || S.Contains(TEXT("purse"))
            || S.Contains(TEXT("towardsu"))
            || S.Contains(TEXT("towardsd"))
            || S.Contains(TEXT("mouth_towards"))
            || S.Contains(TEXT("mouthtowards"));
    }

    static bool IsLipWideExpressionControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("stretch"));
    }

    static bool IsUpperLipRaiseExpressionControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("upperlipraise")) || S.Contains(TEXT("upper_lip_raise"));
    }

    static bool IsMouthCornerBiasControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("cornerpull"))
            || S.Contains(TEXT("cornerdepress"))
            || S.Contains(TEXT("sharpcornerpull"))
            || S.Contains(TEXT("dimple"));
    }

    static void ScaleControlValue(FOffgridAIFaceDriverControlValue& Value, float Scale)
    {
        if (Value.bIsVector2D)
        {
            Value.Vector2DValue *= Scale;
        }
        else
        {
            Value.FloatValue *= Scale;
        }
    }

    static void AddFloatBias(TMap<FName, FOffgridAIFaceDriverControlValue>& InOutControls, FName ControlName, float Bias)
    {
        if (FMath::IsNearlyZero(Bias) || ControlName == NAME_None)
        {
            return;
        }

        FOffgridAIFaceDriverControlValue& Value = InOutControls.FindOrAdd(ControlName);
        Value.ControlName = ControlName;
        Value.bIsVector2D = false;
        Value.FloatValue += Bias;
    }

    static void ApplyEmotionSpeechModifiersToVisemes(TMap<FName, FOffgridAIFaceDriverControlValue>& InOutVisemeControls, const FOffgridAIEmotionSpeechModifiers& Modifiers, float SpeechActivity)
    {
        const float Speech = Clamp01(SpeechActivity);
        if (Speech <= 0.001f)
        {
            return;
        }

        const float JawScale = FMath::Lerp(1.0f, Modifiers.JawOpenScale, Speech);
        const float TeethScale = FMath::Lerp(1.0f, Modifiers.TeethShowScale, Speech);
        const float LipCloseScale = FMath::Lerp(1.0f, Modifiers.LipCloseScale, Speech);
        const float LipRoundScale = FMath::Lerp(1.0f, Modifiers.LipRoundScale, Speech);
        const float LipWideScale = FMath::Lerp(1.0f, Modifiers.LipWideScale, Speech);

        for (TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : InOutVisemeControls)
        {
            FOffgridAIFaceDriverControlValue& Value = Pair.Value;
            if (IsCentralJawControlName(Pair.Key))
            {
                if (Value.bIsVector2D)
                {
                    Value.Vector2DValue.Y = FMath::Max(Value.Vector2DValue.Y * JawScale, Modifiers.JawOpenBaseline * Speech);
                }
                else
                {
                    Value.FloatValue = FMath::Max(Value.FloatValue * JawScale, Modifiers.JawOpenBaseline * Speech);
                }
            }
            else if (IsTeethExpressionControlName(Pair.Key))
            {
                ScaleControlValue(Value, TeethScale);
            }
            else if (IsLipCloseExpressionControlName(Pair.Key))
            {
                ScaleControlValue(Value, LipCloseScale);
            }
            else if (IsLipRoundExpressionControlName(Pair.Key))
            {
                ScaleControlValue(Value, LipRoundScale);
            }
            else if (IsLipWideExpressionControlName(Pair.Key))
            {
                ScaleControlValue(Value, LipWideScale);
            }
        }

        if (Modifiers.JawOpenBaseline > 0.001f)
        {
            FOffgridAIFaceDriverControlValue& Jaw = InOutVisemeControls.FindOrAdd(FName(TEXT("CTRL_C_jaw")));
            Jaw.ControlName = FName(TEXT("CTRL_C_jaw"));
            Jaw.bIsVector2D = true;
            Jaw.Vector2DValue.Y = FMath::Max(Jaw.Vector2DValue.Y, Modifiers.JawOpenBaseline * Speech);
        }

        if (FMath::Abs(Modifiers.JawForwardBias) > 0.001f)
        {
            FOffgridAIFaceDriverControlValue& JawFwd = InOutVisemeControls.FindOrAdd(FName(TEXT("CTRL_C_jaw_fwdBack")));
            JawFwd.ControlName = FName(TEXT("CTRL_C_jaw_fwdBack"));
            JawFwd.bIsVector2D = false;
            JawFwd.FloatValue += Modifiers.JawForwardBias * Speech;
        }

        const float UpperLipBias = Modifiers.UpperLipRaiseBias * Speech;
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_L_mouth_upperLipRaise")), UpperLipBias);
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_R_mouth_upperLipRaise")), UpperLipBias);

        const float LowerLipBias = Modifiers.LowerLipDepressBias * Speech;
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_L_mouth_lowerLipDepress")), LowerLipBias);
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_R_mouth_lowerLipDepress")), LowerLipBias);

        const float CornerBias = Modifiers.MouthCornerBias * Speech;
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_L_mouth_cornerDepress")), CornerBias);
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_R_mouth_cornerDepress")), CornerBias);

        const float TightenBias = Modifiers.MouthTightenBias * Speech;
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_L_mouth_tightenD")), TightenBias);
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_R_mouth_tightenD")), TightenBias);
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_L_mouth_tightenU")), TightenBias);
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_R_mouth_tightenU")), TightenBias);

        const float ClenchBias = Modifiers.JawClenchBias * Speech;
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_L_jaw_clench")), ClenchBias);
        AddFloatBias(InOutVisemeControls, FName(TEXT("CTRL_R_jaw_clench")), ClenchBias);
    }

    static float GetActiveSpeechVisemeBlendInSecondsForPose(FName PoseIDOrAlias)
    {
        // v29: Layer-3 timing is already correct, but v28 showed vowel/round/funnel
        // peaks arriving about 40-55 ms late in FaceDriver. Pull only the soft
        // vowel-side pose state closer to target; keep hard landmarks close to
        // v27/v28 behavior so MBP/TDS do not become twitchy.
        if (IsOpenOrVowelVisemePoseName(PoseIDOrAlias) && !IsHardLandmarkVisemePoseName(PoseIDOrAlias))
        {
            return 0.0035f;
        }
        return GetActiveSpeechVisemeBlendInSeconds();
    }

    static float GetActiveSpeechVisemeBlendOutSecondsForPose(FName PoseIDOrAlias)
    {
        if (IsOpenOrVowelVisemePoseName(PoseIDOrAlias) && !IsHardLandmarkVisemePoseName(PoseIDOrAlias))
        {
            // v30: keep v29's improved timing, but reduce vowel-on-vowel
            // coarticulation mud. These are still layer-4 response constants:
            // they do not alter layer-1 viseme choice, layer-2 timing, or
            // layer-3 event envelopes.
            return 0.0085f;
        }
        return GetActiveSpeechVisemeBlendOutSeconds();
    }

    static bool IsSharpSpeechControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("tongue"))
            || S.Contains(TEXT("teeth"))
            || S.Contains(TEXT("lipstogether"))
            || S.Contains(TEXT("lip_together"))
            || S.Contains(TEXT("lippress"))
            || S.Contains(TEXT("lip_press"))
            || S.Contains(TEXT("lipbite"))
            || S.Contains(TEXT("lip_bite"));
    }

    static bool IsSoftSpeechLipControlName(FName ControlName)
    {
        const FString S = ControlName.ToString().ToLower();
        return S.Contains(TEXT("mouth_stretch"))
            || S.Contains(TEXT("mouthstretch"))
            || S.Contains(TEXT("mouth_purse"))
            || S.Contains(TEXT("mouthpurse"))
            || S.Contains(TEXT("mouth_funnel"))
            || S.Contains(TEXT("mouthfunnel"))
            || S.Contains(TEXT("mouth_towards"))
            || S.Contains(TEXT("mouthtowards"))
            || S.Contains(TEXT("lowerlipdepress"))
            || S.Contains(TEXT("lower_lip_depress"))
            || S.Contains(TEXT("upperlipraise"))
            || S.Contains(TEXT("upper_lip_raise"));
    }

    static float GetSpeechControlMaxUnitsPerSecond(FName ControlName)
    {
        // K2: Increase the final control-rate limiting slightly to soften
        // harsh jaw/lip deltas without changing layer-1/2/3 timing. Keep landmarks sharp:
        // closure, teeth and tongue controls get a very high cap. Jaw and soft
        // lip-shape controls get a light two-to-three-frame softening.
        if (IsSharpSpeechControlName(ControlName))
        {
            return 100.0f;
        }
        if (IsCentralJawControlName(ControlName))
        {
            return 6.0f;
        }
        if (IsSoftSpeechLipControlName(ControlName))
        {
            return 8.5f;
        }
        return 16.0f;
    }

    static float LimitControlDelta(float Current, float Desired, float DeltaTimeSeconds, float MaxUnitsPerSecond)
    {
        const float MaxDelta = FMath::Max(DeltaTimeSeconds, 0.0f) * FMath::Max(MaxUnitsPerSecond, 0.0f);
        return Current + FMath::Clamp(Desired - Current, -MaxDelta, MaxDelta);
    }

    static void ApplyActiveSpeechControlRateLimit(FName ControlName, const FOffgridAIFaceDriverControlValue& Current, FOffgridAIFaceDriverControlValue& Desired, float DeltaTimeSeconds)
    {
        const float MaxUnitsPerSecond = GetSpeechControlMaxUnitsPerSecond(ControlName);
        if (Desired.bIsVector2D)
        {
            Desired.Vector2DValue.X = LimitControlDelta(Current.Vector2DValue.X, Desired.Vector2DValue.X, DeltaTimeSeconds, MaxUnitsPerSecond);
            Desired.Vector2DValue.Y = LimitControlDelta(Current.Vector2DValue.Y, Desired.Vector2DValue.Y, DeltaTimeSeconds, MaxUnitsPerSecond);
            Desired.FloatValue = 0.0f;
        }
        else
        {
            Desired.FloatValue = LimitControlDelta(Current.FloatValue, Desired.FloatValue, DeltaTimeSeconds, MaxUnitsPerSecond);
            Desired.Vector2DValue = FVector2D::ZeroVector;
        }
    }

    static void ApplyDominantControlValue(FOffgridAIFaceDriverControlValue& Existing, const FOffgridAIFaceDriverControlValue& Incoming, float Weight)
    {
        const FOffgridAIFaceDriverControlValue Weighted = ClampVisemeJawOpenForSpeech(MakeWeightedControlValue(Incoming, Weight));

        if (Existing.ControlName == NAME_None || Existing.bIsVector2D != Weighted.bIsVector2D)
        {
            Existing = Weighted;
            return;
        }

        Existing.ControlName = Weighted.ControlName;
        Existing.bIsVector2D = Weighted.bIsVector2D;

        if (Weighted.bIsVector2D)
        {
            if (FMath::Abs(Weighted.Vector2DValue.X) > FMath::Abs(Existing.Vector2DValue.X))
            {
                Existing.Vector2DValue.X = Weighted.Vector2DValue.X;
            }
            if (FMath::Abs(Weighted.Vector2DValue.Y) > FMath::Abs(Existing.Vector2DValue.Y))
            {
                Existing.Vector2DValue.Y = Weighted.Vector2DValue.Y;
            }
            Existing.FloatValue = 0.0f;
        }
        else
        {
            if (FMath::Abs(Weighted.FloatValue) > FMath::Abs(Existing.FloatValue))
            {
                Existing.FloatValue = Weighted.FloatValue;
            }
            Existing.Vector2DValue = FVector2D::ZeroVector;
        }
    }
}

UOffgridAIMetaHumanFaceDriverComponent::UOffgridAIMetaHumanFaceDriverComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    EmotionExpression = new FOffgridAIEmotionExpression();
}

UOffgridAIMetaHumanFaceDriverComponent::~UOffgridAIMetaHumanFaceDriverComponent()
{
    delete EmotionExpression;
    EmotionExpression = nullptr;
}

void UOffgridAIMetaHumanFaceDriverComponent::BeginPlay()
{
    Super::BeginPlay();

    UE_LOG(LogOffgridAI, Log, TEXT("[FaceDriver] BeginPlay owner=%s component=%s bridge_mode=BlueprintEvent"),
        GetOwner() ? *GetOwner()->GetName() : TEXT("<no-owner>"),
        *GetName());

    ReloadPoseLibraries();
}

bool UOffgridAIMetaHumanFaceDriverComponent::ReloadPoseLibraries()
{
    VisemePoses.Reset();
    VisemeAliases.Reset();
    if (EmotionExpression)
    {
        EmotionExpression->ForceNeutral();
    }

    const FString VisemePath = ResolveOffgridAIFaceLibraryPath(VisemeLibraryRelativePath);
    const FString EmotionPath = ResolveOffgridAIFaceLibraryPath(EmotionLibraryRelativePath);

    UE_LOG(LogOffgridAI, Log, TEXT("[FaceDriver] resolved face libraries visemes=%s emotions=%s"), *VisemePath, *EmotionPath);

    const bool bVisemes = LoadVisemeLibrary(VisemePath);
    const bool bEmotions = EmotionExpression ? EmotionExpression->LoadLibrary(EmotionPath) : false;
    UE_LOG(LogOffgridAI, Log, TEXT("[FaceDriver] loaded visemes=%s emotions=%s viseme_count=%d alias_count=%d emotion_families=%d compounds=%d"),
        bVisemes ? TEXT("true") : TEXT("false"),
        bEmotions ? TEXT("true") : TEXT("false"),
        VisemePoses.Num(),
        VisemeAliases.Num(),
        EmotionExpression ? EmotionExpression->GetFamilyCount() : 0,
        EmotionExpression ? EmotionExpression->GetCompoundCount() : 0);
    return bVisemes && bEmotions;
}

void UOffgridAIMetaHumanFaceDriverComponent::SubmitVisemePose(FName PoseIDOrAlias, float Weight)
{
    if (PoseIDOrAlias == NAME_None)
    {
        return;
    }
    TargetVisemeWeights.FindOrAdd(PoseIDOrAlias) = Clamp01(Weight);
}

void UOffgridAIMetaHumanFaceDriverComponent::SubmitVisemePoseWeights(const TMap<FName, float>& PoseIDOrAliasWeights)
{
    // Version G contract enforcement:
    // FaceDriver is not a timing or pose-arbitration phase. It receives the
    // already-performed abstract viseme weights for THIS playback frame and must
    // not delay, reschedule, suppress, or reinterpret them. DebugActivePoseWeights
    // is updated here as the exact layer-3 -> layer-4 handoff, so
    // submitted_poses.csv can verify that FaceDriver saw the same pose/time that
    // VisemePerformer emitted.

    TargetVisemeWeights.Reset();
    DebugActivePoseWeights.Reset();
    for (const TPair<FName, float>& Pair : PoseIDOrAliasWeights)
    {
        if (Pair.Key != NAME_None && Pair.Value > KINDA_SMALL_NUMBER)
        {
            const float W = Clamp01(Pair.Value);
            TargetVisemeWeights.Add(Pair.Key, W);
            DebugActivePoseWeights.Add(Pair.Key, W);
        }
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::SubmitEmotion(FName EmotionName, float Magnitude, float OverallWeight)
{
    if (!EmotionExpression)
    {
        return;
    }

    const float ClampedMagnitude = Clamp01(Magnitude);
    ClampLowerFaceEmotionSmoothingForMagnitudeRetarget(EmotionName, ClampedMagnitude);

    EmotionExpression->SetEnabled(bEnableEmotionExpression);
    EmotionExpression->SubmitEmotion(EmotionName, ClampedMagnitude, OverallWeight, EmotionFamilyTransitionBlendSeconds, bLogDiagnostics);

    LastSubmittedEmotionName = EmotionName;
    LastSubmittedEmotionMagnitude = ClampedMagnitude;
}

void UOffgridAIMetaHumanFaceDriverComponent::ScaleControlMap(TMap<FName, FOffgridAIFaceDriverControlValue>& InOutControls, float Scale) const
{
    const float SafeScale = FMath::Max(0.0f, Scale);
    for (TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : InOutControls)
    {
        if (Pair.Value.bIsVector2D)
        {
            Pair.Value.Vector2DValue *= SafeScale;
        }
        else
        {
            Pair.Value.FloatValue *= SafeScale;
        }
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::ClampLowerFaceEmotionSmoothingForMagnitudeRetarget(FName EmotionName, float NewMagnitude)
{
    if (SmoothedLowerFaceEmotionControlValues.Num() == 0)
    {
        LastSubmittedEmotionName = EmotionName;
        LastSubmittedEmotionMagnitude = NewMagnitude;
        return;
    }

    const bool bSameSubmittedEmotion =
        LastSubmittedEmotionName != NAME_None &&
        EmotionName != NAME_None &&
        LastSubmittedEmotionName.IsEqual(EmotionName, ENameCase::IgnoreCase, false);

    const bool bRetargetingSameEmotionDown =
        bSameSubmittedEmotion &&
        NewMagnitude + 0.001f < LastSubmittedEmotionMagnitude;

    if (!bRetargetingSameEmotionDown)
    {
        return;
    }

    // The lower-face layer intentionally smooths more slowly than the upper face.
    // When CM relaxes an emotion from a high peak to its resting 0.2 magnitude,
    // the old slower target can otherwise keep pulling the lips toward the stale
    // peak for a few frames, then reverse direction toward 0.2. Clamp the current
    // lower-face smoothing state proportionally at the moment of the retarget so
    // the mouth follows the newly relaxed target immediately, without changing the
    // authored emotion data or the global transition speeds.
    const float Scale = LastSubmittedEmotionMagnitude > 0.001f
        ? FMath::Clamp(NewMagnitude / LastSubmittedEmotionMagnitude, 0.0f, 1.0f)
        : 0.0f;
    ScaleControlMap(SmoothedLowerFaceEmotionControlValues, Scale);
}

void UOffgridAIMetaHumanFaceDriverComponent::ClearVisemes()
{
    TargetVisemeWeights.Reset();
}

void UOffgridAIMetaHumanFaceDriverComponent::ClearVisemesImmediate()
{
    TargetVisemeWeights.Reset();
    CurrentVisemeWeights.Reset();

    // Hard-release speech controls immediately. ClearVisemes() only clears the
    // target weights and allows CurrentVisemeWeights/SmoothedControlValues to
    // decay over their normal blend-out, which can leave the jaw visibly ajar
    // after a line. This path preserves any non-speech controls already in the
    // smoothed map, but forces mouth/jaw/lip/tongue controls authored by viseme
    // poses to zero immediately.
    TMap<FName, FOffgridAIFaceDriverControlValue> ImmediateControls;
    for (const TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : SmoothedControlValues)
    {
        if (!IsSpeechCriticalControl(Pair.Key))
        {
            ImmediateControls.Add(Pair.Key, Pair.Value);
        }
    }

    for (const TPair<FName, FRuntimePose>& Pair : VisemePoses)
    {
        for (const FOffgridAIFaceDriverControlValue& Control : Pair.Value.Controls)
        {
            if (Control.ControlName == NAME_None || !IsSpeechCriticalControl(Control.ControlName))
            {
                continue;
            }

            FOffgridAIFaceDriverControlValue Zero = Control;
            Zero.FloatValue = 0.0f;
            Zero.Vector2DValue = FVector2D::ZeroVector;
            ImmediateControls.FindOrAdd(Zero.ControlName) = Zero;
        }
    }

    PublishControlValues(ImmediateControls, 0.0f, true);

    // Keep emotion smoothing intact; only remove active viseme debug entries.
    for (auto It = DebugActivePoseWeights.CreateIterator(); It; ++It)
    {
        if (VisemePoses.Contains(It.Key()))
        {
            It.RemoveCurrent();
        }
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::ConfigureEmotionMouthAllowance(float InSpeechCriticalScaleDuringSpeech, float InMouthCornerScaleDuringSpeech, float InMouthCornerScaleDuringBilabial, float InSharedMouthScaleDuringSpeech, float InSpeechHoldSeconds, float InMouthFadeInSeconds, float InMouthFadeOutSeconds, float InFullMouthAfterSilenceSeconds)
{
    EmotionSpeechControlScaleDuringSpeech = FMath::Clamp(InSpeechCriticalScaleDuringSpeech, 0.0f, 1.0f);
    EmotionMouthCornerScaleDuringSpeech = FMath::Clamp(InMouthCornerScaleDuringSpeech, 0.0f, 1.0f);
    EmotionMouthCornerScaleDuringBilabial = FMath::Clamp(InMouthCornerScaleDuringBilabial, 0.0f, 1.0f);
    EmotionSharedMouthScaleDuringSpeech = FMath::Clamp(InSharedMouthScaleDuringSpeech, 0.0f, 1.0f);
    EmotionSpeechHoldSeconds = FMath::Clamp(InSpeechHoldSeconds, 0.0f, 10.0f);
    EmotionMouthFadeInSeconds = FMath::Clamp(InMouthFadeInSeconds, 0.001f, 10.0f);
    EmotionMouthFadeOutSeconds = FMath::Clamp(InMouthFadeOutSeconds, 0.001f, 10.0f);
    EmotionFullMouthAfterSilenceSeconds = FMath::Clamp(InFullMouthAfterSilenceSeconds, 0.01f, 10.0f);
}

void UOffgridAIMetaHumanFaceDriverComponent::SetLineSpeechMouthSuppressionActive(bool bActive)
{
    if (bLineSpeechMouthSuppressionActive == bActive)
    {
        return;
    }

    bLineSpeechMouthSuppressionActive = bActive;
    if (bActive)
    {
        // Treat the whole active dialogue line as speech-owned for lower-mouth
        // emotion. This prevents smiles/frowns from reappearing during sentence
        // gaps even when instantaneous viseme/audio activity falls to zero.
        EmotionSecondsSinceSpeechActivity = 0.0f;
        EmotionMouthSilenceBlend = 0.0f;
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::ClearEmotion()
{
    LastSubmittedEmotionName = NAME_None;
    LastSubmittedEmotionMagnitude = 0.0f;
    if (EmotionExpression)
    {
        EmotionExpression->ClearEmotion(bLogDiagnostics);
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::ForceNeutral()
{
    TargetVisemeWeights.Reset();
    CurrentVisemeWeights.Reset();
    SmoothedEmotionControlValues.Reset();
    SmoothedLowerFaceEmotionControlValues.Reset();
    SmoothedControlValues.Reset();
    LastSubmittedEmotionName = NAME_None;
    LastSubmittedEmotionMagnitude = 0.0f;
    if (EmotionExpression)
    {
        EmotionExpression->ForceNeutral();
    }
    EmotionSecondsSinceSpeechActivity = 999.0f;
    EmotionMouthSilenceBlend = 1.0f;
    DebugActivePoseWeights.Reset();
    PublishNeutralZeros();
}

void UOffgridAIMetaHumanFaceDriverComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (EmotionExpression)
    {
        EmotionExpression->SetEnabled(bEnableEmotionExpression);
        if (!bEnableEmotionExpression)
        {
            SmoothedEmotionControlValues.Reset();
        }
    }

    TSet<FName> Keys;
    for (const TPair<FName, float>& Pair : TargetVisemeWeights) { Keys.Add(Pair.Key); }
    for (const TPair<FName, float>& Pair : CurrentVisemeWeights) { Keys.Add(Pair.Key); }

    const bool bHasActiveSpeechTargets = TargetVisemeWeights.Num() > 0;
    if (bHasActiveSpeechTargets)
    {
        // Version G: active speech visemes are frame-authored by
        // VisemePerformer. Do not smooth, suppress, select a strongest vowel, or
        // keep stale pose ownership in FaceDriver. Any such behavior moves the
        // observed peak away from CommittedPlaybackCenterSec and violates the
        // phase contract. Exact target copying keeps FaceDriver timing-neutral;
        // visual smoothing belongs in VisemePerformer envelopes, not here.
        CurrentVisemeWeights.Reset();
        for (const TPair<FName, float>& Pair : TargetVisemeWeights)
        {
            if (Pair.Key != NAME_None && Pair.Value > KINDA_SMALL_NUMBER)
            {
                CurrentVisemeWeights.Add(Pair.Key, Clamp01(Pair.Value));
            }
        }
    }
    else
    {
        // No active speech target: only then apply the post-speech release. This
        // release is outside the committed event stream and cannot create or move
        // an event peak because no event is being submitted.
        for (FName Key : Keys)
        {
            const float Current = CurrentVisemeWeights.FindRef(Key);
            const float Next = StepBlend(Current, 0.0f, DeltaTime, VisemeBlendInSeconds, GetPostSpeechVisemeReleaseSeconds(VisemeBlendOutSeconds));
            if (Next > 0.001f)
            {
                CurrentVisemeWeights.FindOrAdd(Key) = Next;
            }
            else
            {
                CurrentVisemeWeights.Remove(Key);
            }
        }
    }

    if (EmotionExpression)
    {
        EmotionExpression->Tick(DeltaTime, EmotionBlendInSeconds, EmotionBlendOutSeconds, EmotionFamilyTransitionBlendSeconds);
    }

    const bool bUseFamilyTransitionBlend = EmotionExpression && EmotionExpression->IsFamilyTransitionActive();
    const float ActiveEmotionBlendInSeconds = bUseFamilyTransitionBlend
        ? FMath::Max(EmotionFamilyTransitionBlendSeconds, 0.001f)
        : EmotionBlendInSeconds;
    const float ActiveEmotionBlendOutSeconds = bUseFamilyTransitionBlend
        ? FMath::Max(EmotionFamilyTransitionBlendSeconds, 0.001f)
        : EmotionBlendOutSeconds;

    // Layer 4 begins here: convert abstract current viseme weights into library
    // pose samples, then into MetaHuman control values. No timing logic belongs here.
    TArray<FOffgridAIFacePoseSample> VisemeSamples;
    TArray<FOffgridAIFacePoseSample> EmotionSamples;
    ResolveVisemeTargets(VisemeSamples);
    if (bEnableEmotionExpression && EmotionExpression)
    {
        EmotionExpression->ResolvePoseSamples(EmotionSamples, bLogDiagnostics);
    }

    TimeSinceLastDiagnosticLog += DeltaTime;
    const bool bEmitDiagnosticThisFrame = bLogDiagnostics && TimeSinceLastDiagnosticLog >= FMath::Max(DiagnosticLogIntervalSeconds, 0.05f);
    if (bEmitDiagnosticThisFrame)
    {
        TimeSinceLastDiagnosticLog = 0.0f;
        if (EmotionExpression && (EmotionExpression->GetTargetEmotionName() != NAME_None || EmotionExpression->GetCurrentOverallWeight() > 0.001f))
        {
            UE_LOG(LogOffgridAI, Log,
                TEXT("[FaceDriver][Emotion] Tick target=%s target_magnitude=%.3f current_magnitude=%.3f target_overall=%.3f current_overall=%.3f resolved_samples=%d viseme_samples=%d"),
                *EmotionExpression->GetTargetEmotionName().ToString(),
                EmotionExpression->GetTargetMagnitude(),
                EmotionExpression->GetCurrentMagnitude(),
                EmotionExpression->GetTargetOverallWeight(),
                EmotionExpression->GetCurrentOverallWeight(),
                EmotionSamples.Num(),
                VisemeSamples.Num());
        }
    }

    TMap<FName, FOffgridAIFaceDriverControlValue> FinalControls;
    TMap<FName, FOffgridAIFaceDriverControlValue> TargetEmotionControls;
    DebugActivePoseWeights.Reset();
    for (const TPair<FName, float>& Pair : CurrentVisemeWeights)
    {
        if (Pair.Key != NAME_None && Pair.Value > KINDA_SMALL_NUMBER)
        {
            DebugActivePoseWeights.Add(Pair.Key, Clamp01(Pair.Value));
        }
    }

    FOffgridAIEmotionSpeechModifiers EmotionSpeechModifiers;
    if (EmotionExpression)
    {
        EmotionExpression->BuildControlTargets(EmotionSamples, EmotionStrength, TargetEmotionControls);
        EmotionSpeechModifiers = EmotionExpression->ResolveSpeechModifiers(bLogDiagnostics && TimeSinceLastDiagnosticLog >= DiagnosticLogIntervalSeconds);
        CurrentEmotionSpeechCriticalMouthScale = EmotionSpeechModifiers.SpeechCriticalMouthExpressionScale;
        CurrentEmotionSharedMouthScale = EmotionSpeechModifiers.SharedMouthExpressionScale;
        CurrentEmotionMouthCornerScale = EmotionSpeechModifiers.MouthCornerExpressionScale;
        CurrentEmotionMouthCornerBilabialScale = EmotionSpeechModifiers.MouthCornerBilabialExpressionScale;
    }
    TMap<FName, FOffgridAIFaceDriverControlValue> UpperFaceEmotionTargets;
    TMap<FName, FOffgridAIFaceDriverControlValue> LowerFaceEmotionTargets;
    for (const TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : TargetEmotionControls)
    {
        const bool bLowerFaceOrSpeechOwned =
            IsSpeechCriticalControl(Pair.Key) ||
            IsMouthCornerExpressionControl(Pair.Key) ||
            IsSharedMouthExpressionControl(Pair.Key);

        if (bLowerFaceOrSpeechOwned)
        {
            LowerFaceEmotionTargets.Add(Pair.Key, Pair.Value);
        }
        else
        {
            UpperFaceEmotionTargets.Add(Pair.Key, Pair.Value);
        }
    }

    SmoothControlMapToward(UpperFaceEmotionTargets, DeltaTime, ActiveEmotionBlendInSeconds, ActiveEmotionBlendOutSeconds, SmoothedEmotionControlValues);

    // The lower face is already busy carrying phonemes. Let emotional mouth
    // posture arrive at half the current emotion approach speed so a new
    // affect does not yank the lips away from a readable lipsync performance.
    const float LowerFaceEmotionBlendInSeconds = ActiveEmotionBlendInSeconds * 2.0f;
    const float LowerFaceEmotionBlendOutSeconds = ActiveEmotionBlendOutSeconds * 2.0f;
    SmoothControlMapToward(LowerFaceEmotionTargets, DeltaTime, LowerFaceEmotionBlendInSeconds, LowerFaceEmotionBlendOutSeconds, SmoothedLowerFaceEmotionControlValues);

    float SpeechActivity = 0.0f;
    float BilabialActivity = 0.0f;
    for (const TPair<FName, float>& Pair : CurrentVisemeWeights)
    {
        const float W = Clamp01(Pair.Value);
        SpeechActivity = FMath::Max(SpeechActivity, W);
        if (IsBilabialVisemePose(Pair.Key))
        {
            BilabialActivity = FMath::Max(BilabialActivity, W);
        }
    }
    UpdateEmotionMouthAllowance(DeltaTime, SpeechActivity);

    // Experiment C emotion/lipsync arbitration:
    // Upper-face emotion remains fully active during speech. Mouth emotion can
    // contribute lightly, but viseme controls remain authoritative for speech-
    // critical controls. After speech has quiesced for EmotionSpeechHoldSeconds,
    // EmotionMouthSilenceBlend ramps the full mouth pose back in over
    // EmotionMouthFadeInSeconds. When speech resumes, the allowance fades out
    // over EmotionMouthFadeOutSeconds so lipsync reclaims the mouth quickly.
    auto AddSmoothedEmotionControlsToFinal = [this, SpeechActivity, BilabialActivity, &FinalControls](const TMap<FName, FOffgridAIFaceDriverControlValue>& Controls)
    {
        for (const TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : Controls)
        {
            FOffgridAIFaceDriverControlValue Value = Pair.Value;
            const float EmotionControlScale = ComputeEmotionControlScaleDuringSpeech(Pair.Key, SpeechActivity, BilabialActivity);
            if (EmotionControlScale <= 0.001f)
            {
                continue;
            }
            if (EmotionControlScale < 0.999f)
            {
                if (Value.bIsVector2D)
                {
                    Value.Vector2DValue *= EmotionControlScale;
                }
                else
                {
                    Value.FloatValue *= EmotionControlScale;
                }
            }
            FinalControls.Add(Pair.Key, Value);
        }
    };

    AddSmoothedEmotionControlsToFinal(SmoothedEmotionControlValues);
    AddSmoothedEmotionControlsToFinal(SmoothedLowerFaceEmotionControlValues);

    TMap<FName, FOffgridAIFaceDriverControlValue> VisemeControls;
    for (const FOffgridAIFacePoseSample& Sample : VisemeSamples)
    {
        if (const FRuntimePose* Pose = VisemePoses.Find(Sample.PoseID))
        {
            AddWeightedControls(Pose->Controls, Sample.Weight * VisemeStrength, false, VisemeControls, Sample.PoseID);
            float& DebugW = DebugActivePoseWeights.FindOrAdd(Sample.PoseID);
            DebugW = FMath::Max(DebugW, Clamp01(Sample.Weight));
        }
    }
    ApplyEmotionSpeechModifiersToVisemes(VisemeControls, EmotionSpeechModifiers, SpeechActivity);

    // Visemes are authoritative over speech-critical controls. For non-speech
    // controls, keep additive composition so cheeks/nose/upper-face expression
    // can coexist with the mouth shapes.
    for (const TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : VisemeControls)
    {
        if (IsSpeechCriticalControl(Pair.Key))
        {
            FinalControls.FindOrAdd(Pair.Key) = Pair.Value;
        }
        else
        {
            FOffgridAIFaceDriverControlValue& Existing = FinalControls.FindOrAdd(Pair.Key);
            AccumulateControl(Existing, Pair.Value, 1.0f);
        }
    }

    PublishControlValues(FinalControls, DeltaTime);
}

bool UOffgridAIMetaHumanFaceDriverComponent::LoadVisemeLibrary(const FString& AbsolutePath)
{
    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *AbsolutePath))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[FaceDriver] failed to load viseme JSON: %s"), *AbsolutePath);
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[FaceDriver] failed to parse viseme JSON: %s"), *AbsolutePath);
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Visemes = nullptr;
    if (Root->TryGetArrayField(TEXT("visemes"), Visemes))
    {
        for (const TSharedPtr<FJsonValue>& Value : *Visemes)
        {
            const TSharedPtr<FJsonObject> Obj = Value->AsObject();
            if (!Obj.IsValid()) { continue; }
            FRuntimePose Pose;
            Pose.PoseID = FName(*ReadStringField(Obj, TEXT("id"), ReadStringField(Obj, TEXT("pose_name"))));
            const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
            if (Pose.PoseID != NAME_None && Obj->TryGetArrayField(TEXT("controls"), Controls) && ParseControlsArray(*Controls, Pose.Controls))
            {
                VisemePoses.Add(Pose.PoseID, Pose);
            }
            const TArray<TSharedPtr<FJsonValue>>* Aliases = nullptr;
            if (Obj->TryGetArrayField(TEXT("aliases"), Aliases))
            {
                for (const TSharedPtr<FJsonValue>& AliasValue : *Aliases)
                {
                    VisemeAliases.FindOrAdd(FName(*AliasValue->AsString())).AddUnique(Pose.PoseID);
                }
            }
        }
    }
    return VisemePoses.Num() > 0;
}

bool UOffgridAIMetaHumanFaceDriverComponent::ParseControlsArray(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FOffgridAIFaceDriverControlValue>& OutControls) const
{
    for (const TSharedPtr<FJsonValue>& Value : Values)
    {
        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid()) { continue; }
        FOffgridAIFaceDriverControlValue Control;
        Control.ControlName = FName(*ReadStringField(Obj, TEXT("name")));
        const FString Type = ReadStringField(Obj, TEXT("type")).ToLower();
        Control.bIsVector2D = Type == TEXT("vector2d");
        if (Control.bIsVector2D)
        {
            float X = ReadNumberField(Obj, TEXT("x"));
            float Y = ReadNumberField(Obj, TEXT("y"));
            const TSharedPtr<FJsonObject>* ValueObject = nullptr;
            if (Obj->TryGetObjectField(TEXT("value"), ValueObject) && ValueObject && ValueObject->IsValid())
            {
                X = ReadNumberField(*ValueObject, TEXT("x"), X);
                Y = ReadNumberField(*ValueObject, TEXT("y"), Y);
            }
            Control.Vector2DValue = FVector2D(X, Y);
        }
        else
        {
            Control.FloatValue = ReadNumberField(Obj, TEXT("value"));
        }
        if (Control.ControlName != NAME_None)
        {
            OutControls.Add(Control);
        }
    }
    return OutControls.Num() > 0;
}

void UOffgridAIMetaHumanFaceDriverComponent::AddWeightedControls(const TArray<FOffgridAIFaceDriverControlValue>& Controls, float Weight, bool bEmotionLayer, TMap<FName, FOffgridAIFaceDriverControlValue>& InOutValues, FName SourcePoseID) const
{
    const float SafeWeight = FMath::Clamp(Weight, -2.0f, 2.0f);
    for (const FOffgridAIFaceDriverControlValue& Control : Controls)
    {
        if (Control.ControlName == NAME_None) { continue; }
        float FinalWeight = SafeWeight;
        if (bEmotionLayer)
        {
            float SpeechWeight = 0.0f;
            float BilabialWeight = 0.0f;
            for (const TPair<FName, float>& Pair : CurrentVisemeWeights)
            {
                const float W = Clamp01(Pair.Value);
                SpeechWeight = FMath::Max(SpeechWeight, W);
                if (IsBilabialVisemePose(Pair.Key))
                {
                    BilabialWeight = FMath::Max(BilabialWeight, W);
                }
            }

            const float EmotionControlScale = ComputeEmotionControlScaleDuringSpeech(Control.ControlName, SpeechWeight, BilabialWeight);
            if (EmotionControlScale <= 0.001f)
            {
                continue;
            }
            FinalWeight *= EmotionControlScale;
        }
        if (!bEmotionLayer && IsTDSOrTHVisemePoseName(SourcePoseID) && IsJawOrOpenMouthControlName(Control.ControlName))
        {
            // Experiment D3 anti-flicker: tongue/TH/TDS accents should read as
            // tongue/teeth articulation, not as a tiny jaw-open pop. Preserve the
            // landmark pose and timing, but damp its jaw/open-mouth contribution.
            FinalWeight *= 0.35f;
        }

        FOffgridAIFaceDriverControlValue& Existing = InOutValues.FindOrAdd(Control.ControlName);
        if (!bEmotionLayer && IsSpeechCriticalControl(Control.ControlName))
        {
            // Viseme mouth/jaw/lip/tongue controls are alternative articulations,
            // not additive facial expressions. If simultaneous vowels each add
            // their jaw controls, the line progressively drifts open and muddy.
            // Compose speech-critical viseme controls by dominant component; keep
            // additive composition for non-speech detail controls and emotions.
            ApplyDominantControlValue(Existing, Control, FinalWeight);
        }
        else
        {
            AccumulateControl(Existing, Control, FinalWeight);
        }
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::ResolveVisemeTargets(TArray<FOffgridAIFacePoseSample>& OutSamples) const
{
    for (const TPair<FName, float>& Pair : CurrentVisemeWeights)
    {
        const float Weight = Clamp01(Pair.Value);
        if (Weight <= 0.001f) { continue; }
        if (VisemePoses.Contains(Pair.Key))
        {
            OutSamples.Add(MakePoseSample(Pair.Key, Weight));
        }
        else if (const TArray<FName>* PoseIDs = VisemeAliases.Find(Pair.Key))
        {
            if (PoseIDs->Num() > 0)
            {
                OutSamples.Add(MakePoseSample((*PoseIDs)[0], Weight));
            }
        }
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::SmoothControlMapToward(
    const TMap<FName, FOffgridAIFaceDriverControlValue>& TargetControls,
    float DeltaTimeSeconds,
    float BlendInSeconds,
    float BlendOutSeconds,
    TMap<FName, FOffgridAIFaceDriverControlValue>& InOutSmoothedControls) const
{
    TSet<FName> Keys;
    for (const TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : TargetControls)
    {
        Keys.Add(Pair.Key);
    }
    for (const TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : InOutSmoothedControls)
    {
        Keys.Add(Pair.Key);
    }

    auto BlendSigned = [](float Current, float TargetValue, float Dt, float InSeconds, float OutSeconds)
    {
        const float Tau = FMath::Max(FMath::Abs(TargetValue) > FMath::Abs(Current) ? InSeconds : OutSeconds, 0.001f);
        const float Alpha = FMath::Clamp(1.0f - FMath::Exp(-FMath::Max(Dt, 0.0f) / Tau), 0.0f, 1.0f);
        return FMath::Lerp(Current, TargetValue, Alpha);
    };

    for (const FName& Key : Keys)
    {
        const FOffgridAIFaceDriverControlValue* TargetPtr = TargetControls.Find(Key);
        const FOffgridAIFaceDriverControlValue* CurrentPtr = InOutSmoothedControls.Find(Key);

        FOffgridAIFaceDriverControlValue Target;
        if (TargetPtr)
        {
            Target = *TargetPtr;
        }
        else if (CurrentPtr)
        {
            Target = *CurrentPtr;
            Target.FloatValue = 0.0f;
            Target.Vector2DValue = FVector2D::ZeroVector;
        }
        else
        {
            continue;
        }

        Target.ControlName = Key;

        FOffgridAIFaceDriverControlValue Next = Target;
        if (CurrentPtr && CurrentPtr->bIsVector2D == Target.bIsVector2D)
        {
            Next = *CurrentPtr;
            Next.ControlName = Key;
            Next.bIsVector2D = Target.bIsVector2D;
            if (Target.bIsVector2D)
            {
                Next.Vector2DValue.X = BlendSigned(CurrentPtr->Vector2DValue.X, Target.Vector2DValue.X, DeltaTimeSeconds, BlendInSeconds, BlendOutSeconds);
                Next.Vector2DValue.Y = BlendSigned(CurrentPtr->Vector2DValue.Y, Target.Vector2DValue.Y, DeltaTimeSeconds, BlendInSeconds, BlendOutSeconds);
                Next.FloatValue = 0.0f;
            }
            else
            {
                Next.FloatValue = BlendSigned(CurrentPtr->FloatValue, Target.FloatValue, DeltaTimeSeconds, BlendInSeconds, BlendOutSeconds);
                Next.Vector2DValue = FVector2D::ZeroVector;
            }
        }

        const bool bNonZero = Next.bIsVector2D
            ? !Next.Vector2DValue.IsNearlyZero(0.0005f)
            : !FMath::IsNearlyZero(Next.FloatValue, 0.0005f);

        if (bNonZero || TargetPtr)
        {
            InOutSmoothedControls.FindOrAdd(Key) = Next;
        }
        else
        {
            InOutSmoothedControls.Remove(Key);
        }
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::PublishControlValues(const TMap<FName, FOffgridAIFaceDriverControlValue>& TargetControls, float DeltaTimeSeconds, bool bImmediate)
{
    TSet<FName> Keys;
    for (const TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : TargetControls)
    {
        Keys.Add(Pair.Key);
    }
    for (const TPair<FName, FOffgridAIFaceDriverControlValue>& Pair : SmoothedControlValues)
    {
        Keys.Add(Pair.Key);
    }

    TArray<FOffgridAIFaceDriverControlValue> NewValues;
    NewValues.Reserve(Keys.Num());

    for (const FName& Key : Keys)
    {
        const FOffgridAIFaceDriverControlValue* TargetPtr = TargetControls.Find(Key);
        const FOffgridAIFaceDriverControlValue* CurrentPtr = SmoothedControlValues.Find(Key);

        FOffgridAIFaceDriverControlValue Target;
        if (TargetPtr)
        {
            Target = *TargetPtr;
        }
        else if (CurrentPtr)
        {
            // Preserve the known type while releasing absent controls toward neutral.
            Target = *CurrentPtr;
            Target.FloatValue = 0.0f;
            Target.Vector2DValue = FVector2D::ZeroVector;
        }
        else
        {
            continue;
        }

        Target.ControlName = Key;
        if (Target.bIsVector2D)
        {
            Target.Vector2DValue.X = FMath::Clamp(Target.Vector2DValue.X, -2.0f, 2.0f);
            Target.Vector2DValue.Y = FMath::Clamp(Target.Vector2DValue.Y, -2.0f, 2.0f);
        }
        else
        {
            Target.FloatValue = FMath::Clamp(Target.FloatValue, -2.0f, 2.0f);
        }

        FOffgridAIFaceDriverControlValue Next = Target;
        if (!bImmediate && CurrentPtr && CurrentPtr->bIsVector2D == Target.bIsVector2D)
        {
            Next = *CurrentPtr;
            Next.ControlName = Key;
            Next.bIsVector2D = Target.bIsVector2D;

            // Mouth/jaw controls should only use the fast viseme time constants while
            // a viseme layer is actually active. Resting/line emotion changes can also
            // touch mouth controls; using 35 ms viseme easing for those emotion-only
            // family swaps reads as a visible pop.
            const bool bVisemeLayerActive = TargetVisemeWeights.Num() > 0 || CurrentVisemeWeights.Num() > 0;
            const bool bActiveSpeechTargets = TargetVisemeWeights.Num() > 0;
            const bool bSpeechCritical = bVisemeLayerActive && IsSpeechCriticalControl(Key);
            const float InSeconds = bSpeechCritical
                ? (bActiveSpeechTargets ? 0.006f : VisemeBlendInSeconds)
                : EmotionBlendInSeconds;
            const float OutSeconds = bSpeechCritical
                ? (bActiveSpeechTargets ? 0.014f : GetPostSpeechVisemeReleaseSeconds(VisemeBlendOutSeconds))
                : EmotionBlendOutSeconds;

            auto BlendSigned = [](float Current, float TargetValue, float Dt, float BlendInSeconds, float BlendOutSeconds)
            {
                const float Tau = FMath::Max(FMath::Abs(TargetValue) > FMath::Abs(Current) ? BlendInSeconds : BlendOutSeconds, 0.001f);
                const float Alpha = FMath::Clamp(1.0f - FMath::Exp(-FMath::Max(Dt, 0.0f) / Tau), 0.0f, 1.0f);
                return FMath::Lerp(Current, TargetValue, Alpha);
            };

            if (Target.bIsVector2D)
            {
                Next.Vector2DValue.X = BlendSigned(CurrentPtr->Vector2DValue.X, Target.Vector2DValue.X, DeltaTimeSeconds, InSeconds, OutSeconds);
                Next.Vector2DValue.Y = BlendSigned(CurrentPtr->Vector2DValue.Y, Target.Vector2DValue.Y, DeltaTimeSeconds, InSeconds, OutSeconds);
                Next.FloatValue = 0.0f;
            }
            else
            {
                Next.FloatValue = BlendSigned(CurrentPtr->FloatValue, Target.FloatValue, DeltaTimeSeconds, InSeconds, OutSeconds);
                Next.Vector2DValue = FVector2D::ZeroVector;
            }

            if (bActiveSpeechTargets && bSpeechCritical && TargetPtr)
            {
                ApplyActiveSpeechControlRateLimit(Key, *CurrentPtr, Next, DeltaTimeSeconds);
            }
        }

        const bool bNonZero = Next.bIsVector2D
            ? !Next.Vector2DValue.IsNearlyZero(0.0005f)
            : !FMath::IsNearlyZero(Next.FloatValue, 0.0005f);

        if (bNonZero || TargetPtr)
        {
            SmoothedControlValues.FindOrAdd(Key) = Next;
            NewValues.Add(Next);
        }
        else
        {
            SmoothedControlValues.Remove(Key);
        }
    }

    NewValues.Sort([](const FOffgridAIFaceDriverControlValue& A, const FOffgridAIFaceDriverControlValue& B)
    {
        return A.ControlName.LexicalLess(B.ControlName);
    });

    const bool bChanged = ControlValuesChanged(LatestControlValues, NewValues);
    LatestControlValues = MoveTemp(NewValues);

    FOffgridAIMetaHumanFacePose NewFacePose;
    for (const FOffgridAIFaceDriverControlValue& Value : LatestControlValues)
    {
        ProjectControlValueToFacePose(Value, NewFacePose);
    }
    LatestFacePose = NewFacePose;

    // Always broadcast while there are active controls; also broadcast the final
    // all-zero pose when the smoothed controls finish releasing. This prevents
    // stale jaw/mouth values from remaining in the AnimBP after a line ends.
    if (bChanged || LatestControlValues.Num() > 0 || SmoothedControlValues.Num() == 0)
    {
        OnControlsUpdated.Broadcast(LatestControlValues);
        ReceiveControlsUpdated(LatestControlValues);
        OnFacePoseUpdated.Broadcast(LatestFacePose);
        ReceiveFacePoseUpdated(LatestFacePose);
    }

    if (bLogDiagnostics)
    {
        TimeSinceLastDiagnosticLog += GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f;
        if (TimeSinceLastDiagnosticLog >= DiagnosticLogIntervalSeconds)
        {
            TimeSinceLastDiagnosticLog = 0.0f;
            FString FirstControls;
            const int32 Limit = FMath::Min(LatestControlValues.Num(), 6);
            for (int32 Index = 0; Index < Limit; ++Index)
            {
                const FOffgridAIFaceDriverControlValue& Value = LatestControlValues[Index];
                FirstControls += FString::Printf(TEXT("%s=%s%.2f "),
                    *Value.ControlName.ToString(),
                    Value.bIsVector2D ? TEXT("v") : TEXT(""),
                    Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue);
            }
            UE_LOG(LogOffgridAI, Log,
                TEXT("[FaceDriver] publish controls=%d smoothed=%d active_poses=%d emotion=%s emotion_current=%.3f sample=[%s]"),
                LatestControlValues.Num(),
                SmoothedControlValues.Num(),
                DebugActivePoseWeights.Num(),
                EmotionExpression ? *EmotionExpression->GetTargetEmotionName().ToString() : TEXT("<none>"),
                EmotionExpression ? EmotionExpression->GetCurrentOverallWeight() : 0.0f,
                *FirstControls);
        }
    }
}

void UOffgridAIMetaHumanFaceDriverComponent::PublishNeutralZeros()
{
    TMap<FName, FOffgridAIFaceDriverControlValue> Zeros;
    auto AddZeros = [&Zeros](const TArray<FOffgridAIFaceDriverControlValue>& Controls)
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
            Zeros.FindOrAdd(Zero.ControlName) = Zero;
        }
    };

    for (const TPair<FName, FRuntimePose>& Pair : VisemePoses)
    {
        AddZeros(Pair.Value.Controls);
    }
    if (EmotionExpression)
    {
        EmotionExpression->AppendNeutralControls(Zeros);
    }

    PublishControlValues(Zeros, 0.0f, true);
}


void UOffgridAIMetaHumanFaceDriverComponent::ProjectControlValueToFacePose(const FOffgridAIFaceDriverControlValue& Value, FOffgridAIMetaHumanFacePose& InOutPose) const
{
    const FName Name = Value.ControlName;

    if (Name == FName(TEXT("CTRL_C_jaw"))) { InOutPose.CTRL_C_jaw = Value.bIsVector2D ? Value.Vector2DValue : FVector2D(Value.FloatValue, Value.FloatValue); return; }
    if (Name == TEXT("CTRL_C_jaw_fwdBack")) { InOutPose.CTRL_C_jaw_fwdBack = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_C_tongue") || Name == TEXT("CTRL_C_tongue_move") || Name == TEXT("CTRL_C_tongueMove")) { InOutPose.CTRL_C_tongue = Value.bIsVector2D ? Value.Vector2DValue : FVector2D(Value.FloatValue, Value.FloatValue); return; }
    if (Name == TEXT("CTRL_C_tongue_inOut")) { InOutPose.CTRL_C_tongue_inOut = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_C_tongue_press")) { InOutPose.CTRL_C_tongue_press = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_brow_down")) { InOutPose.CTRL_L_brow_down = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_brow_lateral")) { InOutPose.CTRL_L_brow_lateral = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_brow_raiseIn")) { InOutPose.CTRL_L_brow_raiseIn = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_brow_raiseOut")) { InOutPose.CTRL_L_brow_raiseOut = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_eye_blink")) { InOutPose.CTRL_L_eye_blink = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_eye_cheekRaise")) { InOutPose.CTRL_L_eye_cheekRaise = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_eye_squintInner")) { InOutPose.CTRL_L_eye_squintInner = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_jaw_ChinRaiseD")) { InOutPose.CTRL_L_jaw_ChinRaiseD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_jaw_ChinRaiseU")) { InOutPose.CTRL_L_jaw_ChinRaiseU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_jaw_chinCompress")) { InOutPose.CTRL_L_jaw_chinCompress = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_jaw_clench")) { InOutPose.CTRL_L_jaw_clench = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == FName(TEXT("CTRL_L_mouth_cornerDepress"))) { InOutPose.CTRL_L_mouth_cornerDepress = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_cornerPull")) { InOutPose.CTRL_L_mouth_cornerPull = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_cornerSharpnessD")) { InOutPose.CTRL_L_mouth_cornerSharpnessD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_cornerSharpnessU")) { InOutPose.CTRL_L_mouth_cornerSharpnessU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_dimple")) { InOutPose.CTRL_L_mouth_dimple = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_funnelD")) { InOutPose.CTRL_L_mouth_funnelD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_funnelU")) { InOutPose.CTRL_L_mouth_funnelU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_lipBiteD")) { InOutPose.CTRL_L_mouth_lipBiteD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_lipsPressU")) { InOutPose.CTRL_L_mouth_lipsPressU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_lipsRollD")) { InOutPose.CTRL_L_mouth_lipsRollD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_lipsRollU")) { InOutPose.CTRL_L_mouth_lipsRollU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_lipsTogetherD")) { InOutPose.CTRL_L_mouth_lipsTogetherD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_lipsTogetherU")) { InOutPose.CTRL_L_mouth_lipsTogetherU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_lowerLipDepress")) { InOutPose.CTRL_L_mouth_lowerLipDepress = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_pressD")) { InOutPose.CTRL_L_mouth_pressD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_pressU")) { InOutPose.CTRL_L_mouth_pressU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_purseD")) { InOutPose.CTRL_L_mouth_purseD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_purseU")) { InOutPose.CTRL_L_mouth_purseU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_pushPullD")) { InOutPose.CTRL_L_mouth_pushPullD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_pushPullU")) { InOutPose.CTRL_L_mouth_pushPullU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_sharpCornerPull")) { InOutPose.CTRL_L_mouth_sharpCornerPull = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_stretch")) { InOutPose.CTRL_L_mouth_stretch = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_thicknessD")) { InOutPose.CTRL_L_mouth_thicknessD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_thicknessU")) { InOutPose.CTRL_L_mouth_thicknessU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_tightenD")) { InOutPose.CTRL_L_mouth_tightenD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_tightenU")) { InOutPose.CTRL_L_mouth_tightenU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_towardsD")) { InOutPose.CTRL_L_mouth_towardsD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_mouth_towardsU")) { InOutPose.CTRL_L_mouth_towardsU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == FName(TEXT("CTRL_L_mouth_upperLipRaise"))) { InOutPose.CTRL_L_mouth_upperLipRaise = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_neck_mastoidContract")) { InOutPose.CTRL_L_neck_mastoidContract = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_neck_stretch")) { InOutPose.CTRL_L_neck_stretch = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_nose")) { InOutPose.CTRL_L_nose = Value.bIsVector2D ? Value.Vector2DValue : FVector2D(Value.FloatValue, Value.FloatValue); return; }
    if (Name == TEXT("CTRL_L_nose_nasolabialDeepen")) { InOutPose.CTRL_L_nose_nasolabialDeepen = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_L_nose_wrinkleUpper")) { InOutPose.CTRL_L_nose_wrinkleUpper = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_brow_down")) { InOutPose.CTRL_R_brow_down = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_brow_lateral")) { InOutPose.CTRL_R_brow_lateral = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_brow_raiseIn")) { InOutPose.CTRL_R_brow_raiseIn = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_brow_raiseOut")) { InOutPose.CTRL_R_brow_raiseOut = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_eye_blink")) { InOutPose.CTRL_R_eye_blink = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_eye_cheekRaise")) { InOutPose.CTRL_R_eye_cheekRaise = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_eye_squintInner")) { InOutPose.CTRL_R_eye_squintInner = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_jaw_ChinRaiseD")) { InOutPose.CTRL_R_jaw_ChinRaiseD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_jaw_ChinRaiseU")) { InOutPose.CTRL_R_jaw_ChinRaiseU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_jaw_chinCompress")) { InOutPose.CTRL_R_jaw_chinCompress = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_jaw_clench")) { InOutPose.CTRL_R_jaw_clench = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == FName(TEXT("CTRL_R_mouth_cornerDepress"))) { InOutPose.CTRL_R_mouth_cornerDepress = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_cornerPull")) { InOutPose.CTRL_R_mouth_cornerPull = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_cornerSharpnessD")) { InOutPose.CTRL_R_mouth_cornerSharpnessD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_cornerSharpnessU")) { InOutPose.CTRL_R_mouth_cornerSharpnessU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_dimple")) { InOutPose.CTRL_R_mouth_dimple = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_funnelD")) { InOutPose.CTRL_R_mouth_funnelD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_funnelU")) { InOutPose.CTRL_R_mouth_funnelU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_lipBiteD")) { InOutPose.CTRL_R_mouth_lipBiteD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_lipsPressU")) { InOutPose.CTRL_R_mouth_lipsPressU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_lipsRollD")) { InOutPose.CTRL_R_mouth_lipsRollD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_lipsRollU")) { InOutPose.CTRL_R_mouth_lipsRollU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_lipsTogetherD")) { InOutPose.CTRL_R_mouth_lipsTogetherD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_lipsTogetherU")) { InOutPose.CTRL_R_mouth_lipsTogetherU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_lowerLipDepress")) { InOutPose.CTRL_R_mouth_lowerLipDepress = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_pressD")) { InOutPose.CTRL_R_mouth_pressD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_pressU")) { InOutPose.CTRL_R_mouth_pressU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_purseD")) { InOutPose.CTRL_R_mouth_purseD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_purseU")) { InOutPose.CTRL_R_mouth_purseU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_pushPullD")) { InOutPose.CTRL_R_mouth_pushPullD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_pushPullU")) { InOutPose.CTRL_R_mouth_pushPullU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_sharpCornerPull")) { InOutPose.CTRL_R_mouth_sharpCornerPull = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_stretch")) { InOutPose.CTRL_R_mouth_stretch = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_thicknessD")) { InOutPose.CTRL_R_mouth_thicknessD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_thicknessU")) { InOutPose.CTRL_R_mouth_thicknessU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_tightenD")) { InOutPose.CTRL_R_mouth_tightenD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_tightenU")) { InOutPose.CTRL_R_mouth_tightenU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_towardsD")) { InOutPose.CTRL_R_mouth_towardsD = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_mouth_towardsU")) { InOutPose.CTRL_R_mouth_towardsU = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == FName(TEXT("CTRL_R_mouth_upperLipRaise"))) { InOutPose.CTRL_R_mouth_upperLipRaise = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_neck_mastoidContract")) { InOutPose.CTRL_R_neck_mastoidContract = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_neck_stretch")) { InOutPose.CTRL_R_neck_stretch = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_nose")) { InOutPose.CTRL_R_nose = Value.bIsVector2D ? Value.Vector2DValue : FVector2D(Value.FloatValue, Value.FloatValue); return; }
    if (Name == TEXT("CTRL_R_nose_nasolabialDeepen")) { InOutPose.CTRL_R_nose_nasolabialDeepen = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
    if (Name == TEXT("CTRL_R_nose_wrinkleUpper")) { InOutPose.CTRL_R_nose_wrinkleUpper = Value.bIsVector2D ? Value.Vector2DValue.Size() : Value.FloatValue; return; }
}

float UOffgridAIMetaHumanFaceDriverComponent::StepBlend(float Current, float Target, float DeltaTime, float InSeconds, float OutSeconds) const
{
    const float Tau = FMath::Max(Target > Current ? InSeconds : OutSeconds, 0.001f);
    const float Alpha = Clamp01(1.0f - FMath::Exp(-FMath::Max(DeltaTime, 0.0f) / Tau));
    return Clamp01(FMath::Lerp(Current, Target, Alpha));
}

bool UOffgridAIMetaHumanFaceDriverComponent::IsMouthCornerExpressionControl(FName ControlName) const
{
    const FString S = ControlName.ToString().ToLower();
    return S.Contains(TEXT("mouth_corner"))
        || S.Contains(TEXT("cornerpull"))
        || S.Contains(TEXT("cornerdepress"))
        || S.Contains(TEXT("cornerstretch"))
        || S.Contains(TEXT("corner_sharpness"))
        || S.Contains(TEXT("corner"))
        || S.Contains(TEXT("mouth_stretch"))
        || S.Contains(TEXT("mouthstretch"));
}

bool UOffgridAIMetaHumanFaceDriverComponent::IsSharedMouthExpressionControl(FName ControlName) const
{
    const FString S = ControlName.ToString().ToLower();
    return IsMouthCornerExpressionControl(ControlName)
        || S.Contains(TEXT("lip_press"))
        || S.Contains(TEXT("lippress"))
        || S.Contains(TEXT("lip_tight"))
        || S.Contains(TEXT("liptight"))
        || S.Contains(TEXT("mouth_dimple"))
        || S.Contains(TEXT("mouthdimple"))
        || S.Contains(TEXT("mouth_shrug"))
        || S.Contains(TEXT("mouthshrug"))
        || S.Contains(TEXT("cheek"));
}

void UOffgridAIMetaHumanFaceDriverComponent::UpdateEmotionMouthAllowance(float DeltaTimeSeconds, float SpeechActivity)
{
    const bool bSpeechActive = bLineSpeechMouthSuppressionActive || Clamp01(SpeechActivity) > 0.045f;
    if (bSpeechActive)
    {
        EmotionSecondsSinceSpeechActivity = 0.0f;
    }
    else
    {
        EmotionSecondsSinceSpeechActivity = FMath::Min(EmotionSecondsSinceSpeechActivity + FMath::Max(DeltaTimeSeconds, 0.0f), 999.0f);
    }

    const float Hold = FMath::Max(EmotionSpeechHoldSeconds, 0.0f);
    const float FullAt = FMath::Max(EmotionFullMouthAfterSilenceSeconds, Hold + 0.001f);
    const float DesiredBlend = bSpeechActive
        ? 0.0f
        : FMath::Clamp((EmotionSecondsSinceSpeechActivity - Hold) / (FullAt - Hold), 0.0f, 1.0f);

    const float BlendSeconds = DesiredBlend > EmotionMouthSilenceBlend
        ? FMath::Max(EmotionMouthFadeInSeconds, 0.001f)
        : FMath::Max(EmotionMouthFadeOutSeconds, 0.001f);
    EmotionMouthSilenceBlend = StepBlend(EmotionMouthSilenceBlend, DesiredBlend, DeltaTimeSeconds, BlendSeconds, BlendSeconds);
}

bool UOffgridAIMetaHumanFaceDriverComponent::IsBilabialVisemePose(FName PoseID) const
{
    const FString S = PoseID.ToString().ToLower();
    return S.Contains(TEXT("mbp")) || S.Contains(TEXT("closed")) || S.Contains(TEXT("22_"));
}

float UOffgridAIMetaHumanFaceDriverComponent::ComputeEmotionControlScaleDuringSpeech(FName ControlName, float SpeechActivity, float BilabialActivity) const
{
    // Upper face stays fully expressive. Mouth-related emotion is time-smoothed:
    // it contributes during speech, remains held through intra-line gaps, and only
    // returns to full-strength mouth expression through EmotionMouthSilenceBlend.
    if (!IsSpeechCriticalControl(ControlName))
    {
        return 1.0f;
    }

    const float Speech = bLineSpeechMouthSuppressionActive ? 1.0f : Clamp01(SpeechActivity);
    const bool bSpeechActive = bLineSpeechMouthSuppressionActive || Speech > 0.001f;

    const float CriticalDuringSpeech = FMath::Clamp(CurrentEmotionSpeechCriticalMouthScale, 0.0f, 1.0f);
    const float SharedDuringSpeech = FMath::Clamp(CurrentEmotionSharedMouthScale, 0.0f, 1.0f);
    const float CornerDuringSpeech = FMath::Clamp(CurrentEmotionMouthCornerScale, 0.0f, 1.0f);
    const float BilabialCornerScale = FMath::Clamp(CurrentEmotionMouthCornerBilabialScale, 0.0f, 1.0f);
    const float SilenceBlend = FMath::Clamp(EmotionMouthSilenceBlend, 0.0f, 1.0f);

    float DuringSpeechScale = CriticalDuringSpeech;
    if (IsMouthCornerExpressionControl(ControlName))
    {
        DuringSpeechScale = FMath::Lerp(CornerDuringSpeech, BilabialCornerScale, Clamp01(BilabialActivity));
    }
    else if (IsSharedMouthExpressionControl(ControlName))
    {
        DuringSpeechScale = SharedDuringSpeech;
    }

    if (!bSpeechActive)
    {
        // This is the important anti-pop path. The previous implementation
        // effectively returned 1.0 as soon as speech activity hit zero, so lower
        // face emotion snapped from the speech-limited value to the full peak.
        return FMath::Clamp(FMath::Lerp(DuringSpeechScale, 1.0f, SilenceBlend), 0.0f, 1.0f);
    }

    const float SpeechScaled = FMath::Lerp(1.0f, DuringSpeechScale, Speech);
    return FMath::Clamp(FMath::Lerp(SpeechScaled, 1.0f, SilenceBlend), 0.0f, 1.0f);
}

bool UOffgridAIMetaHumanFaceDriverComponent::IsSpeechCriticalControl(FName ControlName) const
{
    const FString S = ControlName.ToString().ToLower();
    return S.Contains(TEXT("mouth")) || S.Contains(TEXT("jaw")) || S.Contains(TEXT("tongue")) || S.Contains(TEXT("lip"));
}

bool UOffgridAIMetaHumanFaceDriverComponent::ControlValuesChanged(const TArray<FOffgridAIFaceDriverControlValue>& A, const TArray<FOffgridAIFaceDriverControlValue>& B) const
{
    if (A.Num() != B.Num())
    {
        return true;
    }
    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        if (A[Index].ControlName != B[Index].ControlName || A[Index].bIsVector2D != B[Index].bIsVector2D)
        {
            return true;
        }
        if (A[Index].bIsVector2D)
        {
            if (!A[Index].Vector2DValue.Equals(B[Index].Vector2DValue, 0.0005f))
            {
                return true;
            }
        }
        else if (!FMath::IsNearlyEqual(A[Index].FloatValue, B[Index].FloatValue, 0.0005f))
        {
            return true;
        }
    }
    return false;
}
