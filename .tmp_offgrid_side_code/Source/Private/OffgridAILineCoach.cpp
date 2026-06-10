#include "OffgridAILineCoach.h"
#include "Lipsync/OffgridAIArticulationBudgeter.h"
#include "Lipsync/OffgridAILipsyncRuntimeAdapter.h"
#include "Lipsync/OffgridAIAudioOccupancyScheduler.h"
#include "Lipsync/OffgridAIVisemePerformer.h"

#include "OffgridAI.h"

#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"

#include "Core/OffgridAIOrchestrator.h"
#include "Data/OffgridAILineCoachAudioSettingsDataAsset.h"
#include "Data/OffgridAIEmotionSettingsDataAsset.h"
#include "Data/OffgridAILipsyncSettingsDataAsset.h"
#include "Lipsync/OffgridAITextVisemePlanner.h"
#include "OffgridAIMetaHumanFaceDriverComponent.h"


namespace
{
    FString GOffgridAILipsyncDebugSessionPath;
    bool bGOffgridAILipsyncDebugSessionInitialized = false;

    // v19: submitted pose provenance is event-instance based.
    // Do not store source timing in pose-keyed sidecars; repeated PoseIDs are common.

    static FString SanitizeDebugFilenameToken(const FString& In)
    {
        FString Out = In;
        const TCHAR* InvalidChars = TEXT("\\/:*?\"<>| .");
        for (const TCHAR* Char = InvalidChars; *Char; ++Char)
        {
            Out.ReplaceCharInline(*Char, TCHAR('_'));
        }
        return Out.IsEmpty() ? FString(TEXT("None")) : Out;
    }

    static FString EscapeDebugCSVString(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("\""), TEXT("\"\""));
        return FString::Printf(TEXT("\"%s\""), *Out);
    }

    static FString SerializeDebugPoseWeights(const TMap<FName, float>& Weights)
    {
        TArray<FString> Parts;
        Parts.Reserve(Weights.Num());
        for (const TPair<FName, float>& Pair : Weights)
        {
            if (Pair.Value > 0.0005f)
            {
                Parts.Add(FString::Printf(TEXT("%s=%.3f"), *Pair.Key.ToString(), Pair.Value));
            }
        }
        Parts.Sort();
        return FString::Join(Parts, TEXT(";"));
    }

    static float GetDebugPoseWeightByAlias(const TMap<FName, float>& Weights, FName A, FName B = NAME_None)
    {
        float Out = Weights.FindRef(A);
        if (B != NAME_None)
        {
            Out = FMath::Max(Out, Weights.FindRef(B));
        }
        return FMath::Clamp(Out, 0.0f, 1.0f);
    }

    static void GetDebugDominantPose(const TMap<FName, float>& Weights, FName& OutPose, float& OutWeight)
    {
        OutPose = NAME_None;
        OutWeight = 0.0f;
        for (const TPair<FName, float>& Pair : Weights)
        {
            if (Pair.Value > OutWeight)
            {
                OutPose = Pair.Key;
                OutWeight = Pair.Value;
            }
        }
    }

    static FString DebugFamilyForPose(FName PoseIDOrAlias)
    {
        const FString S = PoseIDOrAlias.ToString();
        if (S.Contains(TEXT("Tongue"), ESearchCase::IgnoreCase) || S.Contains(TEXT("TDS"), ESearchCase::IgnoreCase) || S.Contains(TEXT("KGY"), ESearchCase::IgnoreCase)) return TEXT("Tongue");
        if (S.Equals(TEXT("MBP"), ESearchCase::IgnoreCase) || S.Contains(TEXT("MBP"))) return TEXT("Closed");
        if (S.Equals(TEXT("AAA"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Aa")) || S.Contains(TEXT("Ah")) || S.Contains(TEXT("Oh")) || S.Contains(TEXT("Uh"))) return TEXT("Open");
        if (S.Equals(TEXT("EEE"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ee")) || S.Contains(TEXT("Ih")) || S.Contains(TEXT("Ay")) || S.Contains(TEXT("Eh"))) return TEXT("Wide");
        if (S.Equals(TEXT("OOO"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Oo")) || S.Contains(TEXT("Or")) || S.Contains(TEXT("Rr"))) return TEXT("Round");
        if (S.Equals(TEXT("WUH"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ww")) || S.Contains(TEXT("Ew"))) return TEXT("Funnel");
        if (S.Equals(TEXT("FVS"), ESearchCase::IgnoreCase) || S.Contains(TEXT("FV"))) return TEXT("Teeth");
        return TEXT("Other");
    }

    static bool DebugPoseMatchesFamily(FName PoseIDOrAlias, const TCHAR* Family)
    {
        const FString S = PoseIDOrAlias.ToString();
        const FString F(Family);
        if (F.Equals(TEXT("Tongue"), ESearchCase::IgnoreCase)) return S.Contains(TEXT("Tongue"), ESearchCase::IgnoreCase) || S.Contains(TEXT("TDS"), ESearchCase::IgnoreCase) || S.Contains(TEXT("KGY"), ESearchCase::IgnoreCase);
        if (F.Equals(TEXT("Closed"), ESearchCase::IgnoreCase)) return S.Equals(TEXT("MBP"), ESearchCase::IgnoreCase) || S.Contains(TEXT("MBP"));
        if (F.Equals(TEXT("Open"), ESearchCase::IgnoreCase)) return S.Equals(TEXT("AAA"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Aa")) || S.Contains(TEXT("Ah")) || S.Contains(TEXT("Oh")) || S.Contains(TEXT("Uh"));
        if (F.Equals(TEXT("Wide"), ESearchCase::IgnoreCase)) return S.Equals(TEXT("EEE"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ee")) || S.Contains(TEXT("Ih")) || S.Contains(TEXT("Ay")) || S.Contains(TEXT("Eh"));
        if (F.Equals(TEXT("Round"), ESearchCase::IgnoreCase)) return S.Equals(TEXT("OOO"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Oh")) || S.Contains(TEXT("Oo")) || S.Contains(TEXT("Or")) || S.Contains(TEXT("Rr"));
        if (F.Equals(TEXT("Funnel"), ESearchCase::IgnoreCase)) return S.Equals(TEXT("WUH"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ww")) || S.Contains(TEXT("Ew"));
        if (F.Equals(TEXT("Teeth"), ESearchCase::IgnoreCase)) return S.Equals(TEXT("FVS"), ESearchCase::IgnoreCase) || S.Contains(TEXT("FV"));
        return false;
    }

    static float GetDebugPoseFamilyWeight(const TMap<FName, float>& Weights, const TCHAR* Family)
    {
        float Out = 0.0f;
        for (const TPair<FName, float>& Pair : Weights)
        {
            if (DebugPoseMatchesFamily(Pair.Key, Family))
            {
                Out = FMath::Max(Out, Pair.Value);
            }
        }
        return FMath::Clamp(Out, 0.0f, 1.0f);
    }

    static FString TextVisemeEnvelopeDebugName(EOffgridAITextViseme Viseme)
    {
        switch (Viseme)
        {
        case EOffgridAITextViseme::MBP: return TEXT("AHR_FAST_CLOSURE");
        case EOffgridAITextViseme::AAA: return TEXT("AHR_OPEN_VOWEL");
        case EOffgridAITextViseme::EEE: return TEXT("AHR_WIDE_VOWEL");
        case EOffgridAITextViseme::OOO: return TEXT("AHR_ROUND_VOWEL");
        case EOffgridAITextViseme::WUH: return TEXT("AHR_FUNNEL_ACCENT");
        case EOffgridAITextViseme::FVS: return TEXT("AHR_TEETH_ACCENT");
        case EOffgridAITextViseme::Rest: return TEXT("DETAIL_OR_TRANSITION");
        default: return TEXT("UNKNOWN");
        }
    }


    struct FOffgridAIDebugMotionEnvelopeProfile
    {
        const TCHAR* Name = TEXT("DETAIL_SOFT");
        float AttackLeadSeconds = 0.035f;
        float MinPeakHoldSeconds = 0.035f;
        float ReleaseTailSeconds = 0.060f;
        float PeakStrengthMultiplier = 0.86f;
        float OvershootAmount = 0.00f;
        float JawTarget = 0.20f;
        float RoundTarget = 0.00f;
        float TeethTarget = 0.00f;
        float MaxJawVelocityPerSecond = 7.0f;
        bool bVowel = false;
        bool bClosure = false;
        bool bFricative = false;
    };

    static FOffgridAIDebugMotionEnvelopeProfile GetDebugMotionEnvelopeProfile(EOffgridAITextViseme Viseme)
    {
        FOffgridAIDebugMotionEnvelopeProfile P;
        switch (Viseme)
        {
        case EOffgridAITextViseme::MBP:
            P.Name = TEXT("CLOSURE_SNAP_RELEASE");
            P.AttackLeadSeconds = 0.050f;
            P.MinPeakHoldSeconds = 0.035f;
            P.ReleaseTailSeconds = 0.075f;
            P.PeakStrengthMultiplier = 1.02f;
            P.OvershootAmount = 0.10f;
            P.JawTarget = 0.08f;
            P.MaxJawVelocityPerSecond = 10.0f;
            P.bClosure = true;
            break;
        case EOffgridAITextViseme::AAA:
            P.Name = TEXT("OPEN_ARC_SETTLE");
            P.AttackLeadSeconds = 0.080f;
            P.MinPeakHoldSeconds = 0.105f;
            P.ReleaseTailSeconds = 0.155f;
            P.PeakStrengthMultiplier = 1.00f;
            P.OvershootAmount = 0.075f;
            P.JawTarget = 0.96f;
            P.MaxJawVelocityPerSecond = 6.25f;
            P.bVowel = true;
            break;
        case EOffgridAITextViseme::EEE:
            P.Name = TEXT("WIDE_ARC_SETTLE");
            P.AttackLeadSeconds = 0.070f;
            P.MinPeakHoldSeconds = 0.085f;
            P.ReleaseTailSeconds = 0.125f;
            P.PeakStrengthMultiplier = 0.94f;
            P.OvershootAmount = 0.055f;
            P.JawTarget = 0.48f;
            P.MaxJawVelocityPerSecond = 7.0f;
            P.bVowel = true;
            break;
        case EOffgridAITextViseme::OOO:
            P.Name = TEXT("ROUND_SLOW_SETTLE");
            P.AttackLeadSeconds = 0.095f;
            P.MinPeakHoldSeconds = 0.115f;
            P.ReleaseTailSeconds = 0.180f;
            P.PeakStrengthMultiplier = 0.98f;
            P.OvershootAmount = 0.065f;
            P.JawTarget = 0.42f;
            P.RoundTarget = 1.0f;
            P.MaxJawVelocityPerSecond = 5.75f;
            P.bVowel = true;
            break;
        case EOffgridAITextViseme::WUH:
            P.Name = TEXT("FUNNEL_QUICK_SETTLE");
            P.AttackLeadSeconds = 0.055f;
            P.MinPeakHoldSeconds = 0.075f;
            P.ReleaseTailSeconds = 0.115f;
            P.PeakStrengthMultiplier = 0.96f;
            P.OvershootAmount = 0.045f;
            P.JawTarget = 0.30f;
            P.RoundTarget = 0.80f;
            P.MaxJawVelocityPerSecond = 7.5f;
            P.bVowel = true;
            break;
        case EOffgridAITextViseme::FVS:
            P.Name = TEXT("FRICATIVE_PLATEAU_RELEASE");
            P.AttackLeadSeconds = 0.045f;
            P.MinPeakHoldSeconds = 0.105f;
            P.ReleaseTailSeconds = 0.105f;
            P.PeakStrengthMultiplier = 0.96f;
            P.OvershootAmount = 0.020f;
            P.JawTarget = 0.26f;
            P.TeethTarget = 1.0f;
            P.MaxJawVelocityPerSecond = 8.0f;
            P.bFricative = true;
            break;
        default:
            break;
        }
        return P;
    }

    static void AppendUInt16LE(TArray<uint8>& Bytes, uint16 Value)
    {
        Bytes.Add(static_cast<uint8>(Value & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 8) & 0xFF));
    }

    static void AppendUInt32LE(TArray<uint8>& Bytes, uint32 Value)
    {
        Bytes.Add(static_cast<uint8>(Value & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 8) & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 16) & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 24) & 0xFF));
    }

    static void AppendAnsi(TArray<uint8>& Bytes, const char* Text, int32 Count)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Bytes.Add(static_cast<uint8>(Text[Index]));
        }
    }

    static bool WritePCM16MonoWav(const FString& Path, const TArray<int16>& Samples, int32 SampleRate)
    {
        if (Samples.Num() <= 0 || SampleRate <= 0)
        {
            return false;
        }

        TArray<uint8> Bytes;
        const uint32 DataBytes = static_cast<uint32>(Samples.Num() * sizeof(int16));
        const uint32 RiffSize = 36u + DataBytes;
        Bytes.Reserve(static_cast<int32>(44u + DataBytes));

        AppendAnsi(Bytes, "RIFF", 4);
        AppendUInt32LE(Bytes, RiffSize);
        AppendAnsi(Bytes, "WAVE", 4);
        AppendAnsi(Bytes, "fmt ", 4);
        AppendUInt32LE(Bytes, 16);
        AppendUInt16LE(Bytes, 1);      // PCM
        AppendUInt16LE(Bytes, 1);      // mono
        AppendUInt32LE(Bytes, static_cast<uint32>(SampleRate));
        AppendUInt32LE(Bytes, static_cast<uint32>(SampleRate * sizeof(int16)));
        AppendUInt16LE(Bytes, sizeof(int16));
        AppendUInt16LE(Bytes, 16);
        AppendAnsi(Bytes, "data", 4);
        AppendUInt32LE(Bytes, DataBytes);

        for (int16 Sample : Samples)
        {
            const uint16 AsUnsigned = static_cast<uint16>(Sample);
            AppendUInt16LE(Bytes, AsUnsigned);
        }

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        return FFileHelper::SaveArrayToFile(Bytes, *Path);
    }


    struct FOffgridAIAudioPlannerFeaturePoint
    {
        float TimeSeconds = 0.0f;
        float RMS = 0.0f;
        float RMSNorm = 0.0f;
        float DeltaRMS = 0.0f;
        float Flux = 0.0f;
        float ZCR = 0.0f;
        float LowProxy = 0.0f;
        float MidProxy = 0.0f;
        float HighProxy = 0.0f;
        float CentroidProxy = 0.0f;
        float RolloffProxy = 0.0f;
        float FlatnessProxy = 0.0f;
        float CepstralTiltProxy = 0.0f;
        float EnergyAcceleration = 0.0f;
        float SmoothedRMSNorm = 0.0f;
        float SmoothedFlux = 0.0f;
        float SmoothedLowProxy = 0.0f;
        float SmoothedMidProxy = 0.0f;
        float SmoothedHighProxy = 0.0f;
        float SmoothedCentroidProxy = 0.0f;
        float SmoothedRolloffProxy = 0.0f;
        float SmoothedFlatnessProxy = 0.0f;
        float SmoothedCepstralTiltProxy = 0.0f;
        bool bLocalRMSPeak = false;
        bool bLocalRMSValley = false;
        bool bLocalFluxPeak = false;
        float FricativeContinuity = 0.0f;
        int32 VowelRegionID = INDEX_NONE;
        float VowelRegionStrength = 0.0f;
        FString VowelFamilyGuess;
        FString AcousticStructureGuess;
    };

    static float ClampFinalRenderCenterToSpeechStart(float CenterSeconds, float SpeechStartSeconds);

    static float ComputeAudioPlannerBeatProxy(const FOffgridAIAudioPlannerFeaturePoint& Point)
    {
        // Mirrors the lab's Layer2 diagnostic beat proxy: smoothed energy plus
        // onset flux. This is diagnostic-only in the foundation patch; it does
        // not change the runtime aligner authority.
        return FMath::Clamp(Point.SmoothedRMSNorm + 0.85f * Point.SmoothedFlux, 0.0f, 1.0f);
    }

    static void PopulateAlignedEventMetadata(
        FOffgridAIAlignedVisemeEvent& Aligned,
        const FOffgridAITextVisemeEvent& TextEvent,
        float CenterNorm,
        float AudioStartSeconds,
        float AudioEndSeconds)
    {
        const float AudioDuration = FMath::Max(AudioEndSeconds - AudioStartSeconds, 0.10f);
        const float StartNorm = FMath::Clamp(TextEvent.StartNorm, 0.0f, 1.0f);
        const float EndNorm = FMath::Clamp(TextEvent.EndNorm, StartNorm, 1.0f);
        const float WidthSeconds = FMath::Clamp((EndNorm - StartNorm) * AudioDuration, 0.045f, 0.160f);

        Aligned.SourceWord = TextEvent.SourceText;
        Aligned.WordIndex = TextEvent.WordIndex;
        Aligned.PhraseIndex = TextEvent.PhraseIndex;
        Aligned.bIsLandmark = TextEvent.bIsLandmark;
        Aligned.TextCenterNorm = FMath::Clamp(CenterNorm, 0.0f, 1.0f);
        Aligned.RenderStartSeconds = FMath::Max(0.0f, Aligned.FinalRenderCenterSeconds - WidthSeconds * 0.5f);
        Aligned.RenderEndSeconds = FMath::Max(Aligned.RenderStartSeconds, Aligned.FinalRenderCenterSeconds + WidthSeconds * 0.5f);
    }

    static bool IsLabVowelPose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        return S == TEXT("03_Ee") || S == TEXT("04_Ih") || S == TEXT("05_Ay") || S == TEXT("06_Eh") ||
            S == TEXT("07_Aa") || S == TEXT("08_Ah") || S == TEXT("09_Oh") || S == TEXT("10_Or") ||
            S == TEXT("11_Oo") || S == TEXT("12_Ww-Oo-") || S == TEXT("16_Ww-Ew-") || S == TEXT("17_Rr") ||
            S == TEXT("18_Uh");
    }

    // M09 audio-occupancy runtime does not perform offline Layer2 audio retiming.
    // The helpers that retimed visemes against audio features were deleted to keep
    // OffgridAI aligned with the shared M-series runtime.

    struct FOffgridAIAudioPlannerBounds
    {
        bool bDetected = false;
        float StartSeconds = 0.0f;
        float EndSeconds = 0.0f;
        float PeakRMS = 0.0001f;
        float ThresholdRMS = 0.010f;
    };

    static FString AudioPlannerDetectorForPose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        if (S.Contains(TEXT("MBP"), ESearchCase::IgnoreCase)) return TEXT("MBP_RMS_VALLEY_PLUS_RELEASE_FLUX");
        if (S.Contains(TEXT("FV"), ESearchCase::IgnoreCase)) return TEXT("FV_FRICATIVE_ZCR_MID_HIGH");
        if (S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase)) return TEXT("CH_SH_FRICATIVE_FLUX_HIGH");
        if (S.Contains(TEXT("Tongue_Th"), ESearchCase::IgnoreCase)) return TEXT("TH_DIFFUSE_HIGH_ZCR");
        if (S.Contains(TEXT("Tongue_LNTDS"), ESearchCase::IgnoreCase) || S.Contains(TEXT("TDS"), ESearchCase::IgnoreCase)) return TEXT("ALVEOLAR_TRANSIENT_OR_HIGH_ZCR");
        if (S.Contains(TEXT("Tongue_KGY"), ESearchCase::IgnoreCase) || S.Contains(TEXT("KGY"), ESearchCase::IgnoreCase)) return TEXT("VELAR_BURST_FLUX");
        if (S.Contains(TEXT("Ww"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ew"), ESearchCase::IgnoreCase)) return TEXT("W_LOW_CENTROID_RISING_ENERGY");
        if (S.Contains(TEXT("Ee"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ih"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ay"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Eh"), ESearchCase::IgnoreCase)) return TEXT("WIDE_VOWEL_CENTROID_MID_HIGH");
        if (S.Contains(TEXT("Oo"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Or"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Oh"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Rr"), ESearchCase::IgnoreCase)) return TEXT("ROUND_VOWEL_LOW_CENTROID");
        if (S.Contains(TEXT("Aa"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ah"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Uh"), ESearchCase::IgnoreCase)) return TEXT("OPEN_VOWEL_LOW_MID_ENERGY");
        return TEXT("GENERIC_RMS_PEAK");
    }


    static bool IsAudioPlannerOptionalDetailPose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        return S.Contains(TEXT("Tongue_LNTDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Tongue_KGY"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("01_TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("02_TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("13_KGY"), ESearchCase::IgnoreCase);
    }

    static bool IsAudioPlannerLandmarkPose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        return S.Contains(TEXT("MBP"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("FV"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ww"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("03_Ee"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("05_Ay"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("07_Aa"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("09_Oh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("11_Oo"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("12_Ww"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("16_Ww"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("21_FV"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("19_FV"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("20_FV"), ESearchCase::IgnoreCase);
    }

    static FString AudioPlannerImportanceForPose(FName PoseID)
    {
        if (IsAudioPlannerLandmarkPose(PoseID))
        {
            return TEXT("LANDMARK");
        }
        if (IsAudioPlannerOptionalDetailPose(PoseID))
        {
            return TEXT("OPTIONAL");
        }
        return TEXT("NORMAL");
    }

    static float AudioPlannerDurationScaleForPose(FName PoseID)
    {
        const FString S = PoseID.ToString();

        // Tuned playback widths. These are intentionally plain constants: the
        // text plan is only a guessed performance schedule, and fast TTS speech
        // reads better when minor/detail poses occupy less time.
        if (IsAudioPlannerOptionalDetailPose(PoseID))
        {
            return 0.14f;
        }
        if (S.Contains(TEXT("Tongue_Th"), ESearchCase::IgnoreCase))
        {
            return 0.20f;
        }
        if (S.Contains(TEXT("MBP"), ESearchCase::IgnoreCase))
        {
            return 0.36f;
        }
        if (S.Contains(TEXT("FV"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase))
        {
            return 0.44f;
        }
        if (S.Contains(TEXT("Ww"), ESearchCase::IgnoreCase))
        {
            return 0.43f;
        }
        if (S.Contains(TEXT("09_Oh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("10_Or"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("11_Oo"), ESearchCase::IgnoreCase))
        {
            return 0.58f;
        }
        if (S.Contains(TEXT("03_Ee"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("04_Ih"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("05_Ay"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("06_Eh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("17_Rr"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("18_Uh"), ESearchCase::IgnoreCase))
        {
            return 0.50f;
        }
        if (S.Contains(TEXT("07_Aa"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("08_Ah"), ESearchCase::IgnoreCase))
        {
            return 0.51f;
        }
        return 0.42f;
    }

    static float AudioPlannerStrengthScaleForPose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        if (IsAudioPlannerOptionalDetailPose(PoseID))
        {
            return 0.62f;
        }
        if (S.Contains(TEXT("Tongue_Th"), ESearchCase::IgnoreCase))
        {
            return 0.74f;
        }
        if (S.Contains(TEXT("04_Ih"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("06_Eh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("17_Rr"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("18_Uh"), ESearchCase::IgnoreCase))
        {
            return 0.88f;
        }
        return 1.0f;
    }


    static FString AudioPlannerVowelFamilyGuessForFeature(const FOffgridAIAudioPlannerFeaturePoint& Point)
    {
        const float Total = FMath::Max(Point.SmoothedLowProxy + Point.SmoothedMidProxy + Point.SmoothedHighProxy, 0.0001f);
        const float Low = Point.SmoothedLowProxy / Total;
        const float Mid = Point.SmoothedMidProxy / Total;
        const float High = Point.SmoothedHighProxy / Total;
        const float Centroid = Point.SmoothedCentroidProxy;
        const float Rolloff = Point.SmoothedRolloffProxy;
        const float Tilt = Point.SmoothedCepstralTiltProxy;
        const float Flatness = Point.SmoothedFlatnessProxy;

        // These are deliberately broad vowel families, not exact phonemes.
        // A stable low centroid + positive low/high tilt is rounded/back.
        if (Low > 0.41f && Centroid < 0.44f && Tilt > 0.05f)
        {
            return TEXT("ROUNDED");
        }
        // Bright/front vowels have higher centroid/rolloff and weaker low dominance.
        if ((High > 0.31f || Centroid > 0.56f || Rolloff > 0.58f) && Mid > 0.16f && Tilt < 0.34f)
        {
            return TEXT("WIDE");
        }
        // Open vowels carry strong low+mid energy but are not as low/rounded as Oo/Oh.
        if (Mid > 0.34f && Low > 0.24f && Centroid >= 0.30f && Centroid <= 0.64f && Flatness < 0.78f)
        {
            return TEXT("OPEN");
        }
        return TEXT("CENTRAL");
    }

    static FString AudioPlannerExpectedVowelFamilyForPose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        if (S.Contains(TEXT("Ee"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ih"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ay"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Eh"), ESearchCase::IgnoreCase))
        {
            return TEXT("WIDE");
        }
        if (S.Contains(TEXT("Oo"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Or"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Oh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Rr"), ESearchCase::IgnoreCase))
        {
            return TEXT("ROUNDED");
        }
        if (S.Contains(TEXT("Aa"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ah"), ESearchCase::IgnoreCase))
        {
            return TEXT("OPEN");
        }
        if (S.Contains(TEXT("Uh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ew"), ESearchCase::IgnoreCase))
        {
            return TEXT("CENTRAL");
        }
        return TEXT("");
    }

    static TArray<FOffgridAIAudioPlannerFeaturePoint> BuildAudioPlannerFeaturesFromPCM16(const TArray<int16>& Samples, int32 SampleRate)
    {
        TArray<FOffgridAIAudioPlannerFeaturePoint> Out;
        if (Samples.Num() <= 0 || SampleRate <= 0)
        {
            return Out;
        }

        const int32 WindowFrames = FMath::Clamp(FMath::RoundToInt(static_cast<float>(SampleRate) * 0.020f), 128, 1024);
        const int32 HopFrames = FMath::Clamp(FMath::RoundToInt(static_cast<float>(SampleRate) * 0.005f), 48, WindowFrames);
        if (Samples.Num() < WindowFrames)
        {
            return Out;
        }

        Out.Reserve(1 + Samples.Num() / HopFrames);
        float PeakRMS = 0.0001f;
        float PrevRMS = 0.0f;
        float PrevDeltaRMS = 0.0f;

        for (int32 Start = 0; Start + WindowFrames <= Samples.Num(); Start += HopFrames)
        {
            double SumSquares = 0.0;
            double SumAbsDiff = 0.0;
            double LowProxySum = 0.0;
            double MidProxySum = 0.0;
            double HighProxySum = 0.0;
            int32 ZeroCrossings = 0;
            float PrevSample = static_cast<float>(Samples[Start]) / 32768.0f;
            float LowSlowState = PrevSample;
            float LowFastState = PrevSample;

            for (int32 I = Start; I < Start + WindowFrames; ++I)
            {
                const float V = static_cast<float>(Samples[I]) / 32768.0f;
                SumSquares += static_cast<double>(V) * static_cast<double>(V);

                if (I > Start)
                {
                    if ((V >= 0.0f && PrevSample < 0.0f) || (V < 0.0f && PrevSample >= 0.0f))
                    {
                        ++ZeroCrossings;
                    }
                    SumAbsDiff += FMath::Abs(V - PrevSample);
                }

                // Lightweight, dependency-free spectral proxies. These are not true
                // mel bands, but they separate low vowel energy, mid resonance, and
                // high/fricative noise better than the earlier single high-pass proxy.
                LowSlowState = FMath::Lerp(LowSlowState, V, 0.035f);
                LowFastState = FMath::Lerp(LowFastState, V, 0.220f);
                const float LowComponent = LowSlowState;
                const float MidComponent = LowFastState - LowSlowState;
                const float HighComponent = V - LowFastState;
                LowProxySum += static_cast<double>(LowComponent) * static_cast<double>(LowComponent);
                MidProxySum += static_cast<double>(MidComponent) * static_cast<double>(MidComponent);
                HighProxySum += static_cast<double>(HighComponent) * static_cast<double>(HighComponent);
                PrevSample = V;
            }

            FOffgridAIAudioPlannerFeaturePoint Point;
            Point.TimeSeconds = (static_cast<float>(Start) + static_cast<float>(WindowFrames) * 0.5f) / static_cast<float>(SampleRate);
            Point.RMS = FMath::Sqrt(static_cast<float>(SumSquares / static_cast<double>(WindowFrames)));
            Point.DeltaRMS = Point.RMS - PrevRMS;
            Point.Flux = static_cast<float>(SumAbsDiff / static_cast<double>(FMath::Max(WindowFrames - 1, 1)));
            Point.ZCR = static_cast<float>(ZeroCrossings) / static_cast<float>(FMath::Max(WindowFrames - 1, 1));
            Point.LowProxy = FMath::Sqrt(static_cast<float>(LowProxySum / static_cast<double>(WindowFrames)));
            Point.MidProxy = FMath::Sqrt(static_cast<float>(MidProxySum / static_cast<double>(WindowFrames)));
            Point.HighProxy = FMath::Sqrt(static_cast<float>(HighProxySum / static_cast<double>(WindowFrames)));
            const float BandTotal = FMath::Max(Point.LowProxy + Point.MidProxy + Point.HighProxy, 0.0001f);
            Point.CentroidProxy = (Point.MidProxy * 0.5f + Point.HighProxy) / BandTotal;
            Point.RolloffProxy = (Point.HighProxy > Point.MidProxy && Point.HighProxy > Point.LowProxy) ? 1.0f
                : ((Point.MidProxy + Point.HighProxy) / BandTotal);
            const float ArithmeticMean = BandTotal / 3.0f;
            const float GeometricMean = FMath::Pow(FMath::Max(Point.LowProxy, 0.000001f) * FMath::Max(Point.MidProxy, 0.000001f) * FMath::Max(Point.HighProxy, 0.000001f), 1.0f / 3.0f);
            Point.FlatnessProxy = FMath::Clamp(GeometricMean / FMath::Max(ArithmeticMean, 0.000001f), 0.0f, 1.0f);
            Point.CepstralTiltProxy = FMath::Clamp((Point.LowProxy - Point.HighProxy) / BandTotal, -1.0f, 1.0f);
            Point.EnergyAcceleration = Point.DeltaRMS - PrevDeltaRMS;
            Out.Add(Point);
            PeakRMS = FMath::Max(PeakRMS, Point.RMS);
            PrevDeltaRMS = Point.DeltaRMS;
            PrevRMS = Point.RMS;
        }

        float PeakFlux = 0.0001f;
        float PeakLow = 0.0001f;
        float PeakMid = 0.0001f;
        float PeakHigh = 0.0001f;
        for (const FOffgridAIAudioPlannerFeaturePoint& Point : Out)
        {
            PeakFlux = FMath::Max(PeakFlux, Point.Flux);
            PeakLow = FMath::Max(PeakLow, Point.LowProxy);
            PeakMid = FMath::Max(PeakMid, Point.MidProxy);
            PeakHigh = FMath::Max(PeakHigh, Point.HighProxy);
        }
        for (FOffgridAIAudioPlannerFeaturePoint& Point : Out)
        {
            Point.RMSNorm = FMath::Clamp(Point.RMS / PeakRMS, 0.0f, 1.0f);
            Point.Flux = FMath::Clamp(Point.Flux / PeakFlux, 0.0f, 1.0f);
            Point.LowProxy = FMath::Clamp(Point.LowProxy / PeakLow, 0.0f, 1.0f);
            Point.MidProxy = FMath::Clamp(Point.MidProxy / PeakMid, 0.0f, 1.0f);
            Point.HighProxy = FMath::Clamp(Point.HighProxy / PeakHigh, 0.0f, 1.0f);
        }

        for (int32 I = 0; I < Out.Num(); ++I)
        {
            FOffgridAIAudioPlannerFeaturePoint& Point = Out[I];
            if (I == 0)
            {
                Point.SmoothedRMSNorm = Point.RMSNorm;
                Point.SmoothedFlux = Point.Flux;
                Point.SmoothedLowProxy = Point.LowProxy;
                Point.SmoothedMidProxy = Point.MidProxy;
                Point.SmoothedHighProxy = Point.HighProxy;
                Point.SmoothedCentroidProxy = Point.CentroidProxy;
                Point.SmoothedRolloffProxy = Point.RolloffProxy;
                Point.SmoothedFlatnessProxy = Point.FlatnessProxy;
                Point.SmoothedCepstralTiltProxy = Point.CepstralTiltProxy;
            }
            else
            {
                const FOffgridAIAudioPlannerFeaturePoint& Prev = Out[I - 1];
                constexpr float SmoothAlpha = 0.18f;
                Point.SmoothedRMSNorm = FMath::Lerp(Prev.SmoothedRMSNorm, Point.RMSNorm, SmoothAlpha);
                Point.SmoothedFlux = FMath::Lerp(Prev.SmoothedFlux, Point.Flux, SmoothAlpha);
                Point.SmoothedLowProxy = FMath::Lerp(Prev.SmoothedLowProxy, Point.LowProxy, SmoothAlpha);
                Point.SmoothedMidProxy = FMath::Lerp(Prev.SmoothedMidProxy, Point.MidProxy, SmoothAlpha);
                Point.SmoothedHighProxy = FMath::Lerp(Prev.SmoothedHighProxy, Point.HighProxy, SmoothAlpha);
                Point.SmoothedCentroidProxy = FMath::Lerp(Prev.SmoothedCentroidProxy, Point.CentroidProxy, SmoothAlpha);
                Point.SmoothedRolloffProxy = FMath::Lerp(Prev.SmoothedRolloffProxy, Point.RolloffProxy, SmoothAlpha);
                Point.SmoothedFlatnessProxy = FMath::Lerp(Prev.SmoothedFlatnessProxy, Point.FlatnessProxy, SmoothAlpha);
                Point.SmoothedCepstralTiltProxy = FMath::Lerp(Prev.SmoothedCepstralTiltProxy, Point.CepstralTiltProxy, SmoothAlpha);
            }
        }

        for (int32 I = 0; I < Out.Num(); ++I)
        {
            FOffgridAIAudioPlannerFeaturePoint& Point = Out[I];
            const int32 First = FMath::Max(I - 5, 0);
            const int32 Last = FMath::Min(I + 5, Out.Num() - 1);
            float MaxRMS = 0.0f;
            float MinRMS = 1.0f;
            float MaxFluxLocal = 0.0f;
            float FricativeSum = 0.0f;
            int32 Count = 0;
            for (int32 J = First; J <= Last; ++J)
            {
                MaxRMS = FMath::Max(MaxRMS, Out[J].SmoothedRMSNorm);
                MinRMS = FMath::Min(MinRMS, Out[J].SmoothedRMSNorm);
                MaxFluxLocal = FMath::Max(MaxFluxLocal, Out[J].SmoothedFlux);
                FricativeSum += FMath::Clamp(Out[J].ZCR * 4.0f, 0.0f, 1.0f) * 0.55f + Out[J].SmoothedHighProxy * 0.45f;
                ++Count;
            }
            Point.bLocalRMSPeak = Point.SmoothedRMSNorm >= MaxRMS - 0.015f && Point.SmoothedRMSNorm >= 0.18f;
            Point.bLocalRMSValley = Point.SmoothedRMSNorm <= MinRMS + 0.015f && (MaxRMS - Point.SmoothedRMSNorm) >= 0.10f;
            Point.bLocalFluxPeak = Point.SmoothedFlux >= MaxFluxLocal - 0.020f && Point.SmoothedFlux >= 0.18f;
            Point.FricativeContinuity = Count > 0 ? FMath::Clamp(FricativeSum / static_cast<float>(Count), 0.0f, 1.0f) : 0.0f;
            Point.VowelFamilyGuess = AudioPlannerVowelFamilyGuessForFeature(Point);
            if (Point.bLocalRMSValley && Point.bLocalFluxPeak)
            {
                Point.AcousticStructureGuess = TEXT("CLOSURE_RELEASE");
            }
            else if (Point.FricativeContinuity >= 0.62f && Point.SmoothedHighProxy >= 0.24f && Point.SmoothedFlatnessProxy >= 0.38f)
            {
                Point.AcousticStructureGuess = TEXT("FRICATIVE_SPAN");
            }
            else if (Point.bLocalRMSPeak && Point.FricativeContinuity < 0.55f && Point.SmoothedFlatnessProxy < 0.82f)
            {
                Point.AcousticStructureGuess = TEXT("VOWEL_NUCLEUS");
            }
            else if (Point.bLocalFluxPeak)
            {
                Point.AcousticStructureGuess = TEXT("TRANSIENT_BURST");
            }
            else
            {
                Point.AcousticStructureGuess = TEXT("WEAK_FRAME");
            }
        }

        // Group coherent vowel nuclei so repeated expected vowel events can be
        // evaluated against regions rather than independent arbitrary frames.
        int32 CurrentVowelRegionID = INDEX_NONE;
        float CurrentRegionPeak = 0.0f;
        int32 CurrentRegionStart = INDEX_NONE;
        auto FinalizeRegion = [&Out, &CurrentVowelRegionID, &CurrentRegionPeak, &CurrentRegionStart](int32 RegionEndExclusive)
        {
            if (CurrentRegionStart == INDEX_NONE || CurrentVowelRegionID == INDEX_NONE)
            {
                return;
            }
            for (int32 R = CurrentRegionStart; R < RegionEndExclusive && Out.IsValidIndex(R); ++R)
            {
                Out[R].VowelRegionID = CurrentVowelRegionID;
                Out[R].VowelRegionStrength = CurrentRegionPeak;
            }
            CurrentRegionStart = INDEX_NONE;
            CurrentVowelRegionID = INDEX_NONE;
            CurrentRegionPeak = 0.0f;
        };

        int32 NextVowelRegionID = 0;
        for (int32 I = 0; I < Out.Num(); ++I)
        {
            FOffgridAIAudioPlannerFeaturePoint& Point = Out[I];
            const bool bVowelLike = Point.SmoothedRMSNorm >= 0.18f
                && Point.FricativeContinuity < 0.58f
                && Point.SmoothedFlatnessProxy < 0.84f
                && !Point.VowelFamilyGuess.IsEmpty();
            if (bVowelLike)
            {
                if (CurrentRegionStart == INDEX_NONE)
                {
                    CurrentRegionStart = I;
                    CurrentVowelRegionID = NextVowelRegionID++;
                    CurrentRegionPeak = 0.0f;
                }
                CurrentRegionPeak = FMath::Max(CurrentRegionPeak, Point.SmoothedRMSNorm);
            }
            else
            {
                FinalizeRegion(I);
            }
        }
        FinalizeRegion(Out.Num());
        return Out;
    }

    static FOffgridAIAudioPlannerBounds ComputeAudioPlannerSpeechBounds(const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features)
    {
        FOffgridAIAudioPlannerBounds Bounds;
        if (Features.Num() <= 0)
        {
            return Bounds;
        }

        float PeakRMS = 0.0001f;
        for (const FOffgridAIAudioPlannerFeaturePoint& Point : Features)
        {
            PeakRMS = FMath::Max(PeakRMS, Point.RMS);
        }
        const int32 NoiseCount = FMath::Clamp(FMath::CeilToInt(static_cast<float>(Features.Num()) * 0.08f), 1, Features.Num());
        float NoiseMean = 0.0f;
        for (int32 I = 0; I < NoiseCount; ++I)
        {
            NoiseMean += Features[I].RMS;
        }
        NoiseMean /= static_cast<float>(NoiseCount);

        TArray<float> SortedRMS;
        SortedRMS.Reserve(Features.Num());
        for (const FOffgridAIAudioPlannerFeaturePoint& Point : Features)
        {
            SortedRMS.Add(Point.RMS);
        }
        SortedRMS.Sort();
        const int32 P10Index = FMath::Clamp(FMath::RoundToInt(static_cast<float>(SortedRMS.Num() - 1) * 0.10f), 0, SortedRMS.Num() - 1);
        const int32 P25Index = FMath::Clamp(FMath::RoundToInt(static_cast<float>(SortedRMS.Num() - 1) * 0.25f), 0, SortedRMS.Num() - 1);
        const float P10 = SortedRMS.IsValidIndex(P10Index) ? SortedRMS[P10Index] : NoiseMean;
        const float P25 = SortedRMS.IsValidIndex(P25Index) ? SortedRMS[P25Index] : NoiseMean;
        const float AdaptiveFloor = FMath::Max(P10 + (P25 - P10) * 1.75f, NoiseMean * 1.45f);
        const float PeakRelative = PeakRMS * 0.18f;
        const float AbsoluteFloor = FMath::Min(0.0022f, PeakRMS * 0.28f);

        Bounds.PeakRMS = PeakRMS;
        // Keep the gate below the line peak. Previous versions could set the threshold above
        // the actual speech peak on quiet lines, which made the planner report no speech.
        Bounds.ThresholdRMS = FMath::Clamp(
            FMath::Max3(AbsoluteFloor, AdaptiveFloor, PeakRelative),
            FMath::Min(AbsoluteFloor, PeakRMS * 0.08f),
            FMath::Max(PeakRMS * 0.42f, 0.0002f));

        int32 First = INDEX_NONE;
        int32 Last = INDEX_NONE;
        for (int32 I = 0; I < Features.Num(); ++I)
        {
            if (Features[I].RMS >= Bounds.ThresholdRMS)
            {
                if (First == INDEX_NONE)
                {
                    First = I;
                }
                Last = I;
            }
        }

        if (First != INDEX_NONE && Last != INDEX_NONE)
        {
            Bounds.bDetected = true;
            Bounds.StartSeconds = Features[First].TimeSeconds;
            Bounds.EndSeconds = Features[Last].TimeSeconds;
        }
        return Bounds;
    }


    static bool ComputeAudioPlannerRenderSpeechBoundsFromPCM16(const TArray<int16>& Samples, int32 SampleRate, float& OutStartSeconds, float& OutEndSeconds)
    {
        OutStartSeconds = 0.0f;
        OutEndSeconds = 0.0f;
        if (Samples.Num() <= 0 || SampleRate <= 0)
        {
            return false;
        }

        const float TotalDurationSeconds = static_cast<float>(Samples.Num()) / static_cast<float>(SampleRate);
        const int32 WindowFrames = FMath::Clamp(FMath::RoundToInt(static_cast<float>(SampleRate) * 0.020f), 128, 1024);
        const int32 HopFrames = FMath::Clamp(FMath::RoundToInt(static_cast<float>(SampleRate) * 0.005f), 48, WindowFrames);
        if (Samples.Num() < WindowFrames)
        {
            OutStartSeconds = 0.0f;
            OutEndSeconds = TotalDurationSeconds;
            return TotalDurationSeconds > 0.0f;
        }

        TArray<float> FrameRMS;
        FrameRMS.Reserve(1 + Samples.Num() / HopFrames);
        float PeakRMS = 0.000001f;
        for (int32 Start = 0; Start + WindowFrames <= Samples.Num(); Start += HopFrames)
        {
            double SumSquares = 0.0;
            for (int32 I = Start; I < Start + WindowFrames; ++I)
            {
                const float V = static_cast<float>(Samples[I]) / 32768.0f;
                SumSquares += static_cast<double>(V) * static_cast<double>(V);
            }
            const float RMS = FMath::Sqrt(static_cast<float>(SumSquares / static_cast<double>(WindowFrames)));
            FrameRMS.Add(RMS);
            PeakRMS = FMath::Max(PeakRMS, RMS);
        }

        // v32h streaming timing contract:
        // Use asymmetric speech bounds. The tail/end uses a low threshold so quiet
        // final vowels are not cut off, but the start must be more conservative so
        // breath/noise in the preroll cannot arm layer 3 and fire raw-looking mouth
        // motion before speech actually begins.
        const float TailThreshold = FMath::Max(0.0012f, PeakRMS * 0.035f);
        const float StartThreshold = FMath::Max(0.0024f, PeakRMS * 0.090f);

        auto IsSustainedSpeechOnset = [&](int32 FrameIndex) -> bool
        {
            int32 TailCount = 0;
            int32 StrongCount = 0;
            float LocalPeak = 0.0f;
            const int32 LastProbe = FMath::Min(FrameIndex + 8, FrameRMS.Num() - 1);
            for (int32 J = FrameIndex; J <= LastProbe; ++J)
            {
                const float R = FrameRMS[J];
                LocalPeak = FMath::Max(LocalPeak, R);
                if (R >= TailThreshold)
                {
                    ++TailCount;
                }
                if (R >= StartThreshold)
                {
                    ++StrongCount;
                }
            }
            return TailCount >= 4 && StrongCount >= 1 && LocalPeak >= StartThreshold;
        };

        int32 FirstFrame = INDEX_NONE;
        int32 LastFrame = INDEX_NONE;
        for (int32 I = 0; I < FrameRMS.Num(); ++I)
        {
            if (FirstFrame == INDEX_NONE && IsSustainedSpeechOnset(I))
            {
                FirstFrame = I;
            }
            if (FirstFrame != INDEX_NONE && FrameRMS[I] >= TailThreshold)
            {
                LastFrame = I;
            }
        }

        if (FirstFrame == INDEX_NONE || LastFrame == INDEX_NONE)
        {
            return false;
        }

        const float HalfWindowSeconds = (static_cast<float>(WindowFrames) * 0.5f) / static_cast<float>(SampleRate);
        const float FirstCenterSeconds = (static_cast<float>(FirstFrame * HopFrames) + static_cast<float>(WindowFrames) * 0.5f) / static_cast<float>(SampleRate);
        const float LastCenterSeconds = (static_cast<float>(LastFrame * HopFrames) + static_cast<float>(WindowFrames) * 0.5f) / static_cast<float>(SampleRate);
        OutStartSeconds = FMath::Clamp(FirstCenterSeconds - HalfWindowSeconds, 0.0f, TotalDurationSeconds);
        OutEndSeconds = FMath::Clamp(LastCenterSeconds + HalfWindowSeconds, OutStartSeconds + 0.05f, TotalDurationSeconds);
        return true;
    }


    struct FOffgridAITextPhraseBoundary
    {
        float TextBoundaryNorm = 0.0f;
        FString SourceText;
    };

    struct FOffgridAIAudioPhraseGap
    {
        float CenterSeconds = 0.0f;
        float StartSeconds = 0.0f;
        float EndSeconds = 0.0f;
        float Confidence = 0.0f;
    };

    static bool OffgridAIPhraseWordEndsPhrase(const FString& SourceText)
    {
        const FString S = SourceText.TrimStartAndEnd();
        if (S.IsEmpty())
        {
            return false;
        }
        const TCHAR Last = S[S.Len() - 1];
        return Last == TEXT('.') || Last == TEXT('?') || Last == TEXT('!') ||
            Last == TEXT(';') || Last == TEXT(':') || Last == TEXT(',');
    }

    static TArray<FOffgridAITextPhraseBoundary> BuildTextPhraseBoundariesFromPlan(const TArray<FOffgridAITextVisemeEvent>& Events)
    {
        TArray<FOffgridAITextPhraseBoundary> Out;
        float LastBoundaryNorm = 0.0f;
        FString LastBoundaryWord;
        for (const FOffgridAITextVisemeEvent& Event : Events)
        {
            if (Event.PoseID == NAME_None || Event.Viseme == EOffgridAITextViseme::Rest)
            {
                continue;
            }
            const float EndNorm = FMath::Clamp(Event.EndNorm, 0.0f, 1.0f);
            if (!OffgridAIPhraseWordEndsPhrase(Event.SourceText))
            {
                continue;
            }
            // Do not create tiny phrase slices. Very dense punctuation on short
            // generated lines makes phrase warping worse than the global affine.
            if (EndNorm <= 0.08f || EndNorm >= 0.94f || EndNorm - LastBoundaryNorm < 0.12f)
            {
                continue;
            }
            FOffgridAITextPhraseBoundary Boundary;
            Boundary.TextBoundaryNorm = EndNorm;
            Boundary.SourceText = Event.SourceText;
            Out.Add(Boundary);
            LastBoundaryNorm = EndNorm;
            LastBoundaryWord = Event.SourceText;
        }
        return Out;
    }

    static TArray<FOffgridAIAudioPhraseGap> DetectAudioPhraseGaps(
        const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features,
        float SpeechStartSeconds,
        float SpeechEndSeconds,
        float SpeechThresholdRMS)
    {
        TArray<FOffgridAIAudioPhraseGap> Out;
        if (Features.Num() <= 0 || SpeechEndSeconds <= SpeechStartSeconds + 0.20f)
        {
            return Out;
        }

        // Conservative pause detector: only accept sustained valleys inside the
        // detected speech span. These gaps are phrase anchors, not phoneme locks.
        const float LowRMSNormThreshold = 0.16f;
        const float LowRMSThreshold = FMath::Max(SpeechThresholdRMS * 0.72f, 0.00035f);
        const float MinGapSeconds = 0.060f;
        const float EdgeGuardSeconds = 0.10f;

        int32 GapStart = INDEX_NONE;
        auto FlushGap = [&](int32 GapEndExclusive)
        {
            if (GapStart == INDEX_NONE)
            {
                return;
            }
            const int32 LastIndex = FMath::Clamp(GapEndExclusive - 1, 0, Features.Num() - 1);
            const float StartSeconds = Features[GapStart].TimeSeconds;
            const float EndSeconds = Features[LastIndex].TimeSeconds;
            const float DurationSeconds = EndSeconds - StartSeconds;
            if (DurationSeconds >= MinGapSeconds &&
                StartSeconds > SpeechStartSeconds + EdgeGuardSeconds &&
                EndSeconds < SpeechEndSeconds - EdgeGuardSeconds)
            {
                float MinRMSNorm = 1.0f;
                float MeanRMSNorm = 0.0f;
                int32 Count = 0;
                for (int32 I = GapStart; I <= LastIndex; ++I)
                {
                    MinRMSNorm = FMath::Min(MinRMSNorm, Features[I].SmoothedRMSNorm);
                    MeanRMSNorm += Features[I].SmoothedRMSNorm;
                    ++Count;
                }
                MeanRMSNorm = Count > 0 ? MeanRMSNorm / static_cast<float>(Count) : 1.0f;

                FOffgridAIAudioPhraseGap Gap;
                Gap.StartSeconds = StartSeconds;
                Gap.EndSeconds = EndSeconds;
                Gap.CenterSeconds = (StartSeconds + EndSeconds) * 0.5f;
                Gap.Confidence = FMath::Clamp((DurationSeconds / 0.12f) * (1.0f - MeanRMSNorm), 0.0f, 1.0f);
                if (Gap.Confidence >= 0.20f || DurationSeconds >= 0.095f || MinRMSNorm <= 0.08f)
                {
                    Out.Add(Gap);
                }
            }
            GapStart = INDEX_NONE;
        };

        for (int32 I = 0; I < Features.Num(); ++I)
        {
            const FOffgridAIAudioPlannerFeaturePoint& Point = Features[I];
            const bool bInsideSpeech = Point.TimeSeconds >= SpeechStartSeconds && Point.TimeSeconds <= SpeechEndSeconds;
            const bool bLow = bInsideSpeech && (Point.RMS < LowRMSThreshold || Point.SmoothedRMSNorm <= LowRMSNormThreshold);
            if (bLow)
            {
                if (GapStart == INDEX_NONE)
                {
                    GapStart = I;
                }
            }
            else
            {
                FlushGap(I);
            }
        }
        FlushGap(Features.Num());

        Out.Sort([](const FOffgridAIAudioPhraseGap& A, const FOffgridAIAudioPhraseGap& B)
        {
            return A.CenterSeconds < B.CenterSeconds;
        });
        return Out;
    }

    static TArray<float> BuildPhraseAudioBoundaryTimes(
        const TArray<FOffgridAITextPhraseBoundary>& TextBoundaries,
        const TArray<FOffgridAIAudioPhraseGap>& AudioGaps,
        float SpeechStartSeconds,
        float SpeechEndSeconds)
    {
        TArray<float> Out;
        if (TextBoundaries.Num() <= 0 || SpeechEndSeconds <= SpeechStartSeconds + 0.20f)
        {
            return Out;
        }

        const float SpeechDuration = SpeechEndSeconds - SpeechStartSeconds;
        TSet<int32> UsedGaps;
        for (const FOffgridAITextPhraseBoundary& Boundary : TextBoundaries)
        {
            const float ExpectedGlobalTime = SpeechStartSeconds + FMath::Clamp(Boundary.TextBoundaryNorm, 0.0f, 1.0f) * SpeechDuration;
            int32 BestGapIndex = INDEX_NONE;
            float BestScore = TNumericLimits<float>::Max();
            for (int32 GapIndex = 0; GapIndex < AudioGaps.Num(); ++GapIndex)
            {
                if (UsedGaps.Contains(GapIndex))
                {
                    continue;
                }
                const FOffgridAIAudioPhraseGap& Gap = AudioGaps[GapIndex];
                const float Delta = FMath::Abs(Gap.CenterSeconds - ExpectedGlobalTime);
                // Phrase anchors should be nearby in the global model. This avoids
                // v32k/H-style rubber-banding from a wrong local event/gap.
                const float MaxAllowedDelta = FMath::Clamp(SpeechDuration * 0.10f, 0.14f, 0.32f);
                if (Delta > MaxAllowedDelta)
                {
                    continue;
                }
                const float Score = Delta - Gap.Confidence * 0.030f;
                if (Score < BestScore)
                {
                    BestScore = Score;
                    BestGapIndex = GapIndex;
                }
            }

            if (BestGapIndex != INDEX_NONE)
            {
                UsedGaps.Add(BestGapIndex);
                Out.Add(AudioGaps[BestGapIndex].CenterSeconds);
            }
            else
            {
                // Fallback boundary preserves the global affine mapping for this
                // phrase boundary. Phrase warping is opportunistic, not mandatory.
                Out.Add(ExpectedGlobalTime);
            }
        }

        // Enforce monotonic audio phrase boundaries. If detection produced a bad
        // order, collapse back toward the global affine boundary rather than
        // letting phrase-local timing invert or stretch aggressively.
        float Prev = SpeechStartSeconds + 0.08f;
        for (int32 I = 0; I < Out.Num(); ++I)
        {
            const float MaxForBoundary = SpeechEndSeconds - 0.08f * static_cast<float>(Out.Num() - I);
            Out[I] = FMath::Clamp(Out[I], Prev + 0.06f, MaxForBoundary);
            Prev = Out[I];
        }
        return Out;
    }

    static float MapTextNormToAudioSecondsWithPhraseBoundaries(
        float CenterNorm,
        const TArray<FOffgridAITextPhraseBoundary>& TextBoundaries,
        const TArray<float>& AudioBoundaries,
        float SpeechStartSeconds,
        float SpeechEndSeconds)
    {
        CenterNorm = FMath::Clamp(CenterNorm, 0.0f, 1.0f);
        const float SpeechDuration = FMath::Max(SpeechEndSeconds - SpeechStartSeconds, 0.10f);
        if (TextBoundaries.Num() <= 0 || TextBoundaries.Num() != AudioBoundaries.Num())
        {
            return SpeechStartSeconds + CenterNorm * SpeechDuration;
        }

        float TextA = 0.0f;
        float AudioA = SpeechStartSeconds;
        for (int32 BoundaryIndex = 0; BoundaryIndex <= TextBoundaries.Num(); ++BoundaryIndex)
        {
            const bool bFinalSpan = BoundaryIndex == TextBoundaries.Num();
            const float TextB = bFinalSpan ? 1.0f : FMath::Clamp(TextBoundaries[BoundaryIndex].TextBoundaryNorm, TextA + 0.01f, 1.0f);
            const float AudioB = bFinalSpan ? SpeechEndSeconds : AudioBoundaries[BoundaryIndex];
            if (CenterNorm <= TextB || bFinalSpan)
            {
                const float LocalAlpha = (TextB > TextA + 0.001f) ? (CenterNorm - TextA) / (TextB - TextA) : 0.0f;
                return FMath::Lerp(AudioA, AudioB, FMath::Clamp(LocalAlpha, 0.0f, 1.0f));
            }
            TextA = TextB;
            AudioA = AudioB;
        }
        return SpeechStartSeconds + CenterNorm * SpeechDuration;
    }

    static float AudioPlannerAverageRMS(const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features, int32 First, int32 Last)
    {
        if (Features.Num() <= 0)
        {
            return 0.0f;
        }
        First = FMath::Clamp(First, 0, Features.Num() - 1);
        Last = FMath::Clamp(Last, 0, Features.Num() - 1);
        if (Last < First)
        {
            Swap(First, Last);
        }
        float Sum = 0.0f;
        for (int32 I = First; I <= Last; ++I)
        {
            Sum += Features[I].RMSNorm;
        }
        return Sum / static_cast<float>(FMath::Max(Last - First + 1, 1));
    }

    static float AudioPlannerMaxFlux(const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features, int32 First, int32 Last)
    {
        if (Features.Num() <= 0)
        {
            return 0.0f;
        }
        First = FMath::Clamp(First, 0, Features.Num() - 1);
        Last = FMath::Clamp(Last, 0, Features.Num() - 1);
        if (Last < First)
        {
            Swap(First, Last);
        }
        float MaxFlux = 0.0f;
        for (int32 I = First; I <= Last; ++I)
        {
            MaxFlux = FMath::Max(MaxFlux, Features[I].Flux);
        }
        return MaxFlux;
    }

    static float AudioPlannerSustainedHighZCR(const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features, int32 Index)
    {
        if (!Features.IsValidIndex(Index))
        {
            return 0.0f;
        }
        float Sum = 0.0f;
        int32 Count = 0;
        for (int32 I = FMath::Max(Index - 2, 0); I <= FMath::Min(Index + 2, Features.Num() - 1); ++I)
        {
            Sum += FMath::Clamp(Features[I].ZCR * 5.0f, 0.0f, 1.0f);
            ++Count;
        }
        return Count > 0 ? Sum / static_cast<float>(Count) : 0.0f;
    }

    static float AudioPlannerLocalRMSPeakScore(const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features, int32 Index)
    {
        if (!Features.IsValidIndex(Index))
        {
            return 0.0f;
        }
        const float Prev = AudioPlannerAverageRMS(Features, Index - 5, Index - 2);
        const float Next = AudioPlannerAverageRMS(Features, Index + 2, Index + 5);
        const float Surround = FMath::Max(Prev, Next);
        return FMath::Clamp((Features[Index].RMSNorm - Surround + 0.10f) / 0.35f, 0.0f, 1.0f);
    }

    static float AudioPlannerLocalRMSValleyScore(const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features, int32 Index)
    {
        if (!Features.IsValidIndex(Index))
        {
            return 0.0f;
        }
        const float Prev = AudioPlannerAverageRMS(Features, Index - 8, Index - 3);
        const float Next = AudioPlannerAverageRMS(Features, Index + 3, Index + 8);
        const float Shoulder = FMath::Min(Prev, Next);
        return FMath::Clamp((Shoulder - Features[Index].RMSNorm + 0.12f) / 0.45f, 0.0f, 1.0f);
    }

    static float AudioPlannerVowelShapeScoreForPose(FName PoseID, const FOffgridAIAudioPlannerFeaturePoint& Point)
    {
        const FString S = PoseID.ToString();
        const float Total = FMath::Max(Point.SmoothedLowProxy + Point.SmoothedMidProxy + Point.SmoothedHighProxy, 0.0001f);
        const float Low = Point.SmoothedLowProxy / Total;
        const float Mid = Point.SmoothedMidProxy / Total;
        const float High = Point.SmoothedHighProxy / Total;
        const float Centroid = Point.SmoothedCentroidProxy;

        if (S.Contains(TEXT("Ee"), ESearchCase::IgnoreCase))
        {
            return FMath::Clamp(High * 0.35f + Mid * 0.30f + Centroid * 0.35f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Ih"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Eh"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ay"), ESearchCase::IgnoreCase))
        {
            return FMath::Clamp(Mid * 0.42f + Centroid * 0.28f + High * 0.18f + (1.0f - FMath::Abs(Centroid - 0.55f)) * 0.12f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Oo"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Or"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Oh"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Rr"), ESearchCase::IgnoreCase))
        {
            return FMath::Clamp(Low * 0.48f + (1.0f - Centroid) * 0.34f + Mid * 0.18f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Aa"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ah"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Uh"), ESearchCase::IgnoreCase))
        {
            return FMath::Clamp(Low * 0.28f + Mid * 0.38f + (1.0f - FMath::Abs(Centroid - 0.42f)) * 0.34f, 0.0f, 1.0f);
        }
        return 0.50f;
    }

    static float AudioPlannerNegativeEvidenceForPose(FName PoseID, const FOffgridAIAudioPlannerFeaturePoint& Point, const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features, int32 Index)
    {
        const FString S = PoseID.ToString();
        const float LocalPeak = AudioPlannerLocalRMSPeakScore(Features, Index);
        const float LocalValley = AudioPlannerLocalRMSValleyScore(Features, Index);
        const float ExpectedVowel = AudioPlannerExpectedVowelFamilyForPose(PoseID).IsEmpty() ? 0.0f : 1.0f;
        float Penalty = 0.0f;

        if (S.Contains(TEXT("MBP"), ESearchCase::IgnoreCase))
        {
            // A bilabial should not be centered in a loud vowel nucleus.
            if (Point.SmoothedRMSNorm > 0.58f && LocalValley < 0.30f)
            {
                Penalty += 0.38f;
            }
            if (Point.FricativeContinuity > 0.70f)
            {
                Penalty += 0.18f;
            }
        }
        else if (S.Contains(TEXT("FV"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase))
        {
            // Fricatives should not be pure vowel nuclei.
            if (Point.FricativeContinuity < 0.34f && LocalPeak > 0.55f)
            {
                Penalty += 0.32f;
            }
            if (Point.SmoothedFlatnessProxy < 0.24f && Point.bLocalRMSPeak)
            {
                Penalty += 0.16f;
            }
            if (Point.SmoothedHighProxy < 0.10f)
            {
                Penalty += 0.16f;
            }
        }
        else if (S.Contains(TEXT("Ww"), ESearchCase::IgnoreCase))
        {
            if (Point.SmoothedCentroidProxy > 0.62f || Point.FricativeContinuity > 0.62f)
            {
                Penalty += 0.30f;
            }
        }
        else if (ExpectedVowel > 0.0f)
        {
            const FString ExpectedFamily = AudioPlannerExpectedVowelFamilyForPose(PoseID);
            const bool bFamilyMatch = Point.VowelFamilyGuess.Equals(ExpectedFamily, ESearchCase::IgnoreCase);
            if (!bFamilyMatch)
            {
                Penalty += 0.22f;
            }
            if (Point.FricativeContinuity > 0.62f || Point.SmoothedFlatnessProxy > 0.88f)
            {
                Penalty += 0.24f;
            }
        }

        return FMath::Clamp(Penalty, 0.0f, 0.85f);
    }

    static FString AudioPlannerRejectionReasonForPose(FName PoseID, const FOffgridAIAudioPlannerFeaturePoint* Point, const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features, int32 Index, float BestConfidence, float RequiredConfidence)
    {
        if (!Point || !Features.IsValidIndex(Index))
        {
            return TEXT("No feature point in local search window");
        }

        const FString S = PoseID.ToString();
        const FString ExpectedFamily = AudioPlannerExpectedVowelFamilyForPose(PoseID);
        if (S.Contains(TEXT("MBP"), ESearchCase::IgnoreCase))
        {
            if (!Point->bLocalRMSValley && AudioPlannerLocalRMSValleyScore(Features, Index) < 0.35f)
            {
                return TEXT("RejectedBecauseNoClosureValley");
            }
            if (AudioPlannerMaxFlux(Features, Index, Index + 8) < 0.25f)
            {
                return TEXT("RejectedBecauseNoReleaseFlux");
            }
        }
        if (S.Contains(TEXT("FV"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase))
        {
            if (Point->FricativeContinuity < 0.38f)
            {
                return TEXT("RejectedBecauseNoFricativeContinuity");
            }
        }
        if (!ExpectedFamily.IsEmpty() && !Point->VowelFamilyGuess.Equals(ExpectedFamily, ESearchCase::IgnoreCase))
        {
            return TEXT("RejectedBecauseWrongVowelFamily");
        }
        if (BestConfidence < RequiredConfidence)
        {
            return TEXT("RejectedBecauseLowConfidence");
        }
        return TEXT("NotRejected");
    }

    static bool IsAudioPlannerStrongVisualLandmark(FName PoseID)
    {
        const FString S = PoseID.ToString();
        return S.Contains(TEXT("MBP"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("FV"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ww"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("03_Ee"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("07_Aa"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("09_Oh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("11_Oo"), ESearchCase::IgnoreCase);
    }

    static float ScoreAudioPlannerFeatureForPose(FName PoseID, const FOffgridAIAudioPlannerFeaturePoint& Point, const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features, int32 Index)
    {
        const FString S = PoseID.ToString();
        const float PrevRMS = AudioPlannerAverageRMS(Features, Index - 8, Index - 3);
        const float NextRMS = AudioPlannerAverageRMS(Features, Index + 3, Index + 8);
        const float FutureFlux = AudioPlannerMaxFlux(Features, Index, Index + 8);
        const float FutureRise = FMath::Clamp(NextRMS - Point.RMSNorm, 0.0f, 1.0f);
        const float LocalPeak = AudioPlannerLocalRMSPeakScore(Features, Index);
        const float LocalValley = AudioPlannerLocalRMSValleyScore(Features, Index);
        const float SustainedZCR = AudioPlannerSustainedHighZCR(Features, Index);
        const float LowDominance = Point.SmoothedLowProxy / FMath::Max(Point.SmoothedLowProxy + Point.SmoothedMidProxy + Point.SmoothedHighProxy, 0.0001f);
        const float MidHigh = FMath::Clamp(Point.SmoothedMidProxy * 0.45f + Point.SmoothedHighProxy * 0.55f, 0.0f, 1.0f);
        const float StructurePeak = Point.bLocalRMSPeak ? 1.0f : LocalPeak;
        const float StructureValley = Point.bLocalRMSValley ? 1.0f : LocalValley;
        const float StructureFlux = Point.bLocalFluxPeak ? FMath::Max(Point.SmoothedFlux, 0.75f) : Point.SmoothedFlux;
        const float VoicedVowelish = FMath::Clamp(Point.SmoothedRMSNorm * 0.48f + StructurePeak * 0.30f + (1.0f - Point.FricativeContinuity) * 0.22f, 0.0f, 1.0f);

        if (S.Contains(TEXT("MBP"), ESearchCase::IgnoreCase))
        {
            // Bilabial evidence is closure morphology, not just silence: energy shoulders on both sides,
            // a local valley at/near closure, and a following burst/release.
            const float ShoulderEnergy = FMath::Clamp(FMath::Min(PrevRMS, NextRMS) * 1.25f, 0.0f, 1.0f);
            const float Release = FMath::Clamp(FutureFlux * 0.45f + StructureFlux * 0.35f + FutureRise * 0.20f, 0.0f, 1.0f);
            return FMath::Clamp(StructureValley * 0.48f + Release * 0.34f + ShoulderEnergy * 0.18f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("FV"), ESearchCase::IgnoreCase))
        {
            // Labiodental frication: sustained noisy mid/high energy with ZCR, but not a vowel nucleus.
            return FMath::Clamp(SustainedZCR * 0.28f + MidHigh * 0.26f + Point.SmoothedHighProxy * 0.16f + Point.SmoothedFlatnessProxy * 0.16f + Point.SmoothedRolloffProxy * 0.08f + (1.0f - LocalPeak) * 0.06f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase))
        {
            // Palato-alveolar affricates/fricatives: high/mid noise, often with an onset transient.
            return FMath::Clamp(Point.SmoothedHighProxy * 0.26f + Point.SmoothedMidProxy * 0.22f + SustainedZCR * 0.20f + Point.SmoothedFlatnessProxy * 0.16f + Point.Flux * 0.16f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Tongue_Th"), ESearchCase::IgnoreCase))
        {
            // TH is diffuse, usually quieter than S/SH. Prefer ZCR + high noise but penalize very strong sibilant peaks.
            const float NotSibilantPeak = 1.0f - FMath::Clamp(Point.SmoothedHighProxy - Point.SmoothedMidProxy, 0.0f, 1.0f) * 0.35f;
            return FMath::Clamp((SustainedZCR * 0.42f + Point.SmoothedHighProxy * 0.22f + Point.Flux * 0.14f + Point.RMSNorm * 0.12f) * NotSibilantPeak, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Tongue_LNTDS"), ESearchCase::IgnoreCase) || S.Contains(TEXT("TDS"), ESearchCase::IgnoreCase))
        {
            // Mixed alveolar class. T/D favor transient flux; S/Z favor sustained ZCR/high band.
            return FMath::Clamp(Point.Flux * 0.34f + SustainedZCR * 0.28f + Point.SmoothedHighProxy * 0.20f + Point.RMSNorm * 0.10f + LocalPeak * 0.08f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Tongue_KGY"), ESearchCase::IgnoreCase) || S.Contains(TEXT("KGY"), ESearchCase::IgnoreCase))
        {
            // Velar stops are burst/flux-led but less high-sibilant than SH/S.
            return FMath::Clamp(Point.Flux * 0.48f + Point.SmoothedMidProxy * 0.22f + LocalPeak * 0.12f + (1.0f - Point.SmoothedCentroidProxy) * 0.10f + Point.RMSNorm * 0.08f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Ww"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ew"), ESearchCase::IgnoreCase))
        {
            // W is a transition: low centroid plus rising energy. Do not score the whole following rounded vowel equally.
            const float Rising = FMath::Clamp(Point.DeltaRMS * 10.0f + Point.EnergyAcceleration * 14.0f + FutureRise * 0.65f, 0.0f, 1.0f);
            return FMath::Clamp((1.0f - Point.SmoothedCentroidProxy) * 0.26f + LowDominance * 0.24f + Point.SmoothedCepstralTiltProxy * 0.12f + Rising * 0.28f + Point.RMSNorm * 0.10f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Ee"), ESearchCase::IgnoreCase))
        {
            const float Shape = AudioPlannerVowelShapeScoreForPose(PoseID, Point);
            return FMath::Clamp(VoicedVowelish * 0.24f + Shape * 0.48f + Point.SmoothedCentroidProxy * 0.18f + Point.SmoothedHighProxy * 0.10f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Ih"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Eh"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ay"), ESearchCase::IgnoreCase))
        {
            const float Shape = AudioPlannerVowelShapeScoreForPose(PoseID, Point);
            return FMath::Clamp(VoicedVowelish * 0.26f + Shape * 0.46f + Point.SmoothedMidProxy * 0.18f + Point.SmoothedHighProxy * 0.10f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Oo"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Or"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Oh"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Rr"), ESearchCase::IgnoreCase))
        {
            const float Shape = AudioPlannerVowelShapeScoreForPose(PoseID, Point);
            return FMath::Clamp(VoicedVowelish * 0.24f + Shape * 0.50f + LowDominance * 0.18f + (1.0f - Point.SmoothedCentroidProxy) * 0.08f, 0.0f, 1.0f);
        }
        if (S.Contains(TEXT("Aa"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Ah"), ESearchCase::IgnoreCase) || S.Contains(TEXT("Uh"), ESearchCase::IgnoreCase))
        {
            const float Shape = AudioPlannerVowelShapeScoreForPose(PoseID, Point);
            return FMath::Clamp(VoicedVowelish * 0.28f + Shape * 0.48f + Point.SmoothedLowProxy * 0.10f + Point.SmoothedMidProxy * 0.14f, 0.0f, 1.0f);
        }
        return FMath::Clamp(Point.RMSNorm * 0.65f + LocalPeak * 0.35f, 0.0f, 1.0f);
    }

    struct FOffgridAIAU38AnchorMatch
    {
        int32 TrackOrdinal = INDEX_NONE;
        int32 EventIndex = INDEX_NONE;
        int32 IslandIndex = INDEX_NONE;
        int32 GroupIndex = INDEX_NONE;
        FName PoseID = NAME_None;
        FString SourceWord;
        float CommittedCenterSec = 0.0f;
        float CandidateTimeSec = 0.0f;
        float CandidateConfidence = 0.0f;
        float BufferedUsefulLeadSec = 0.0f;
        float DeltaSec = 0.0f;
        bool bMatched = false;
    };

    struct FOffgridAIAU38GroupTransform
    {
        int32 IslandIndex = INDEX_NONE;
        int32 GroupIndex = INDEX_NONE;
        TArray<int32> TrackOrdinals;
        TArray<FOffgridAIAU38AnchorMatch> UsableAnchors;
        float OriginalStartSec = FLT_MAX;
        float OriginalEndSec = -FLT_MAX;
        float Scale = 1.0f;
        float ShiftSec = 0.0f;
        float ForwardShiftSec = 0.0f;
        FName Mode = TEXT("identity");
    };

    static FString AU38GroupKey(int32 IslandIndex, int32 GroupIndex)
    {
        return FString::Printf(TEXT("%d:%d"), IslandIndex, GroupIndex);
    }

    static int32 AU38GetEventProsodyGroupIndex(const FOffgridAIAlignedVisemeEvent& Event)
    {
        if (Event.PlannerProsodyGroupIndex != INDEX_NONE)
        {
            return Event.PlannerProsodyGroupIndex;
        }
        if (Event.ProsodyGroupIndex != INDEX_NONE)
        {
            return Event.ProsodyGroupIndex;
        }
        return Event.PhraseIndex;
    }

    static float AU38ApplyGroupTransform(const FOffgridAIAU38GroupTransform& Group, float CenterSec)
    {
        return (CenterSec * Group.Scale) + Group.ShiftSec + Group.ForwardShiftSec;
    }

    static void WriteAU39RuntimeTopologyMetricsCsv(const FString& LineDebugDirectory, const FOffgridAIAlignedVisemeTrack& Track, float PrerollSec)
    {
        if (LineDebugDirectory.IsEmpty())
        {
            return;
        }

        TArray<float> GapsMs;
        GapsMs.Reserve(FMath::Max(0, Track.Events.Num() - 1));
        int32 DenseGapCount = 0;
        int32 LargeGapCount = 0;
        int32 NegativeGapCount = 0;
        const float MinHealthyGapMs = FMath::Max(25.0f, PrerollSec * 1000.0f * 0.18f);
        const float LargeGapThresholdMs = FMath::Max(110.0f, PrerollSec * 1000.0f * 0.75f);

        for (int32 I = 1; I < Track.Events.Num(); ++I)
        {
            const float GapMs = (Track.Events[I].FinalRenderCenterSeconds - Track.Events[I - 1].FinalRenderCenterSeconds) * 1000.0f;
            GapsMs.Add(GapMs);
            if (GapMs < -0.5f)
            {
                ++NegativeGapCount;
            }
            if (GapMs >= 0.0f && GapMs < MinHealthyGapMs)
            {
                ++DenseGapCount;
            }
            if (GapMs > LargeGapThresholdMs)
            {
                ++LargeGapCount;
            }
        }

        auto Percentile = [](TArray<float> Values, float P) -> float
        {
            if (Values.Num() <= 0)
            {
                return 0.0f;
            }
            Values.Sort();
            const float Pos = FMath::Clamp(P, 0.0f, 1.0f) * static_cast<float>(Values.Num() - 1);
            const int32 Lo = FMath::Clamp(FMath::FloorToInt(Pos), 0, Values.Num() - 1);
            const int32 Hi = FMath::Clamp(Lo + 1, 0, Values.Num() - 1);
            const float T = Pos - static_cast<float>(Lo);
            return FMath::Lerp(Values[Lo], Values[Hi], T);
        };

        float MinGapMs = GapsMs.Num() > 0 ? TNumericLimits<float>::Max() : 0.0f;
        float MaxGapMs = 0.0f;
        float SumAbsGapMs = 0.0f;
        for (float GapMs : GapsMs)
        {
            MinGapMs = FMath::Min(MinGapMs, GapMs);
            MaxGapMs = FMath::Max(MaxGapMs, GapMs);
            SumAbsGapMs += FMath::Abs(GapMs);
        }
        const float MeanAbsGapMs = GapsMs.Num() > 0 ? SumAbsGapMs / static_cast<float>(GapsMs.Num()) : 0.0f;

        FString CSV;
        CSV.Reserve(1024);
        CSV += FString::Printf(TEXT("AU39EventCount,%d\n"), Track.Events.Num());
        CSV += FString::Printf(TEXT("AU39GapCount,%d\n"), GapsMs.Num());
        CSV += FString::Printf(TEXT("AU39MinEventGapMs,%.3f\n"), MinGapMs);
        CSV += FString::Printf(TEXT("AU39P50EventGapMs,%.3f\n"), Percentile(GapsMs, 0.50f));
        CSV += FString::Printf(TEXT("AU39P90EventGapMs,%.3f\n"), Percentile(GapsMs, 0.90f));
        CSV += FString::Printf(TEXT("AU39MaxEventGapMs,%.3f\n"), MaxGapMs);
        CSV += FString::Printf(TEXT("AU39MeanAbsEventGapMs,%.3f\n"), MeanAbsGapMs);
        CSV += FString::Printf(TEXT("AU39DenseGapCount,%d\n"), DenseGapCount);
        CSV += FString::Printf(TEXT("AU39LargeGapCount,%d\n"), LargeGapCount);
        CSV += FString::Printf(TEXT("AU39NegativeGapCount,%d\n"), NegativeGapCount);
        CSV += FString::Printf(TEXT("AU39MinHealthyGapMs,%.3f\n"), MinHealthyGapMs);
        CSV += FString::Printf(TEXT("AU39LargeGapThresholdMs,%.3f\n"), LargeGapThresholdMs);
        CSV += TEXT("AU39RuntimeReflowDisabled,1\n");
        FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(LineDebugDirectory, TEXT("au39_runtime_topology_metrics.csv")));
    }

    static void AU38ResetCounters(
        bool& bApplied,
        int32& GroupCount,
        int32& AffineGroupCount,
        int32& SingleAnchorGroupCount,
        int32& ForwardShiftGroupCount,
        int32& AppliedEventCount,
        int32& AnchorCount,
        float& MeanAbsEventDeltaMs,
        float& MaxAbsEventDeltaMs)
    {
        bApplied = false;
        GroupCount = 0;
        AffineGroupCount = 0;
        SingleAnchorGroupCount = 0;
        ForwardShiftGroupCount = 0;
        AppliedEventCount = 0;
        AnchorCount = 0;
        MeanAbsEventDeltaMs = 0.0f;
        MaxAbsEventDeltaMs = 0.0f;
    }

    static bool AU38FindBestAnchorForEvent(
        const FOffgridAIAlignedVisemeEvent& Event,
        int32 TrackOrdinal,
        const TArray<FOffgridAIAudioPlannerFeaturePoint>& Features,
        float AudioTimeBaseSec,
        float VisibleAudioEndSec,
        float PrerollSec,
        FOffgridAIAU38AnchorMatch& OutMatch)
    {
        OutMatch = FOffgridAIAU38AnchorMatch();
        OutMatch.TrackOrdinal = TrackOrdinal;
        OutMatch.EventIndex = Event.EventIndex;
        OutMatch.IslandIndex = Event.TextIslandIndex;
        OutMatch.GroupIndex = AU38GetEventProsodyGroupIndex(Event);
        OutMatch.PoseID = Event.PoseID;
        OutMatch.SourceWord = Event.SourceWord;
        OutMatch.CommittedCenterSec = Event.FinalRenderCenterSeconds;

        if (!IsAudioPlannerStrongVisualLandmark(Event.PoseID) || Features.Num() <= 0 || PrerollSec <= 0.0f)
        {
            return false;
        }

        const float MatchWindowSec = FMath::Max(0.035f, PrerollSec * 0.75f);
        const float SearchLo = FMath::Max(0.0f, Event.FinalRenderCenterSeconds - MatchWindowSec);
        const float SearchHi = FMath::Min(VisibleAudioEndSec, Event.FinalRenderCenterSeconds + MatchWindowSec);
        if (SearchHi <= SearchLo)
        {
            return false;
        }

        float BestScore = 0.0f;
        int32 BestIndex = INDEX_NONE;
        for (int32 FeatureIndex = 0; FeatureIndex < Features.Num(); ++FeatureIndex)
        {
            const FOffgridAIAudioPlannerFeaturePoint& Point = Features[FeatureIndex];
            const float PointTimeSec = AudioTimeBaseSec + Point.TimeSeconds;
            if (PointTimeSec < SearchLo || PointTimeSec > SearchHi || PointTimeSec > VisibleAudioEndSec)
            {
                continue;
            }

            const float PoseScore = ScoreAudioPlannerFeatureForPose(Event.PoseID, Point, Features, FeatureIndex);
            const float DistanceNorm = FMath::Clamp(FMath::Abs(PointTimeSec - Event.FinalRenderCenterSeconds) / MatchWindowSec, 0.0f, 1.0f);
            const float DistanceScore = 1.0f - DistanceNorm;
            const float Score = PoseScore * (0.72f + 0.28f * DistanceScore);
            if (Score > BestScore)
            {
                BestScore = Score;
                BestIndex = FeatureIndex;
            }
        }

        const float RequiredConfidence = IsLabVowelPose(Event.PoseID) ? 0.44f : 0.40f;
        if (!Features.IsValidIndex(BestIndex) || BestScore < RequiredConfidence)
        {
            return false;
        }

        const float CandidateTimeSec = AudioTimeBaseSec + Features[BestIndex].TimeSeconds;
        const float CandidateAvailableSec = CandidateTimeSec + FMath::Max(0.008f, PrerollSec / 6.0f);
        const float BufferedUsefulLeadSec = Event.FinalRenderCenterSeconds + PrerollSec - CandidateAvailableSec;
        if (BufferedUsefulLeadSec < FMath::Max(0.025f, PrerollSec / 3.0f))
        {
            return false;
        }

        OutMatch.CandidateTimeSec = CandidateTimeSec;
        OutMatch.CandidateConfidence = BestScore;
        OutMatch.BufferedUsefulLeadSec = BufferedUsefulLeadSec;
        OutMatch.DeltaSec = CandidateTimeSec - Event.FinalRenderCenterSeconds;
        OutMatch.bMatched = true;
        return true;
    }

    constexpr bool DefaultEnableSpatialization = true;
    const FVector DefaultAudioSourceOffset = FVector::ZeroVector;
    constexpr float DefaultPlaybackVolumeMultiplier = 1.0f;

    constexpr float DefaultLipsyncOpenThreshold = 0.08f;
    constexpr float DefaultLipsyncCloseThreshold = 0.04f;

    static int32 GetDefaultLipsyncFrameSizeForSampleRate(int32 SampleRate)
    {
        // Keep the intended ~21.3 ms analysis window across 24 kHz TTS and
        // 48 kHz BoomOperator loopback. A fixed 512-sample window at 48 kHz is
        // only ~10.7 ms, which is too short for stable voicedness/autocorrelation.
        const int32 SafeSampleRate = FMath::Max(SampleRate, 1);
        const int32 Scaled = FMath::RoundToInt(static_cast<float>(SafeSampleRate) * (512.0f / 24000.0f));
        const int32 Pow2 = FMath::RoundUpToPowerOfTwo(FMath::Max(Scaled, 512));
        return FMath::Clamp(Pow2, 512, 2048);
    }

    static int32 GetDefaultLipsyncHopSizeForSampleRate(int32 SampleRate)
    {
        return FMath::Max(GetDefaultLipsyncFrameSizeForSampleRate(SampleRate) / 2, 128);
    }


    constexpr float DefaultEmotionBlendInSeconds = 0.12f;
    constexpr float DefaultEmotionBlendOutSeconds = 0.30f;

    // Version I: this tracks whether a LineCoach has already resolved its first-line
    // emotion presentation. It deliberately lives in this cpp so we do not add a
    // serialized UObject field for a purely runtime guard. EndPlay removes entries.
    static TSet<const UOffgridAILineCoach*> GInitialDominantEmotionPresentedLineCoaches;

    static float Clamp01(float Value)
    {
        return FMath::Clamp(Value, 0.0f, 1.0f);
    }

    static bool AreEmotionMapsNearlyEqual(const TMap<FName, float>& A, const TMap<FName, float>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }

        for (const TPair<FName, float>& Pair : A)
        {
            const float* Other = B.Find(Pair.Key);
            if (!Other || !FMath::IsNearlyEqual(Pair.Value, *Other, KINDA_SMALL_NUMBER))
            {
                return false;
            }
        }

        return true;
    }

    static bool AreFloatMapsNearlyEqual(const TMap<FName, float>& A, const TMap<FName, float>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }

        for (const TPair<FName, float>& Pair : A)
        {
            const float* Other = B.Find(Pair.Key);
            if (!Other || !FMath::IsNearlyEqual(Pair.Value, *Other, 0.001f))
            {
                return false;
            }
        }

        return true;
    }

    static float SmoothStep01(float Edge0, float Edge1, float X)
    {
        if (FMath::IsNearlyEqual(Edge0, Edge1))
        {
            return X >= Edge1 ? 1.0f : 0.0f;
        }

        const float T = Clamp01((X - Edge0) / (Edge1 - Edge0));
        return T * T * (3.0f - 2.0f * T);
    }


    static bool IsPoseIDLike(FName PoseID, const TCHAR* Token)
    {
        return PoseID.ToString().Contains(Token, ESearchCase::IgnoreCase);
    }

    static bool IsProtectedSpeechLandmarkPose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        return PoseID == TEXT("22_MBP") ||
            PoseID == TEXT("20_FV") ||
            PoseID == TEXT("24_Tongue_Th") ||
            PoseID == TEXT("14_ChJjSh") ||
            PoseID == TEXT("03_Ee") ||
            S.Contains(TEXT("TDS"), ESearchCase::IgnoreCase) ||
            S.Contains(TEXT("Tongue"), ESearchCase::IgnoreCase);
    }

    static float V49LeadSecondsForEvent(const FOffgridAITextVisemeEvent& Event)
    {
        const FName PoseID = Event.PoseID;
        if (PoseID == TEXT("22_MBP") || Event.Viseme == EOffgridAITextViseme::MBP) return 0.030f;
        if (PoseID == TEXT("20_FV") || PoseID == TEXT("24_Tongue_Th") || PoseID == TEXT("14_ChJjSh") || Event.Viseme == EOffgridAITextViseme::FVS) return 0.026f;
        if (PoseID == TEXT("03_Ee") || Event.Viseme == EOffgridAITextViseme::EEE) return 0.032f;
        if (PoseID == TEXT("12_Ww-Oo-") || PoseID == TEXT("16_Ww-Ew-") || Event.Viseme == EOffgridAITextViseme::WUH) return 0.026f;
        if (PoseID == TEXT("11_Oo") || PoseID == TEXT("10_Or") || PoseID == TEXT("09_Oh") || Event.Viseme == EOffgridAITextViseme::OOO) return 0.028f;
        return 0.018f;
    }

    static float V49HoldSecondsForEvent(const FOffgridAITextVisemeEvent& Event)
    {
        const FName PoseID = Event.PoseID;
        if (PoseID == TEXT("22_MBP") || Event.Viseme == EOffgridAITextViseme::MBP) return 0.050f;
        if (PoseID == TEXT("20_FV") || PoseID == TEXT("24_Tongue_Th") || PoseID == TEXT("14_ChJjSh") || Event.Viseme == EOffgridAITextViseme::FVS) return 0.044f;
        if (PoseID == TEXT("03_Ee") || Event.Viseme == EOffgridAITextViseme::EEE) return 0.060f;
        if (PoseID == TEXT("12_Ww-Oo-") || Event.Viseme == EOffgridAITextViseme::WUH) return 0.024f;
        if (PoseID == TEXT("16_Ww-Ew-")) return 0.042f;
        if (PoseID == TEXT("11_Oo") || PoseID == TEXT("10_Or") || PoseID == TEXT("09_Oh") || Event.Viseme == EOffgridAITextViseme::OOO) return 0.050f;
        return 0.052f;
    }

    static float V49FalloffSecondsForEvent(const FOffgridAITextVisemeEvent& Event)
    {
        const FName PoseID = Event.PoseID;
        if (PoseID == TEXT("22_MBP") || Event.Viseme == EOffgridAITextViseme::MBP) return 0.030f;
        if (PoseID == TEXT("20_FV") || PoseID == TEXT("24_Tongue_Th") || PoseID == TEXT("14_ChJjSh") || Event.Viseme == EOffgridAITextViseme::FVS) return 0.032f;
        if (PoseID == TEXT("03_Ee") || Event.Viseme == EOffgridAITextViseme::EEE) return 0.040f;
        if (PoseID == TEXT("12_Ww-Oo-") || Event.Viseme == EOffgridAITextViseme::WUH) return 0.022f;
        if (PoseID == TEXT("16_Ww-Ew-")) return 0.032f;
        return 0.038f;
    }

    static void V49AddWeight(TMap<FName, float>& Out, FName PoseID, float Weight)
    {
        Weight = Clamp01(Weight);
        if (Weight <= 0.001f || PoseID == NAME_None)
        {
            return;
        }
        float& Existing = Out.FindOrAdd(PoseID);
        Existing = FMath::Max(Existing, Weight);
    }


    struct FOffgridAIV15PerformedVisemeEvent
    {
        int32 EventIndex = INDEX_NONE;
        FString SourceText;
        FName PlannedPoseID = NAME_None;
        float TextCenterSec = 0.0f;
        // Authoritative layer-2 event center, in speech-relative playback seconds.
        // This mirrors FOffgridAIAlignedVisemeEvent::FinalRenderCenterSeconds.
        float FinalRenderCenterSec = 0.0f;
        float EnvelopePeakSec = 0.0f;
        float PerformancePeakSec = 0.0f;
        float PlannedStrength = 0.0f;
        float EnvelopeWeight = 0.0f;
        float SubmittedStrength = 0.0f;
        bool bIsLandmark = false;
        FString Result;
        TMap<FName, float> PoseWeights;
    };

    static bool V15IsMBPEvent(const FOffgridAITextVisemeEvent& Event)
    {
        return Event.PoseID == TEXT("22_MBP") || Event.Viseme == EOffgridAITextViseme::MBP;
    }

    static bool V15IsTDSEvent(const FOffgridAITextVisemeEvent& Event)
    {
        const FString S = Event.PoseID.ToString();
        return S.Contains(TEXT("TDS"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Tongue"), ESearchCase::IgnoreCase);
    }

    static FString V19NormalizeSourceWordForTDSTransient(FString Word)
    {
        Word.TrimStartAndEndInline();
        Word = Word.ToLower();
        while (Word.Len() > 0 && !FChar::IsAlnum(Word[0]))
        {
            Word.RightChopInline(1, EAllowShrinking::No);
        }
        while (Word.Len() > 0 && !FChar::IsAlnum(Word[Word.Len() - 1]))
        {
            Word.LeftChopInline(1, EAllowShrinking::No);
        }
        return Word;
    }

    static bool V19ShouldAddInitialTDSTransientForWord(const FString& NormalizedWord)
    {
        if (NormalizedWord.IsEmpty())
        {
            return false;
        }

        // Intentionally small/explicit. This is a layer-1 symbolic event, not
        // a timing correction. The aligned track will still own final runtime time.
        static const TSet<FString> ExplicitTDWords =
        {
            // K2: keep only intentional, visibly useful alveolar starts. Earlier
            // versions injected TDS into short function words and soft/unstressed
            // words such as "to", "today", "do", "day", and "dollars";
            // K logs showed those as low-value mouth pops rather than readable
            // articulation. TH words are handled separately by the text planner.
            TEXT("take"), TEXT("takes"), TEXT("taking"),
            TEXT("talk"), TEXT("talks"), TEXT("talking"),
            TEXT("tell"), TEXT("tells")
        };
        return ExplicitTDWords.Contains(NormalizedWord);
    }

    static void V19AddInitialTDSTransientsToPlan(FOffgridAITextVisemePlan& Plan)
    {
        if (Plan.Events.Num() <= 0)
        {
            return;
        }

        TMap<FString, int32> FirstEventByWord;
        for (int32 Index = 0; Index < Plan.Events.Num(); ++Index)
        {
            const FString Word = V19NormalizeSourceWordForTDSTransient(Plan.Events[Index].SourceText);
            if (!V19ShouldAddInitialTDSTransientForWord(Word))
            {
                continue;
            }
            int32* Existing = FirstEventByWord.Find(Word);
            if (!Existing || Plan.Events[Index].StartNorm < Plan.Events[*Existing].StartNorm)
            {
                FirstEventByWord.Add(Word, Index);
            }
        }

        TArray<FOffgridAITextVisemeEvent> Added;
        for (const TPair<FString, int32>& Pair : FirstEventByWord)
        {
            const FOffgridAITextVisemeEvent& First = Plan.Events[Pair.Value];
            bool bAlreadyHasNearbyTDS = false;
            for (const FOffgridAITextVisemeEvent& Existing : Plan.Events)
            {
                if (V15IsTDSEvent(Existing) && FMath::Abs(Existing.StartNorm - First.StartNorm) < 0.020f)
                {
                    bAlreadyHasNearbyTDS = true;
                    break;
                }
            }
            if (bAlreadyHasNearbyTDS)
            {
                continue;
            }

            FOffgridAITextVisemeEvent TDS;
            TDS.StartNorm = FMath::Clamp(First.StartNorm, 0.0f, 1.0f);
            TDS.EndNorm = FMath::Clamp(First.StartNorm + 0.018f, TDS.StartNorm, 1.0f);
            TDS.Viseme = EOffgridAITextViseme::FVS;
            TDS.PoseID = TEXT("01_TDS-Ah-");
            TDS.Strength = FMath::Max(First.Strength * 0.72f, 0.42f);
            TDS.SourceText = First.SourceText;
            Added.Add(TDS);
        }

        if (Added.Num() <= 0)
        {
            return;
        }

        Plan.Events.Append(Added);
        Plan.Events.Sort([](const FOffgridAITextVisemeEvent& A, const FOffgridAITextVisemeEvent& B)
        {
            if (!FMath::IsNearlyEqual(A.StartNorm, B.StartNorm))
            {
                return A.StartNorm < B.StartNorm;
            }
            const bool bATDS = V15IsTDSEvent(A);
            const bool bBTDS = V15IsTDSEvent(B);
            if (bATDS != bBTDS)
            {
                return bATDS;
            }
            return A.EndNorm < B.EndNorm;
        });
    }


    static float VFTextEventCenterNorm(const FOffgridAITextVisemeEvent& Event)
    {
        return FMath::Clamp((Event.StartNorm + Event.EndNorm) * 0.5f, 0.0f, 1.0f);
    }

    static bool VFIsMBPEvent(const FOffgridAITextVisemeEvent& Event)
    {
        return Event.PoseID == TEXT("22_MBP") || Event.Viseme == EOffgridAITextViseme::MBP;
    }

    static bool VFIsFricativeOrTongueEvent(const FOffgridAITextVisemeEvent& Event)
    {
        const FString S = Event.PoseID.ToString();
        return V15IsTDSEvent(Event)
            || Event.PoseID == TEXT("20_FV")
            || Event.PoseID == TEXT("24_Tongue_Th")
            || Event.PoseID == TEXT("14_ChJjSh")
            || Event.Viseme == EOffgridAITextViseme::FVS
            || S.Contains(TEXT("FV"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Tongue"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ch"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Sh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Jj"), ESearchCase::IgnoreCase);
    }

    static bool VFIsRoundOrFunnelEvent(const FOffgridAITextVisemeEvent& Event)
    {
        const FString S = Event.PoseID.ToString();
        return Event.Viseme == EOffgridAITextViseme::OOO
            || Event.Viseme == EOffgridAITextViseme::WUH
            || S.Contains(TEXT("Oo"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Oh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Or"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ww"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ew"), ESearchCase::IgnoreCase);
    }

    static bool VFIsVowelOrRoundEvent(const FOffgridAITextVisemeEvent& Event)
    {
        if (VFIsRoundOrFunnelEvent(Event))
        {
            return true;
        }
        const FString S = Event.PoseID.ToString();
        return Event.Viseme == EOffgridAITextViseme::AAA
            || Event.Viseme == EOffgridAITextViseme::EEE
            || S.Contains(TEXT("Aa"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ah"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Uh"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ee"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ih"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Ay"), ESearchCase::IgnoreCase)
            || S.Contains(TEXT("Eh"), ESearchCase::IgnoreCase);
    }

    static bool VFIsK2SuppressedFunctionWord(const FString& Word)
    {
        return Word == TEXT("a")
            || Word == TEXT("an")
            || Word == TEXT("and")
            || Word == TEXT("is")
            || Word == TEXT("it")
            || Word == TEXT("of")
            || Word == TEXT("the")
            || Word == TEXT("to");
    }

    static bool VFIsK2ProtectedShortPronoun(const FString& Word)
    {
        return Word == TEXT("you")
            || Word == TEXT("your")
            || Word == TEXT("we");
    }

    static bool VFIsPhraseFinalCandidate(const FOffgridAITextVisemeEvent& Event)
    {
        // Layer-1 only approximation: punctuation phrase structure is not carried
        // this far, so only protect events very near the line end. This prevents
        // sentence-final secondary tails from being kept just because they are last.
        return VFTextEventCenterNorm(Event) >= 0.92f;
    }

    static float VFVisualSalienceScore(const FOffgridAITextVisemeEvent& Event)
    {
        float Score = FMath::Clamp(Event.Strength, 0.0f, 1.0f);
        if (VFIsMBPEvent(Event))
        {
            Score += 0.30f;
        }
        else if (VFIsFricativeOrTongueEvent(Event))
        {
            Score += 0.20f;
        }
        else if (VFIsRoundOrFunnelEvent(Event))
        {
            Score += 0.12f;
        }
        return Score;
    }

    static FString VFWordOccurrenceKey(const FOffgridAITextVisemeEvent& Event)
    {
        const FString Word = V19NormalizeSourceWordForTDSTransient(Event.SourceText);
        const int32 Bucket = FMath::RoundToInt(VFTextEventCenterNorm(Event) * 14.0f);
        return FString::Printf(TEXT("%s_%02d"), *Word, Bucket);
    }

    static void VFSalienceReduceTextVisemePlan(FOffgridAITextVisemePlan& Plan)
    {
        // K2: Layer-1 final pass. Keep the complete planner as the linguistic
        // source of truth, then reduce it to confident animation keyframes before
        // Layer 2 assigns runtime timing. This deliberately makes the mouth less
        // busy: fewer, stronger, more readable poses are more tolerant of
        // imperfect streaming timing than dense phoneme-by-phoneme choreography.
        //
        // K2 specifically tightens the Version-F reducer against the K-log
        // failure mode: weak function-word vowels, low-salience secondary vowel
        // tails, and synthetic TDS pops surviving as visible wiggles.
        const int32 OriginalCount = Plan.Events.Num();
        if (OriginalCount <= 0)
        {
            return;
        }

        TSet<int32> Keep;
        TArray<int32> VowelCandidates;
        TMap<FString, int32> BestVowelByWordOccurrence;

        for (int32 Index = 0; Index < Plan.Events.Num(); ++Index)
        {
            const FOffgridAITextVisemeEvent& Event = Plan.Events[Index];
            if (Event.PoseID == NAME_None || Event.Viseme == EOffgridAITextViseme::Rest)
            {
                continue;
            }

            const float Strength = FMath::Clamp(Event.Strength, 0.0f, 1.0f);

            const FString Word = V19NormalizeSourceWordForTDSTransient(Event.SourceText);

            // Always retain strong, visually diagnostic landmarks. These are the
            // events users notice when missing: closures and real teeth/tongue
            // cues. K2 raises the fricative/tongue threshold slightly so weak
            // TDS/TH hints do not read as little jaw pops.
            if (VFIsMBPEvent(Event) && Strength >= 0.55f)
            {
                Keep.Add(Index);
                continue;
            }
            if (VFIsFricativeOrTongueEvent(Event) && Strength >= 0.70f)
            {
                Keep.Add(Index);
                continue;
            }
            if (Event.Viseme == EOffgridAITextViseme::WUH && Strength >= 0.74f)
            {
                Keep.Add(Index);
                continue;
            }

            if (!VFIsVowelOrRoundEvent(Event))
            {
                continue;
            }

            // K2: non-landmark vowel bodies need to be real mouth beats. The
            // previous 0.45 floor preserved too many weak 03_Ee / 08_Ah / 11_Oo
            // / 18_Uh details after smoothing made them visible as wiggles.
            const bool bProtectedPronoun = VFIsK2ProtectedShortPronoun(Word);
            const bool bPhraseFinal = VFIsPhraseFinalCandidate(Event);
            const bool bSuppressedFunctionWord = VFIsK2SuppressedFunctionWord(Word);
            const float RequiredVowelStrength = bProtectedPronoun ? 0.62f : (bSuppressedFunctionWord && !bPhraseFinal ? 0.92f : 0.58f);
            if (Strength < RequiredVowelStrength)
            {
                continue;
            }

            const FString Key = VFWordOccurrenceKey(Event);
            int32* ExistingIndex = BestVowelByWordOccurrence.Find(Key);
            if (!ExistingIndex || VFVisualSalienceScore(Event) > VFVisualSalienceScore(Plan.Events[*ExistingIndex]))
            {
                BestVowelByWordOccurrence.Add(Key, Index);
            }
        }

        BestVowelByWordOccurrence.GenerateValueArray(VowelCandidates);
        VowelCandidates.Sort([&Plan](int32 A, int32 B)
        {
            const float ScoreA = VFVisualSalienceScore(Plan.Events[A]);
            const float ScoreB = VFVisualSalienceScore(Plan.Events[B]);
            if (!FMath::IsNearlyEqual(ScoreA, ScoreB))
            {
                return ScoreA > ScoreB;
            }
            return VFTextEventCenterNorm(Plan.Events[A]) < VFTextEventCenterNorm(Plan.Events[B]);
        });

        TArray<int32> AcceptedVowels;
        const float EstimatedDuration = FMath::Max(Plan.EstimatedDurationSeconds, 0.50f);
        constexpr float MinimumVowelSpacingSeconds = 0.125f;
        for (int32 CandidateIndex : VowelCandidates)
        {
            const FOffgridAITextVisemeEvent& Candidate = Plan.Events[CandidateIndex];
            const float CandidateTime = VFTextEventCenterNorm(Candidate) * EstimatedDuration;

            bool bTooCloseToStrongerVowel = false;
            for (int32 AcceptedIndex : AcceptedVowels)
            {
                const float AcceptedTime = VFTextEventCenterNorm(Plan.Events[AcceptedIndex]) * EstimatedDuration;
                if (FMath::Abs(CandidateTime - AcceptedTime) < MinimumVowelSpacingSeconds)
                {
                    bTooCloseToStrongerVowel = true;
                    break;
                }
            }

            if (!bTooCloseToStrongerVowel)
            {
                Keep.Add(CandidateIndex);
                AcceptedVowels.Add(CandidateIndex);
            }
        }

        // Preserve the final dominant mouth beat only when it is genuinely
        // readable. Do not preserve a weak secondary tail merely because it is the
        // last event in the line; that was one of the K-log end-of-sentence wiggle
        // sources.
        int32 FinalDominantIndex = INDEX_NONE;
        float FinalDominantScore = 0.0f;
        for (int32 Index = Plan.Events.Num() - 1; Index >= 0; --Index)
        {
            const FOffgridAITextVisemeEvent& Event = Plan.Events[Index];
            if (Event.PoseID == NAME_None || Event.Viseme == EOffgridAITextViseme::Rest)
            {
                continue;
            }
            if (!VFIsPhraseFinalCandidate(Event))
            {
                break;
            }

            const float Strength = FMath::Clamp(Event.Strength, 0.0f, 1.0f);
            if (VFIsMBPEvent(Event) && Strength >= 0.55f)
            {
                FinalDominantIndex = Index;
                break;
            }
            if (VFIsFricativeOrTongueEvent(Event) && Strength >= 0.70f)
            {
                FinalDominantIndex = Index;
                break;
            }
            if (VFIsVowelOrRoundEvent(Event) && Strength >= 0.58f)
            {
                const float Score = VFVisualSalienceScore(Event);
                if (Score > FinalDominantScore)
                {
                    FinalDominantScore = Score;
                    FinalDominantIndex = Index;
                }
            }
        }
        if (FinalDominantIndex != INDEX_NONE)
        {
            Keep.Add(FinalDominantIndex);
        }

        TArray<FOffgridAITextVisemeEvent> Reduced;
        Reduced.Reserve(Keep.Num());
        for (int32 Index = 0; Index < Plan.Events.Num(); ++Index)
        {
            if (Keep.Contains(Index))
            {
                Reduced.Add(Plan.Events[Index]);
            }
        }

        if (Reduced.Num() <= 0)
        {
            return;
        }

        UE_LOG(LogOffgridAI, Log, TEXT("[Lipsync][Layer1][K2] salience_reduction events=%d -> %d estimated_duration=%.3f"),
            OriginalCount,
            Reduced.Num(),
            Plan.EstimatedDurationSeconds);

        Plan.Events = MoveTemp(Reduced);
    }

    static float V20TrustedPlanHalfWidthForEvent(const FOffgridAITextVisemeEvent& Event)
    {
        // v20 trusted-plan performer: keep active windows readable but compact.
        // Earlier v19 widened many events until vowels/rounds/funnels overlapped
        // heavily, then hierarchy code suppressed them again. That produced weak,
        // muddy motion. The text planner is now treated as the source of truth:
        // play each event confidently near FinalRenderCenterSeconds and avoid
        // broad defensive overlap.
        const FName PoseID = Event.PoseID;
        if (V15IsTDSEvent(Event)) return 0.045f;
        if (PoseID == TEXT("22_MBP") || Event.Viseme == EOffgridAITextViseme::MBP) return 0.052f;
        if (PoseID == TEXT("20_FV") || PoseID == TEXT("24_Tongue_Th") || PoseID == TEXT("14_ChJjSh") || Event.Viseme == EOffgridAITextViseme::FVS) return 0.052f;
        if (PoseID == TEXT("12_Ww-Oo-") || PoseID == TEXT("16_Ww-Ew-") || Event.Viseme == EOffgridAITextViseme::WUH) return 0.064f;
        if (PoseID == TEXT("11_Oo") || PoseID == TEXT("10_Or") || PoseID == TEXT("09_Oh") || Event.Viseme == EOffgridAITextViseme::OOO) return 0.068f;
        if (PoseID == TEXT("07_Aa") || PoseID == TEXT("08_Ah") || PoseID == TEXT("18_Uh") || Event.Viseme == EOffgridAITextViseme::AAA) return 0.066f;
        if (PoseID == TEXT("03_Ee") || PoseID == TEXT("04_Ih") || PoseID == TEXT("05_Ay") || PoseID == TEXT("06_Eh") || Event.Viseme == EOffgridAITextViseme::EEE) return 0.064f;
        return 0.052f;
    }

    static float V20TrustedPlanPeakFloorForEvent(const FOffgridAITextVisemeEvent& Event)
    {
        // Minimum readable floor only; never a cap and never a substitute for the
        // text-plan strength. If the planner asks for a strong event, layer 3
        // should submit a strong event.
        const FName PoseID = Event.PoseID;
        if (PoseID == TEXT("22_MBP") || Event.Viseme == EOffgridAITextViseme::MBP) return 0.78f;
        if (V15IsTDSEvent(Event)) return 0.42f;
        if (PoseID == TEXT("20_FV") || PoseID == TEXT("24_Tongue_Th") || PoseID == TEXT("14_ChJjSh") || Event.Viseme == EOffgridAITextViseme::FVS) return 0.62f;
        if (PoseID == TEXT("12_Ww-Oo-") || PoseID == TEXT("16_Ww-Ew-") || Event.Viseme == EOffgridAITextViseme::WUH) return 0.55f;
        if (PoseID == TEXT("11_Oo") || PoseID == TEXT("10_Or") || PoseID == TEXT("09_Oh") || Event.Viseme == EOffgridAITextViseme::OOO) return 0.58f;
        if (PoseID == TEXT("07_Aa") || PoseID == TEXT("08_Ah") || PoseID == TEXT("18_Uh") || Event.Viseme == EOffgridAITextViseme::AAA) return 0.62f;
        if (PoseID == TEXT("03_Ee") || PoseID == TEXT("04_Ih") || PoseID == TEXT("05_Ay") || PoseID == TEXT("06_Eh") || Event.Viseme == EOffgridAITextViseme::EEE) return 0.60f;
        return 0.0f;
    }

    static float V16EvaluateMBPClosureEnvelope(float SampleSeconds, float FinalRenderCenterSeconds, float& OutPerformancePeakSeconds)
    {
        // Layer 3 only: MBP is a visible closure impulse around the immutable
        // layer-2 release vicinity. Because the downstream FaceDriver smooths
        // target weights, submit the closure peak slightly earlier than the
        // desired visual peak so the MetaHuman-visible closure lands immediately
        // before the audible B/P/M release. This does not change layer-2 timing.
        constexpr float DriverSmoothingCompensationSeconds = 0.030f;
        const float DesiredVisualLeadSeconds = 0.035f;
        const float Peak = FMath::Max(0.0f, FinalRenderCenterSeconds - DesiredVisualLeadSeconds - DriverSmoothingCompensationSeconds);
        const float CloseStart = FMath::Max(0.0f, Peak - 0.055f);
        const float ReleaseStart = FMath::Max(Peak + 0.018f, FinalRenderCenterSeconds - DriverSmoothingCompensationSeconds);
        const float ReleaseEnd = FinalRenderCenterSeconds + 0.045f;
        OutPerformancePeakSeconds = Peak;

        if (SampleSeconds < CloseStart || SampleSeconds > ReleaseEnd)
        {
            return 0.0f;
        }
        if (SampleSeconds <= Peak)
        {
            return SmoothStep01(CloseStart, Peak, SampleSeconds);
        }
        if (SampleSeconds <= ReleaseStart)
        {
            return 1.0f;
        }
        return 1.0f - SmoothStep01(ReleaseStart, ReleaseEnd, SampleSeconds);
    }

    static float V16EvaluateTDSImpulseEnvelope(float SampleSeconds, float FinalRenderCenterSeconds, float& OutPerformancePeakSeconds)
    {
        // Initial T/D/S-style tongue/alveolar accents need a short visible cue
        // before the audible transient. Keep this as a layer-3 envelope around
        // the layer-2 center; do not move the event.
        constexpr float DriverSmoothingCompensationSeconds = 0.020f;
        const float Peak = FMath::Max(0.0f, FinalRenderCenterSeconds - 0.020f - DriverSmoothingCompensationSeconds);
        const float Start = FMath::Max(0.0f, Peak - 0.045f);
        const float ReleaseEnd = FinalRenderCenterSeconds + 0.045f;
        OutPerformancePeakSeconds = Peak;

        if (SampleSeconds < Start || SampleSeconds > ReleaseEnd)
        {
            return 0.0f;
        }
        if (SampleSeconds <= Peak)
        {
            return SmoothStep01(Start, Peak, SampleSeconds);
        }
        return 1.0f - SmoothStep01(Peak, ReleaseEnd, SampleSeconds);
    }

    static void V15ReducePerformedEventsToPoseWeights(const TArray<FOffgridAIV15PerformedVisemeEvent>& Events, TMap<FName, float>& Out)
    {
        Out.Reset();
        for (const FOffgridAIV15PerformedVisemeEvent& Event : Events)
        {
            for (const TPair<FName, float>& Pair : Event.PoseWeights)
            {
                V49AddWeight(Out, Pair.Key, Pair.Value);
            }
        }
    }

    static FString V50LowerSourceWord(const FOffgridAITextVisemeEvent& Event)
    {
        FString Word = Event.SourceText.ToLower();
        Word.TrimStartAndEndInline();
        return Word;
    }

    static bool V50IsEwTransitionWord(const FOffgridAITextVisemeEvent& Event)
    {
        const FString Word = V50LowerSourceWord(Event);
        return Word.StartsWith(TEXT("new")) ||
            Word.StartsWith(TEXT("you")) ||
            Word.Contains(TEXT("ewe")) ||
            Word.Contains(TEXT("ue")) ||
            Word.Contains(TEXT("ew")) ||
            Word.Contains(TEXT("iew")) ||
            Word.Contains(TEXT("future"));
    }

    static bool V50IsWConsonantWord(const FOffgridAITextVisemeEvent& Event)
    {
        const FString Word = V50LowerSourceWord(Event);
        return Word.StartsWith(TEXT("w")) || Word.StartsWith(TEXT("wh")) || Word.StartsWith(TEXT("qu"));
    }

    static bool V50IsTrueSustainedOoWord(const FOffgridAITextVisemeEvent& Event)
    {
        const FString Word = V50LowerSourceWord(Event);
        return Word.Contains(TEXT("oo")) ||
            Word.Contains(TEXT("food")) ||
            Word.Contains(TEXT("room")) ||
            Word.Contains(TEXT("soon")) ||
            Word.Equals(TEXT("two")) ||
            Word.Equals(TEXT("too"));
    }

    static void V49AddCanonicalPoseWeights(TMap<FName, float>& Out, const FOffgridAITextVisemeEvent& Event, float Weight)
    {
        const FName PoseID = Event.PoseID;
        if (PoseID == TEXT("22_MBP"))
        {
            V49AddWeight(Out, TEXT("22_MBP"), Weight);
            return;
        }
        if (PoseID == TEXT("20_FV"))
        {
            V49AddWeight(Out, TEXT("20_FV"), Weight);
            return;
        }
        if (PoseID == TEXT("24_Tongue_Th"))
        {
            V49AddWeight(Out, TEXT("24_Tongue_Th"), Weight * 0.95f);
            V49AddWeight(Out, TEXT("20_FV"), Weight * 0.36f); // visible dental/teeth accent if tongue pose is subtle
            return;
        }
        if (PoseID == TEXT("14_ChJjSh"))
        {
            V49AddWeight(Out, TEXT("14_ChJjSh"), Weight * 0.90f);
            V49AddWeight(Out, TEXT("20_FV"), Weight * 0.42f); // visible fricative fallback
            return;
        }
        if (PoseID == TEXT("16_Ww-Ew-"))
        {
            // v50: EW is not sustained purse. It is a tiny anticipatory W/funnel
            // followed by the perceptual EEE/spread shape. This fixes "new/Newport/you/future"
            // reading as duck lips for the whole syllable.
            V49AddWeight(Out, TEXT("12_Ww-Oo-"), Weight * 0.20f);
            V49AddWeight(Out, TEXT("03_Ee"), Weight * 0.84f);
            return;
        }
        if (PoseID == TEXT("12_Ww-Oo-"))
        {
            // v50: W consonants are brief runway accents, not the vowel carrier.
            // Sustained OO lives on 11_Oo; W/Oo funnel is deliberately fragile.
            if (V50IsTrueSustainedOoWord(Event))
            {
                V49AddWeight(Out, TEXT("11_Oo"), Weight * 0.54f);
                V49AddWeight(Out, TEXT("12_Ww-Oo-"), Weight * 0.18f);
            }
            else if (V50IsEwTransitionWord(Event))
            {
                V49AddWeight(Out, TEXT("12_Ww-Oo-"), Weight * 0.18f);
                V49AddWeight(Out, TEXT("03_Ee"), Weight * 0.72f);
            }
            else if (V50IsWConsonantWord(Event))
            {
                V49AddWeight(Out, TEXT("12_Ww-Oo-"), Weight * 0.30f);
            }
            else
            {
                V49AddWeight(Out, TEXT("12_Ww-Oo-"), Weight * 0.22f);
            }
            return;
        }
        if (PoseID == TEXT("09_Oh") || PoseID == TEXT("10_Or") || PoseID == TEXT("11_Oo") || PoseID == TEXT("17_Rr"))
        {
            // Rounded vowels should read as round/open vowel, not persistent funnel.
            V49AddWeight(Out, TEXT("11_Oo"), Weight * (V50IsTrueSustainedOoWord(Event) ? 0.58f : 0.46f));
            return;
        }
        if (PoseID == TEXT("03_Ee") || PoseID == TEXT("04_Ih") || PoseID == TEXT("05_Ay") || PoseID == TEXT("06_Eh") || PoseID == TEXT("15_Ew") || PoseID == TEXT("02_TDS_KGY-Ee-"))
        {
            V49AddWeight(Out, TEXT("03_Ee"), Weight);
            return;
        }
        if (PoseID == TEXT("07_Aa") || PoseID == TEXT("08_Ah") || PoseID == TEXT("18_Uh") || PoseID == TEXT("01_TDS-Ah-"))
        {
            V49AddWeight(Out, TEXT("08_Ah"), Weight * 0.80f);
            return;
        }

        // Fallback by planner family. This guarantees no text event is dead-on-arrival
        // just because the authored pose id is absent from the runtime submission set.
        switch (Event.Viseme)
        {
        case EOffgridAITextViseme::MBP: V49AddWeight(Out, TEXT("22_MBP"), Weight); break;
        case EOffgridAITextViseme::AAA: V49AddWeight(Out, TEXT("08_Ah"), Weight * 0.78f); break;
        case EOffgridAITextViseme::EEE: V49AddWeight(Out, TEXT("03_Ee"), Weight); break;
        case EOffgridAITextViseme::OOO: V49AddWeight(Out, TEXT("11_Oo"), Weight * 0.46f); break;
        case EOffgridAITextViseme::WUH:
            V49AddWeight(Out, TEXT("12_Ww-Oo-"), Weight * (V50IsWConsonantWord(Event) ? 0.30f : 0.20f));
            if (V50IsEwTransitionWord(Event))
            {
                V49AddWeight(Out, TEXT("03_Ee"), Weight * 0.62f);
            }
            break;
        case EOffgridAITextViseme::FVS: V49AddWeight(Out, TEXT("20_FV"), Weight * 0.82f); break;
        default: break;
        }
    }

    static void V49ApplyPerceptualHierarchy(TMap<FName, float>& Weights)
    {
        // v20: no-op by design.
        // This function used to be a defensive anti-mush arbitration layer that
        // capped OOO/WUH and suppressed open/round/funnel whenever MBP/FV/EEE
        // were present. That directly contradicted the current architecture:
        // the text viseme plan is trusted, and layer 3 should play aligned plan
        // events with confidence. Event-local envelopes and max pose reduction
        // are the only arbitration in trusted-plan mode.
        (void)Weights;
    }

    static float V09FindMaxWeight(const TMap<FName, float>& Weights, FName A, FName B = NAME_None, FName C = NAME_None, FName D = NAME_None, FName E = NAME_None, FName FArg = NAME_None)
    {
        float Out = Weights.FindRef(A);
        if (B != NAME_None) { Out = FMath::Max(Out, Weights.FindRef(B)); }
        if (C != NAME_None) { Out = FMath::Max(Out, Weights.FindRef(C)); }
        if (D != NAME_None) { Out = FMath::Max(Out, Weights.FindRef(D)); }
        if (E != NAME_None) { Out = FMath::Max(Out, Weights.FindRef(E)); }
        if (FArg != NAME_None) { Out = FMath::Max(Out, Weights.FindRef(FArg)); }
        return Clamp01(Out);
    }

    static void V09ApplyPeakLocalSaliencePreservation(const TMap<FName, float>& TextDriverWeights, FOffgridAILipsyncPoseRuntimeState& PoseState, TMap<FName, float>* DriverWeights)
    {
        // Layer 3 only: preserve peak-local salience from the immutable aligned
        // text/audio event sample. This does not move event centers, alter event
        // selection, change planner strengths, or touch MetaHuman rig mapping.
        // It only prevents high-salience active events from being over-arbitrated
        // into a muted/mumbled performed frame.
        const float ClosedIntent = TextDriverWeights.FindRef(TEXT("22_MBP"));
        const float TeethIntent = V09FindMaxWeight(TextDriverWeights, TEXT("20_FV"), TEXT("24_Tongue_Th"), TEXT("14_ChJjSh"));
        const float OpenIntent = V09FindMaxWeight(TextDriverWeights, TEXT("08_Ah"), TEXT("07_Aa"), TEXT("18_Uh"), TEXT("01_TDS-Ah-"));
        const float WideIntent = V09FindMaxWeight(TextDriverWeights, TEXT("03_Ee"), TEXT("04_Ih"), TEXT("05_Ay"), TEXT("06_Eh"), TEXT("15_Ew"), TEXT("02_TDS_KGY-Ee-"));
        const float RoundIntent = V09FindMaxWeight(TextDriverWeights, TEXT("11_Oo"), TEXT("10_Or"), TEXT("09_Oh"), TEXT("17_Rr"));
        const float FunnelIntent = V09FindMaxWeight(TextDriverWeights, TEXT("12_Ww-Oo-"), TEXT("16_Ww-Ew-"));

        const float ClosedSovereignty = SmoothStep01(0.22f, 0.76f, ClosedIntent);
        const float TeethSovereignty = SmoothStep01(0.36f, 0.86f, TeethIntent);

        // v09b: keep layer-2 timing immutable, but make layer-3 arbitration less
        // hostile to rounded/funnel events. In v09, WUH/OO could still disappear
        // whenever a nearby closure/teeth event was strong because all vowels used
        // the same heavily reduced VowelRoom. Rounded onsets need a little more
        // room than open/wide vowels, but this must remain a frame-local ownership
        // decision, not a time shift.
        const float VowelRoom = Clamp01(1.0f - (ClosedSovereignty * 0.88f) - (TeethSovereignty * 0.32f));
        const float RoundedRoom = Clamp01(1.0f - (ClosedSovereignty * 0.46f) - (TeethSovereignty * 0.22f));

        auto PreserveWithRoom = [](float Intent, float& Current, float LowTarget, float HighTarget, float Room)
        {
            const float PeakLocal = SmoothStep01(0.30f, 0.92f, Intent);
            if (PeakLocal <= 0.0f)
            {
                return;
            }

            const float Target = FMath::Lerp(LowTarget, HighTarget, PeakLocal) * Room;
            Current = FMath::Max(Current, Target);
        };

        // Conservative natural-performance ranges. These are abstract viseme
        // weights, not rig-control percentages. MBP/FV are not globally boosted.
        PreserveWithRoom(OpenIntent, PoseState.Open, 0.28f, 0.62f, VowelRoom);
        PreserveWithRoom(WideIntent, PoseState.Wide, 0.32f, 0.66f, VowelRoom);
        PreserveWithRoom(RoundIntent, PoseState.Round, 0.28f, 0.62f, RoundedRoom);
        PreserveWithRoom(FunnelIntent, PoseState.Funnel, 0.22f, 0.52f, RoundedRoom);

        // When a high-salience rounded/funnel event is active, let it own a small
        // peak-local window by reducing generic open/wide carrier bleed. This
        // happens only from already-active aligned event samples, so it cannot move
        // layer-2 event times or create new viseme choices.
        const float RoundedIntent = FMath::Max(RoundIntent, FunnelIntent);
        const float RoundClaim = RoundedIntent * RoundedRoom;
        if (RoundClaim > 0.20f)
        {
            const float S = SmoothStep01(0.20f, 0.85f, RoundClaim);
            PoseState.Open *= (1.0f - 0.38f * S);
            PoseState.Wide *= (1.0f - 0.30f * S);

            // Very small peak-local ducking only when the rounded/funnel intent is
            // itself strong. This prevents W/OO from being erased by carrier
            // closures, without changing closure timing or weakening MBP globally.
            const float StrongRounded = SmoothStep01(0.58f, 0.95f, RoundedIntent);
            if (StrongRounded > 0.0f)
            {
                PoseState.Closed *= (1.0f - 0.08f * S * StrongRounded);
                PoseState.Teeth *= (1.0f - 0.06f * S * StrongRounded);
            }
        }

        PoseState.Closed = Clamp01(PoseState.Closed);
        PoseState.Open = Clamp01(PoseState.Open);
        PoseState.Wide = Clamp01(PoseState.Wide);
        PoseState.Round = Clamp01(PoseState.Round);
        PoseState.Funnel = Clamp01(PoseState.Funnel);
        PoseState.Teeth = Clamp01(PoseState.Teeth);

        if (DriverWeights)
        {
            V49AddWeight(*DriverWeights, TEXT("08_Ah"), PoseState.Open);
            V49AddWeight(*DriverWeights, TEXT("03_Ee"), PoseState.Wide);
            V49AddWeight(*DriverWeights, TEXT("11_Oo"), PoseState.Round);
            V49AddWeight(*DriverWeights, TEXT("12_Ww-Oo-"), PoseState.Funnel);
        }
    }

    static void SetPoseMapFromState(TMap<FName, float>& OutMap, const FOffgridAILipsyncPoseRuntimeState& State)
    {
        OutMap.Reset();
        OutMap.Add(TEXT("MBP"), Clamp01(State.Closed));
        OutMap.Add(TEXT("AAA"), Clamp01(State.Open));
        OutMap.Add(TEXT("EEE"), Clamp01(State.Wide));
        OutMap.Add(TEXT("OOO"), Clamp01(State.Round));
        OutMap.Add(TEXT("WUH"), Clamp01(State.Funnel));
        OutMap.Add(TEXT("FVS"), Clamp01(State.Teeth));
    }



    static FOffgridAILipsyncPoseRuntimeState BuildPoseStateFromDirectVisemeWeights(const TMap<FName, float>& PoseWeights)
    {
        FOffgridAILipsyncPoseRuntimeState State;
        for (const TPair<FName, float>& Pair : PoseWeights)
        {
            const FName PoseID = Pair.Key;
            const float W = Clamp01(Pair.Value);
            if (W <= 0.0f)
            {
                continue;
            }

            if (PoseID == TEXT("22_MBP"))
            {
                State.Closed = FMath::Max(State.Closed, W);
            }
            else if (PoseID == TEXT("09_Oh"))
            {
                // v44: Oh is primarily a rounded vowel. v42/v43 logs showed
                // 09_Oh falling through the Open/Ah path, causing excessive
                // 08_Ah dominance and almost no round-bias contribution.
                State.Round = FMath::Max(State.Round, W);
                State.Open = FMath::Max(State.Open, W * 0.22f);
            }
            else if (PoseID == TEXT("10_Or") || PoseID == TEXT("11_Oo") || PoseID == TEXT("17_Rr"))
            {
                State.Round = FMath::Max(State.Round, W);
                State.Open = FMath::Max(State.Open, W * 0.10f);
            }
            else if (PoseID == TEXT("12_Ww-Oo-"))
            {
                // v50: runtime W/funnel is a brief lip-prep accent. Do not let it
                // become the line's persistent carrier shape.
                State.Funnel = FMath::Max(State.Funnel, W);
                State.Round = FMath::Max(State.Round, W * 0.28f);
                State.Open = FMath::Max(State.Open, W * 0.02f);
            }
            else if (PoseID == TEXT("16_Ww-Ew-"))
            {
                State.Funnel = FMath::Max(State.Funnel, W * 0.72f);
                State.Round = FMath::Max(State.Round, W * 0.46f);
                State.Wide = FMath::Max(State.Wide, W * 0.42f);
            }
            else if (PoseID == TEXT("07_Aa") || PoseID == TEXT("08_Ah") || PoseID == TEXT("18_Uh") || PoseID == TEXT("01_TDS-Ah-"))
            {
                State.Open = FMath::Max(State.Open, W);
            }
            else if (PoseID == TEXT("03_Ee") || PoseID == TEXT("04_Ih") || PoseID == TEXT("05_Ay") || PoseID == TEXT("06_Eh") || PoseID == TEXT("15_Ew") || PoseID == TEXT("02_TDS_KGY-Ee-"))
            {
                State.Wide = FMath::Max(State.Wide, W);
                State.Open = FMath::Max(State.Open, W * 0.10f);
            }
            else if (PoseID == TEXT("20_FV"))
            {
                State.Teeth = FMath::Max(State.Teeth, W);
            }
            else if (PoseID == TEXT("19_FV-Or-"))
            {
                State.Teeth = FMath::Max(State.Teeth, W);
                State.Round = FMath::Max(State.Round, W * 0.55f);
            }
            else if (PoseID == TEXT("21_FV-Ee-"))
            {
                State.Teeth = FMath::Max(State.Teeth, W);
                State.Wide = FMath::Max(State.Wide, W * 0.55f);
            }
            else if (PoseID == TEXT("24_Tongue_Th"))
            {
                State.Teeth = FMath::Max(State.Teeth, W * 0.58f);
            }
            else if (PoseID == TEXT("14_ChJjSh") || PoseID == TEXT("13_KGY_TDS"))
            {
                State.Teeth = FMath::Max(State.Teeth, W * 0.55f);
            }
        }

        return State;
    }

    static FOffgridAILipsyncPoseRuntimeState LerpPoseState(const FOffgridAILipsyncPoseRuntimeState& A, const FOffgridAILipsyncPoseRuntimeState& B, float Alpha)
    {
        FOffgridAILipsyncPoseRuntimeState Out;
        Out.Closed = Clamp01(FMath::Lerp(A.Closed, B.Closed, Alpha));
        Out.Open = Clamp01(FMath::Lerp(A.Open, B.Open, Alpha));
        Out.Wide = Clamp01(FMath::Lerp(A.Wide, B.Wide, Alpha));
        Out.Round = Clamp01(FMath::Lerp(A.Round, B.Round, Alpha));
        Out.Funnel = Clamp01(FMath::Lerp(A.Funnel, B.Funnel, Alpha));
        Out.Teeth = Clamp01(FMath::Lerp(A.Teeth, B.Teeth, Alpha));
        return Out;
    }

    static float GetPoseStateValue(const FOffgridAILipsyncPoseRuntimeState& State, FName PoseName)
    {
        if (PoseName == TEXT("MBP")) { return State.Closed; }
        if (PoseName == TEXT("AAA")) { return State.Open; }
        if (PoseName == TEXT("EEE")) { return State.Wide; }
        if (PoseName == TEXT("OOO")) { return State.Round; }
        if (PoseName == TEXT("WUH")) { return State.Funnel; }
        if (PoseName == TEXT("FVS")) { return State.Teeth; }
        return 0.0f;
    }

    static void SetPoseStateValue(FOffgridAILipsyncPoseRuntimeState& State, FName PoseName, float Value)
    {
        if (PoseName == TEXT("MBP")) { State.Closed = Clamp01(Value); return; }
        if (PoseName == TEXT("AAA")) { State.Open = Clamp01(Value); return; }
        if (PoseName == TEXT("EEE")) { State.Wide = Clamp01(Value); return; }
        if (PoseName == TEXT("OOO")) { State.Round = Clamp01(Value); return; }
        if (PoseName == TEXT("WUH")) { State.Funnel = Clamp01(Value); return; }
        if (PoseName == TEXT("FVS")) { State.Teeth = Clamp01(Value); return; }
    }

    static FName GetDominantPoseName(const FOffgridAILipsyncPoseRuntimeState& State, float& OutWeight)
    {
        FName BestName = TEXT("MBP");
        float BestWeight = State.Closed;

        if (State.Open > BestWeight) { BestName = TEXT("AAA"); BestWeight = State.Open; }
        if (State.Wide > BestWeight) { BestName = TEXT("EEE"); BestWeight = State.Wide; }
        if (State.Round > BestWeight) { BestName = TEXT("OOO"); BestWeight = State.Round; }
        if (State.Funnel > BestWeight) { BestName = TEXT("WUH"); BestWeight = State.Funnel; }
        if (State.Teeth > BestWeight) { BestName = TEXT("FVS"); BestWeight = State.Teeth; }

        OutWeight = BestWeight;
        return BestName;
    }

    static void ApplyPoseConviction(FOffgridAILipsyncPoseRuntimeState& State, float Power, float FloorThreshold)
    {
        State.Closed = State.Closed > FloorThreshold ? Clamp01(FMath::Pow(State.Closed, Power)) : 0.0f;
        State.Open   = State.Open   > FloorThreshold ? Clamp01(FMath::Pow(State.Open,   Power)) : 0.0f;
        State.Wide   = State.Wide   > FloorThreshold ? Clamp01(FMath::Pow(State.Wide,   Power)) : 0.0f;
        State.Round  = State.Round  > FloorThreshold ? Clamp01(FMath::Pow(State.Round,  Power)) : 0.0f;
        State.Funnel = State.Funnel > FloorThreshold ? Clamp01(FMath::Pow(State.Funnel, Power)) : 0.0f;
        State.Teeth  = State.Teeth  > FloorThreshold ? Clamp01(FMath::Pow(State.Teeth,  Power)) : 0.0f;
    }

    static void ClampToDominantPose(FOffgridAILipsyncPoseRuntimeState& State, float SecondaryScale)
    {
        float DominantWeight = 0.0f;
        const FName Dominant = GetDominantPoseName(State, DominantWeight);
        if (DominantWeight <= 0.0f)
        {
            return;
        }

        if (Dominant != TEXT("MBP")) { State.Closed *= SecondaryScale; }
        if (Dominant != TEXT("AAA")) { State.Open *= SecondaryScale; }
        if (Dominant != TEXT("EEE")) { State.Wide *= SecondaryScale; }
        if (Dominant != TEXT("OOO")) { State.Round *= SecondaryScale; }
        if (Dominant != TEXT("WUH")) { State.Funnel *= SecondaryScale; }
        if (Dominant != TEXT("FVS")) { State.Teeth *= SecondaryScale; }
    }

static FString NormalizeEmotionRawString(const FString& InRaw)
{
    FString Raw = InRaw.TrimStartAndEnd().ToLower();
    Raw.ReplaceInline(TEXT("\""), TEXT(""));
    Raw.ReplaceInline(TEXT("'"), TEXT(""));
    Raw.ReplaceInline(TEXT(";"), TEXT(","));

    // Accept indexed classifier rows ("1=joy") and return only the emotion token.
    int32 EqualsIndex = INDEX_NONE;
    if (Raw.FindChar(TEXT('='), EqualsIndex))
    {
        Raw = Raw.Mid(EqualsIndex + 1).TrimStartAndEnd();
    }

    int32 CommaIndex = INDEX_NONE;
    if (Raw.FindChar(TEXT(','), CommaIndex))
    {
        Raw = Raw.Left(CommaIndex).TrimStartAndEnd();
    }

    int32 DashIndex = INDEX_NONE;
    if (Raw.FindChar(TEXT('-'), DashIndex))
    {
        const FString Left = Raw.Left(DashIndex).TrimStartAndEnd();
        const FString Right = Raw.Mid(DashIndex + 1).TrimStartAndEnd();
        if (!Left.IsEmpty())
        {
            Raw = Left;
        }
    }

    Raw.ReplaceInline(TEXT("."), TEXT(""));
    Raw.ReplaceInline(TEXT(":"), TEXT(""));
    return Raw.TrimStartAndEnd();
}

static FName NormalizeEmotionName(FName Emotion)
{
    const FString Raw = NormalizeEmotionRawString(Emotion.ToString());

    if (Raw.IsEmpty() || Raw == TEXT("none") || Raw == TEXT("neutral") || Raw == TEXT("noop") || Raw == TEXT("no-op"))
    {
        return TEXT("neutral");
    }

    // Canonical runtime labels match the MetaHuman emotion families.
    // This mapping is intentionally forgiving because small local models may
    // vary casing, synonyms, or use the old DAOffgridAIEmotionSettings labels.
    if (Raw == TEXT("joy") || Raw == TEXT("happy") || Raw == TEXT("happiness") || Raw == TEXT("joyful") || Raw == TEXT("amused") || Raw == TEXT("amusement") || Raw == TEXT("upbeat") || Raw == TEXT("friendly"))
    {
        return TEXT("joy");
    }

    if (Raw == TEXT("sadness") || Raw == TEXT("sad") || Raw == TEXT("sorrow") || Raw == TEXT("unhappy") || Raw == TEXT("sympathy") || Raw == TEXT("regret"))
    {
        return TEXT("sadness");
    }

    if (Raw == TEXT("fear") || Raw == TEXT("fearful") || Raw == TEXT("afraid") || Raw == TEXT("scared") || Raw == TEXT("anxious") || Raw == TEXT("intimidated"))
    {
        return TEXT("fear");
    }

    if (Raw == TEXT("anger") || Raw == TEXT("angry") || Raw == TEXT("mad") || Raw == TEXT("rage") || Raw == TEXT("furious") || Raw == TEXT("irritated") || Raw == TEXT("annoyed"))
    {
        return TEXT("anger");
    }

    if (Raw == TEXT("surprise") || Raw == TEXT("surprised") || Raw == TEXT("shock") || Raw == TEXT("shocked") || Raw == TEXT("unexpected"))
    {
        return TEXT("surprise");
    }

    if (Raw == TEXT("disgust") || Raw == TEXT("disgusted") || Raw == TEXT("revulsion") || Raw == TEXT("revulsed") || Raw == TEXT("gross") || Raw == TEXT("filth"))
    {
        return TEXT("disgust");
    }

    return FName(*Raw);
}


} // namespace

UOffgridAILineCoach::UOffgridAILineCoach()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UOffgridAILineCoach::BeginPlay()
{
    Super::BeginPlay();

    InitializeEmotionMapsIfNeeded();
    CachedFaceDriver = GetOwner() ? GetOwner()->FindComponentByClass<UOffgridAIMetaHumanFaceDriverComponent>() : nullptr;
    ApplyEmotionMouthAllowanceSettingsToFaceDriver(CachedFaceDriver);
    ForceNeutral();

    if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
    {
        Orchestrator->RegisterLineCoach(this);
    }
}

void UOffgridAILineCoach::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(PlaybackCompletionTimerHandle);
    }

    GInitialDominantEmotionPresentedLineCoaches.Remove(this);

    ForceNeutral();
    TeardownPlaybackObjects();

    if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
    {
        Orchestrator->UnregisterLineCoach(NPCID, this);
    }

    delete LipsyncRuntimeSession;
    LipsyncRuntimeSession = nullptr;

    Super::EndPlay(EndPlayReason);
}

void UOffgridAILineCoach::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    FlushPendingOutputPCM(false);
    StartOutputPlaybackIfReady(false);

    // V04 streaming contract: incoming TTS chunks advance observed audio, but
    // playback time advances every tick. The streaming aligner commit horizon is
    // PlaybackSec + designer preroll, converted back into AudioBufferSec.
    // Therefore the committed track must be refreshed on the playback tick, not
    // only when new audio chunks arrive. Without this, fast TTS synthesis can
    // finish before audible playback reaches later visemes, leaving the track
    // permanently starved after the first small commit window.
    if (bOutputPlaybackStarted && IsStreamingLipsyncRuntimeEnabled())
    {
        UpdateStreamingAlignedVisemeTrack();
    }

    UpdateLipsyncFromPlayback(DeltaTime);
    UpdateDisplayedLipsyncPose(DeltaTime);
    BroadcastFacialFrameIfChanged();
}


FString UOffgridAILineCoach::BuildVoiceDesignInstructionForLine(const FOffgridAILinePerformanceRequest& LineRequest) const
{
    auto NormalizePromptFragment = [](const FString& InText, const TCHAR* Fallback)
    {
        FString Out = InText.TrimStartAndEnd();
        if (Out.IsEmpty())
        {
            Out = Fallback;
        }
        while (Out.EndsWith(TEXT(".")) || Out.EndsWith(TEXT(",")) || Out.EndsWith(TEXT(";")) || Out.EndsWith(TEXT(":")))
        {
            Out.LeftChopInline(1, EAllowShrinking::No);
            Out.TrimEndInline();
        }
        return Out;
    };

    const FString Identity = NormalizePromptFragment(VoiceDesignIdentity, TEXT("natural adult conversational voice"));
    const FString Delivery = NormalizePromptFragment(VoiceDesignNeutralDelivery, TEXT("natural, conversational, emotionally balanced"));

    // Phase 1 VoiceDesign adoption intentionally ignores PAD/emotion grading.
    // The active line is accepted only so this helper can later incorporate
    // per-line delivery without changing the call site.
    (void)LineRequest;

    return FString::Printf(TEXT("%s. %s."), *Identity, *Delivery);
}

void UOffgridAILineCoach::PerformLine(const FOffgridAILinePerformanceRequest& LineRequest)
{
    ActiveLineRequest = LineRequest;
    bHasActiveLineRequest = true;

    // Shared-lipsync contract: LineCoach must not build or tune its own text
    // timeline.  The AU/LipLab core session is the single owner of planning,
    // budgeting, streaming detection, island alignment, and final evidence
    // retiming.  The authoritative plan is created in BeginSharedLipsyncRuntimeSession()
    // when the matching output audio stream opens.
    ActiveTextVisemePlan = FOffgridAITextVisemePlan();
    ActiveAlignedVisemeTrack = FOffgridAIAlignedVisemeTrack();
    bActiveAlignedVisemeTrackBuilt = false;
    LipsyncEstimatedTextDurationSeconds = 0.0f;

    UE_LOG(LogOffgridAI, Verbose, TEXT("LineCoach accepted line for shared lipsync runtime npc=%s line=%s dialogue=\"%s\""),
        *NPCID.ToString(),
        *LineRequest.LineID.ToString(),
        *LineRequest.Dialogue.ToString());

    // Do not start facial emotion here. With streamed TTS, this method runs when
    // the text line is dispatched, which may be noticeably before audio playback
    // actually begins. The emotion target is applied in StartOutputPlaybackIfReady()
    // at the same moment the AudioComponent starts playing.
}

void UOffgridAILineCoach::BeginOutputAudioStream(int32 SampleRate, int32 NumChannels)
{
    BeginOutputAudioStream(ActiveLineRequest.LineID, SampleRate, NumChannels);
}

void UOffgridAILineCoach::BeginOutputAudioStream(FName LineID, int32 SampleRate, int32 NumChannels)
{
    if (!bHasActiveLineRequest || ActiveLineRequest.LineID != LineID)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("LineCoach ignored begin stream for inactive/stale line npc=%s expected=%s actual=%s"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            *LineID.ToString());
        return;
    }

    if (bHasActiveLineRequest &&
        ActiveLineRequest.LineID == LineID &&
        bOutputPlaybackStarted &&
        HasPendingOrBufferedOutputAudio())
    {
        // Some None/loopback and service polling paths can surface a duplicate
        // stream_started event for the same line after EndOutputAudioStream()
        // has already marked the input closed but before audible playback has
        // drained. Treat this as idempotent. Restarting here resets playback and
        // lipsync state mid-line.
        UE_LOG(LogOffgridAI, Verbose, TEXT("LineCoach duplicate begin output stream ignored during active drain npc=%s line=%s sample_rate=%d channels=%d queued_bytes=%lld submitted_bytes=%lld buffered_bytes=%lld"),
            *NPCID.ToString(),
            *LineID.ToString(),
            SampleRate,
            NumChannels,
            QueuedOutputBytes,
            SubmittedOutputBytes,
            GetEstimatedBufferedPlaybackBytes());
        return;
    }

    if (SampleRate <= 0 || NumChannels <= 0)
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(PlaybackCompletionTimerHandle);
    }

    UE_LOG(LogOffgridAI, Log, TEXT("LineCoach begin output stream npc=%s line=%s sample_rate=%d channels=%d"),
        *NPCID.ToString(),
        *ActiveLineRequest.LineID.ToString(),
        SampleRate,
        NumChannels);

    if (bOutputStreamOpen && bHasActiveLineRequest && ActiveSampleRate == SampleRate && ActiveNumChannels == NumChannels)
    {
        // Some service/poll paths can surface a duplicate stream_started event for
        // the same line. Treat it as idempotent. Resetting the procedural wave here
        // destroys the beginning of the first hot-start line.
        UE_LOG(LogOffgridAI, Verbose, TEXT("LineCoach duplicate begin output stream ignored npc=%s line=%s sample_rate=%d channels=%d queued_bytes=%lld submitted_bytes=%lld buffered_bytes=%lld"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            SampleRate,
            NumChannels,
            QueuedOutputBytes,
            SubmittedOutputBytes,
            GetEstimatedBufferedPlaybackBytes());
        return;
    }

    EnsurePlaybackObjects(SampleRate, NumChannels);

    if (OutputAudioComponent && OutputAudioComponent->IsPlaying())
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("LineCoach stopping active audio for new stream npc=%s old_line=%s new_line=%s queued_bytes=%lld submitted_bytes=%lld buffered_bytes=%lld"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            QueuedOutputBytes,
            SubmittedOutputBytes,
            GetEstimatedBufferedPlaybackBytes());
        OutputAudioComponent->Stop();
    }

    if (ProceduralSoundWave)
    {
        ProceduralSoundWave->ResetAudio();
    }

    QueuedOutputBytes = 0;
    SubmittedOutputBytes = 0;
    ReceivedOutputChunkCount = 0;
    SubmittedOutputChunkCount = 0;
    PlaybackStartedAfterWindowIndex = 0;
    LastSubmittedChunkStartSample = 0;
    LastSubmittedChunkEndSample = 0;
    OutputPlaybackStartTimeSeconds = 0.0;
    OutputDrainZeroSinceSeconds = 0.0;
    PendingOutputPCM.Reset();
    PendingOutputPCMReadOffset = 0;

    ResetLipsyncRuntimeState();
    bDriverVisemeSubmissionEnabled = true;
    ResetLipsyncDebugLog();
    ResetLipsyncDebugInputAudio();
    BeginSharedLipsyncRuntimeSession();

    bOutputStreamOpen = true;
    bOutputPlaybackStarted = false;
    bOutputPlaybackPausedForUnderrun = false;
    OutputPlaybackResumeTimeSeconds = 0.0;
    ConsumedPlaybackTimeSeconds = 0.0;
    OutputDrainZeroSinceSeconds = 0.0;

    if (OutputAudioComponent && ProceduralSoundWave)
    {
        OutputAudioComponent->SetSound(ProceduralSoundWave);
        OutputAudioComponent->SetVolumeMultiplier(GetPlaybackVolumeMultiplier());
        OutputAudioComponent->Stop();
    }
}

void UOffgridAILineCoach::SubmitOutputAudioChunk(const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels)
{
    SubmitOutputAudioChunk(ActiveLineRequest.LineID, PCMChunk, SampleRate, NumChannels);
}

void UOffgridAILineCoach::SubmitOutputAudioChunk(FName LineID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels)
{
    const int32 BytesPerFrame = FMath::Max(NumChannels * static_cast<int32>(sizeof(int16)), static_cast<int32>(sizeof(int16)));
    const int32 InferredSampleCount = BytesPerFrame > 0 ? PCMChunk.Num() / BytesPerFrame : 0;
    SubmitOutputAudioChunk(LineID, PCMChunk, SampleRate, NumChannels, LastSubmittedChunkEndSample, InferredSampleCount);
}

void UOffgridAILineCoach::SubmitOutputAudioChunk(FName LineID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample, int32 ChunkSampleCount)
{
    if (!bHasActiveLineRequest || ActiveLineRequest.LineID != LineID)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("LineCoach ignored chunk for inactive/stale line npc=%s expected=%s actual=%s bytes=%d"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            *LineID.ToString(),
            PCMChunk.Num());
        return;
    }

    if (!ProceduralSoundWave || PCMChunk.Num() == 0 || ActiveSampleRate <= 0 || ActiveNumChannels <= 0)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("LineCoach dropped TTS chunk npc=%s line=%s bytes=%d procedural=%s active_sr=%d active_ch=%d"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            PCMChunk.Num(),
            ProceduralSoundWave ? TEXT("true") : TEXT("false"),
            ActiveSampleRate,
            ActiveNumChannels);
        return;
    }

    if (SampleRate > 0 && SampleRate != ActiveSampleRate)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("LineCoach received TTS chunk with sample-rate mismatch for NPC %s line %s: stream=%d chunk=%d. Playback will use stream sample rate."),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            ActiveSampleRate,
            SampleRate);
    }

    if (NumChannels > 0 && NumChannels != ActiveNumChannels)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("LineCoach received TTS chunk with channel mismatch for NPC %s line %s: stream=%d chunk=%d. Playback will use stream channel count."),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            ActiveNumChannels,
            NumChannels);
    }

    // Streaming playback contract: qwen emits bursty decoded windows. LineCoach owns
    // the Unreal-side equivalent of qwen's StreamingAudioPlayer: keep a byte reservoir,
    // queue all available complete PCM frames into USoundWaveProcedural once the live
    // preroll watermark is reached, and let lipsync run strictly sidecar.
    const int32 BytesPerFrame = FMath::Max(ActiveNumChannels * static_cast<int32>(sizeof(int16)), static_cast<int32>(sizeof(int16)));
    int32 BytesToQueue = (PCMChunk.Num() / BytesPerFrame) * BytesPerFrame;
    if (BytesToQueue <= 0)
    {
        return;
    }

    PendingOutputPCM.Append(PCMChunk.GetData(), BytesToQueue);
    QueuedOutputBytes += BytesToQueue;
    ++ReceivedOutputChunkCount;
    OutputDrainZeroSinceSeconds = 0.0;

    const int32 TraceBytesPerSecond = FMath::Max(GetBytesPerSecond(), 1);
    const double TraceChunkMs = static_cast<double>(BytesToQueue) / static_cast<double>(TraceBytesPerSecond) * 1000.0;
    const double TraceQueuedMs = static_cast<double>(QueuedOutputBytes) / static_cast<double>(TraceBytesPerSecond) * 1000.0;
    UE_LOG(LogOffgridAI, Log, TEXT("[TTS_WINDOW] line=%s index=%d bytes=%d audio_ms=%.1f queued_audio_ms=%.1f chunk_start_sample=%lld chunk_samples=%d"),
        *LineID.ToString(),
        ReceivedOutputChunkCount,
        BytesToQueue,
        TraceChunkMs,
        TraceQueuedMs,
        ChunkStartSample,
        ChunkSampleCount);

    if (bHasActiveLineRequest && QueuedOutputBytes == BytesToQueue)
    {
        if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
        {
            Orchestrator->NotifyLineFirstAudioSample(ActiveLineRequest.ConversationID, ActiveLineRequest.LineID, BytesToQueue);
        }
    }

    // Lipsync is strictly sidecar/non-blocking. In streaming mode, analysis updates
    // speech islands and commits immutable viseme events as soon as the relevant audio
    // is present in the linecoach-owned buffer. Offline mode still waits for the final
    // full-buffer AudioAnchorAligner track at stream close.
    const int32 QueuedSampleCount = BytesToQueue / BytesPerFrame;
    const int32 EffectiveChunkSampleCount = ChunkSampleCount > 0 ? ChunkSampleCount : QueuedSampleCount;
    LastSubmittedChunkStartSample = ChunkStartSample;
    LastSubmittedChunkEndSample = ChunkStartSample + EffectiveChunkSampleCount;
    AppendPCMToLipsyncAnalysisBuffer(PCMChunk, BytesToQueue, ActiveSampleRate, ActiveNumChannels, ChunkStartSample, EffectiveChunkSampleCount);

    if (IsStreamingLipsyncRuntimeEnabled())
    {
        GetOrCreateSharedLipsyncRuntimeSession().PushAudioPCM16(PCMChunk, BytesToQueue, ActiveSampleRate, ActiveNumChannels, ChunkStartSample);
        SyncSharedLipsyncRuntimeMirrors();
        UpdateStreamingAlignedVisemeTrack();
        FlushPendingOutputPCM(false);
    }

    UE_LOG(LogOffgridAI, Verbose, TEXT("LineCoach queued TTS chunk npc=%s line=%s bytes=%d total_queued=%lld procedural_buffered=%lld playing=%s"),
        *NPCID.ToString(),
        *ActiveLineRequest.LineID.ToString(),
        BytesToQueue,
        QueuedOutputBytes,
        GetEstimatedBufferedPlaybackBytes(),
        (OutputAudioComponent && OutputAudioComponent->IsPlaying()) ? TEXT("true") : TEXT("false"));

    StartOutputPlaybackIfReady(false);
}

void UOffgridAILineCoach::EndOutputAudioStream()
{
    EndOutputAudioStream(ActiveLineRequest.LineID);
}

void UOffgridAILineCoach::EndOutputAudioStream(FName LineID)
{
    if (!bHasActiveLineRequest || ActiveLineRequest.LineID != LineID)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("LineCoach ignored end stream for inactive/stale line npc=%s expected=%s actual=%s"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            *LineID.ToString());
        return;
    }

    if (!bOutputStreamOpen)
    {
        return;
    }

    // TTS completed only means no more input chunks will arrive. It is not an
    // audible playback-complete signal. Qwen now commonly finishes synthesis
    // faster than real time, so LineCoach must own the final drain and only
    // notify ConversationManager after the procedural audio queue is actually
    // empty.
    bOutputStreamOpen = false;

    if (IsStreamingLipsyncRuntimeEnabled())
    {
        FinalizeSharedLipsyncRuntimeTrack();
    }
    WriteLipsyncDebugInputAudio();

    FlushPendingOutputPCM(true);

    // BackendKind=None hot-start has no player loopback audio to replay. In that
    // case the stub emits StreamStarted -> Completed with zero chunks. Complete
    // the line immediately instead of waiting forever for playback that never
    // started.
    if (QueuedOutputBytes <= 0 && SubmittedOutputBytes <= 0 && !HasPendingOrBufferedOutputAudio())
    {
        UE_LOG(LogOffgridAI, Log, TEXT("LineCoach completed silent output stream npc=%s line=%s"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString());
        HandlePlaybackFinished();
        return;
    }

    StartOutputPlaybackIfReady(true);

    UE_LOG(LogOffgridAI, Log, TEXT("LineCoach input ended npc=%s line=%s queued_bytes=%lld submitted_bytes=%lld buffered_bytes=%lld"),
        *NPCID.ToString(),
        *ActiveLineRequest.LineID.ToString(),
        QueuedOutputBytes,
        SubmittedOutputBytes,
        GetEstimatedBufferedPlaybackBytes());

    SchedulePlaybackDrainCheck(0.03f);
}

bool UOffgridAILineCoach::IsPerformingLine(FName LineID) const
{
    return bHasActiveLineRequest && ActiveLineRequest.LineID == LineID;
}

bool UOffgridAILineCoach::IsOutputBusy() const
{
    return bHasActiveLineRequest || bOutputStreamOpen || bOutputPlaybackStarted || HasPendingOrBufferedOutputAudio();
}

float UOffgridAILineCoach::GetInitialPrerollSeconds() const
{
    const float Configured = (LineCoachAudioSettingsAsset && LineCoachAudioSettingsAsset->InitialPrerollSeconds > 0.0f)
        ? LineCoachAudioSettingsAsset->InitialPrerollSeconds
        : 0.15f;

    // VoiceDesign should match the qwen streaming CLI startup policy. A stale
    // LineCoach asset value from earlier starvation experiments (for example 1s)
    // forces playback to wait for window 2 and adds hundreds of milliseconds of
    // perceived latency. For VoiceDesign, cap live-preroll at the qwen default so
    // the first decoded window can start playback when it already exceeds 150ms.
    if (ShouldUseVoiceDesignForTTS())
    {
        return FMath::Clamp(Configured, 0.02f, 0.15f);
    }

    return FMath::Clamp(Configured, 0.02f, 2.0f);
}

float UOffgridAILineCoach::GetMaintainBufferedAudioFloorSeconds() const
{
    const float Configured = (LineCoachAudioSettingsAsset && LineCoachAudioSettingsAsset->MaintainBufferedAudioFloorSeconds > 0.0f)
        ? LineCoachAudioSettingsAsset->MaintainBufferedAudioFloorSeconds
        : 0.05f;

    return FMath::Clamp(Configured, 0.02f, 0.25f);
}

float UOffgridAILineCoach::GetCoalescedWriteSeconds() const
{
    const float Configured = (LineCoachAudioSettingsAsset && LineCoachAudioSettingsAsset->CoalescedWriteSeconds > 0.0f)
        ? LineCoachAudioSettingsAsset->CoalescedWriteSeconds
        : 0.02f;

    return FMath::Clamp(Configured, 0.005f, 0.10f);
}

float UOffgridAILineCoach::GetMaxWriteBurstSeconds() const
{
    const float Configured = (LineCoachAudioSettingsAsset && LineCoachAudioSettingsAsset->MaxWriteBurstSeconds > 0.0f)
        ? LineCoachAudioSettingsAsset->MaxWriteBurstSeconds
        : 0.12f;

    return FMath::Clamp(Configured, 0.04f, 0.50f);
}

float UOffgridAILineCoach::GetPlaybackPostDrainHoldSeconds() const
{
    const float Configured = LineCoachAudioSettingsAsset
        ? LineCoachAudioSettingsAsset->PlaybackPostDrainHoldSeconds
        : 0.30f;

    // USoundWaveProcedural can report an empty source queue before the platform
    // audio mixer has actually rendered the final frames to the listener. Keep a
    // short grace period after source drain before completing the line, otherwise
    // Stop()/ResetAudio can audibly chop the tail of the TTS output.
    return FMath::Clamp(Configured, 0.05f, 0.75f);
}

int32 UOffgridAILineCoach::GetBytesPerSecond() const
{
    return (ActiveSampleRate > 0 && ActiveNumChannels > 0)
        ? ActiveSampleRate * ActiveNumChannels * static_cast<int32>(sizeof(int16))
        : 0;
}

int64 UOffgridAILineCoach::GetEstimatedBufferedPlaybackBytes() const
{
    const int64 PendingBytes = FMath::Max(PendingOutputPCM.Num() - PendingOutputPCMReadOffset, 0);

    if (ProceduralSoundWave)
    {
        const int64 ProceduralAvailableBytes = static_cast<int64>(ProceduralSoundWave->GetAvailableAudioByteCount());
        if (!bOutputPlaybackStarted && SubmittedOutputBytes > 0)
        {
            // Some platform/mixer paths report zero available procedural bytes immediately
            // after QueueAudio even though startable PCM has just been submitted. Before
            // playback starts, LineCoach-owned accounting is the source of truth.
            return FMath::Max<int64>(ProceduralAvailableBytes, SubmittedOutputBytes) + PendingBytes;
        }
        return ProceduralAvailableBytes + PendingBytes;
    }

    const int32 BytesPerSecond = GetBytesPerSecond();
    if (BytesPerSecond <= 0)
    {
        return PendingBytes;
    }

    int64 BufferedInProcedural = SubmittedOutputBytes;
    if (bOutputPlaybackStarted && OutputPlaybackStartTimeSeconds > 0.0)
    {
        const double ElapsedSeconds = FMath::Max(FPlatformTime::Seconds() - OutputPlaybackStartTimeSeconds, 0.0);
        const int64 ConsumedBytes = static_cast<int64>(ElapsedSeconds * static_cast<double>(BytesPerSecond));
        BufferedInProcedural = FMath::Max<int64>(SubmittedOutputBytes - ConsumedBytes, 0);
    }

    return BufferedInProcedural + PendingBytes;
}

void UOffgridAILineCoach::FlushPendingOutputPCM(bool bForceFlushAll)
{
    if (!ProceduralSoundWave || ActiveSampleRate <= 0 || ActiveNumChannels <= 0)
    {
        return;
    }

    // Offline/non-streaming compatibility: do not leak queued PCM into the procedural
    // source until the final aligned track exists. Streaming mode is different: it
    // must behave like qwen's StreamingAudioPlayer and feed the audio device as soon
    // as the live-preroll watermark is available.
    if (!bForceFlushAll && !IsStreamingLipsyncRuntimeEnabled() && (bOutputStreamOpen || !bActiveAlignedVisemeTrackBuilt))
    {
        return;
    }

    const int32 PendingBytes = FMath::Max(PendingOutputPCM.Num() - PendingOutputPCMReadOffset, 0);
    if (PendingBytes <= 0)
    {
        return;
    }

    const int32 BytesPerSecond = GetBytesPerSecond();
    if (BytesPerSecond <= 0)
    {
        return;
    }

    const int32 BytesPerFrame = FMath::Max(ActiveNumChannels * static_cast<int32>(sizeof(int16)), static_cast<int32>(sizeof(int16)));
    const int32 InitialPrerollBytesRaw = FMath::Max(FMath::CeilToInt(GetInitialPrerollSeconds() * static_cast<float>(BytesPerSecond)), BytesPerFrame);
    const int32 InitialPrerollBytes = FMath::Max((InitialPrerollBytesRaw / BytesPerFrame) * BytesPerFrame, BytesPerFrame);

    if (!bOutputPlaybackStarted && !bForceFlushAll && PendingBytes < InitialPrerollBytes)
    {
        return;
    }

    // Important: do not trickle qwen windows into the procedural source using a
    // small max-write burst. Qwen's own StreamingAudioPlayer writes each available
    // decoded window to its device queue immediately. Holding a 960ms qwen window
    // in PendingOutputPCM while submitting only 120ms guarantees underruns/pops
    // before the next burst arrives.
    const int32 RemainingBytes = PendingOutputPCM.Num() - PendingOutputPCMReadOffset;
    int32 BytesToWrite = (RemainingBytes / BytesPerFrame) * BytesPerFrame;
    if (BytesToWrite <= 0)
    {
        return;
    }

    ProceduralSoundWave->QueueAudio(PendingOutputPCM.GetData() + PendingOutputPCMReadOffset, BytesToWrite);
    PendingOutputPCMReadOffset += BytesToWrite;
    SubmittedOutputBytes += BytesToWrite;
    ++SubmittedOutputChunkCount;

    const double SubmittedAudioMs = static_cast<double>(BytesToWrite) / static_cast<double>(BytesPerSecond) * 1000.0;
    const double TotalSubmittedAudioMs = static_cast<double>(SubmittedOutputBytes) / static_cast<double>(BytesPerSecond) * 1000.0;
    UE_LOG(LogOffgridAI, Log, TEXT("[TTS_TRACE] T6 audio_submitted_to_soundwave line=%s submit_index=%d bytes=%d submit_audio_ms=%.1f total_submitted_audio_ms=%.1f received_windows=%d pending_remaining_bytes=%d"),
        *ActiveLineRequest.LineID.ToString(),
        SubmittedOutputChunkCount,
        BytesToWrite,
        SubmittedAudioMs,
        TotalSubmittedAudioMs,
        ReceivedOutputChunkCount,
        PendingOutputPCM.Num() - PendingOutputPCMReadOffset);

    if (PendingOutputPCMReadOffset > 0 && PendingOutputPCMReadOffset >= PendingOutputPCM.Num())
    {
        PendingOutputPCM.Reset();
        PendingOutputPCMReadOffset = 0;
    }
    else if (PendingOutputPCMReadOffset >= 131072)
    {
        PendingOutputPCM.RemoveAt(0, PendingOutputPCMReadOffset, EAllowShrinking::No);
        PendingOutputPCMReadOffset = 0;
    }
}

void UOffgridAILineCoach::StartOutputPlaybackIfReady(bool bForceStart)
{
    if (!OutputAudioComponent || !ProceduralSoundWave || ActiveSampleRate <= 0 || ActiveNumChannels <= 0)
    {
        return;
    }

    const int32 BytesPerSecond = GetBytesPerSecond();
    if (BytesPerSecond <= 0)
    {
        return;
    }

    const int64 AvailableBytes = GetEstimatedBufferedPlaybackBytes();
    if (AvailableBytes <= 0)
    {
        return;
    }

    const int64 RequiredPrerollBytes = static_cast<int64>(FMath::CeilToInt(GetInitialPrerollSeconds() * static_cast<float>(BytesPerSecond)));
    if (!bForceStart && AvailableBytes < RequiredPrerollBytes)
    {
        return;
    }

    // Match qwen StreamingAudioPlayer semantics: audio playback starts when the
    // procedural queue has enough PCM for the live-preroll watermark. Lipsync and
    // speech detection are sidecar consumers and must never gate audible playback.
    if (IsStreamingLipsyncRuntimeEnabled())
    {
        FOffgridAILipsyncRuntimeSession& Runtime = GetOrCreateSharedLipsyncRuntimeSession();
        Runtime.Update(GetCurrentOutputPlaybackSeconds());
        SyncSharedLipsyncRuntimeMirrors();
    }

    // USoundWaveProcedural may underrun and stop the audio component even after
    // playback has already started. Treat this as resumable as long as the line
    // still has buffered or pending PCM. The first start emits the latency mark;
    // later resumes do not re-advance conversation state.
    if (!OutputAudioComponent->IsPlaying())
    {
        OutputAudioComponent->Play();
        const double QueuedAudioMs = static_cast<double>(AvailableBytes) / static_cast<double>(BytesPerSecond) * 1000.0;
        UE_LOG(LogOffgridAI, Log, TEXT("LineCoach playback %s npc=%s line=%s buffered_bytes=%lld buffered_audio_ms=%.1f force=%s"),
            bOutputPlaybackStarted ? TEXT("resumed") : TEXT("started"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            AvailableBytes,
            QueuedAudioMs,
            bForceStart ? TEXT("true") : TEXT("false"));
    }

    if (!bOutputPlaybackStarted)
    {
        bOutputPlaybackStarted = true;
        OutputPlaybackStartTimeSeconds = FPlatformTime::Seconds();
        PlaybackStartedAfterWindowIndex = ReceivedOutputChunkCount;

        const double QueuedAudioMsAtStart = static_cast<double>(AvailableBytes) / static_cast<double>(BytesPerSecond) * 1000.0;
        UE_LOG(LogOffgridAI, Log, TEXT("[TTS_TRACE] T7 playback_started line=%s queued_audio_ms=%.1f queued_bytes=%lld live_preroll_ms=%.1f started_after_window=%d submitted_windows=%d force=%s"),
            *ActiveLineRequest.LineID.ToString(),
            QueuedAudioMsAtStart,
            AvailableBytes,
            GetInitialPrerollSeconds() * 1000.0f,
            PlaybackStartedAfterWindowIndex,
            SubmittedOutputChunkCount,
            bForceStart ? TEXT("true") : TEXT("false"));

        if (IsStreamingLipsyncRuntimeEnabled())
        {
            // V03 clock contract: PlaybackSec zero maps to the first sample queued
            // into the procedural stream. We do not skip leading synthesized silence,
            // so AudioBufferSec 0.0 is the playback origin. All committed streaming
            // viseme centers are converted to playback space before FaceDriver sees them.
            StreamingPlaybackAudioBufferStartSec = 0.0f;
            bStreamingPlaybackAudioBufferMapValid = true;
            UpdateStreamingAlignedVisemeTrack();
        }

        if (bHasActiveLineRequest)
        {
            if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
            {
                Orchestrator->NotifyLineOutputPlaybackStarted(ActiveLineRequest.ConversationID, ActiveLineRequest.LineID);
            }
        }

        if (bHasActiveLineRequest)
        {
            BeginLineFacialState(ActiveLineRequest.Emotion, ActiveLineRequest.EmotionMagnitude);
        }
    }
}


bool UOffgridAILineCoach::IsStreamingLipsyncRuntimeEnabled() const
{
    return true;
}


void UOffgridAILineCoach::UpdateStreamingAlignedVisemeTrack()
{
    if (!IsStreamingLipsyncRuntimeEnabled() || !LipsyncRuntimeSession || bSharedLipsyncRuntimeFinalized)
    {
        return;
    }

    LipsyncRuntimeSession->Update(GetCurrentOutputPlaybackSeconds());
    SyncSharedLipsyncRuntimeMirrors();
}

void UOffgridAILineCoach::FinalizeSharedLipsyncRuntimeTrack()
{
    if (!IsStreamingLipsyncRuntimeEnabled() || !LipsyncRuntimeSession)
    {
        return;
    }

    LipsyncRuntimeSession->Finalize(GetCurrentOutputPlaybackSeconds());
    SyncSharedLipsyncRuntimeMirrors();

    FOffgridAIAlignedVisemeTrack& RuntimeTrack = LipsyncRuntimeSession->GetMutableCommittedTrack();
    const int32 PreRetimingEventCount = RuntimeTrack.Events.Num();
    SharedLipsyncPreRetimingEventCount = PreRetimingEventCount;
    SharedLipsyncPostRetimingEventCount = PreRetimingEventCount;
    bSharedLipsyncOfflineEvidenceRetimingApplied = false;

    if (bLipsyncDebugFileInitialized && !LipsyncDebugLineDirectory.IsEmpty())
    {
        FString PreRetimingCSV = TEXT("LineID,EventIndex,TextIslandIndex,AudioIslandIndex,PoseID,SourceWord,FinalRenderCenterSec,PlaybackSecAtCommit,CommitLeadSec,CommitReason,ObservedAudioEndSec,SpeechIslandStartSec,SpeechIslandEndSec,CommittedTrackEventCount,AudioNudgeEligible,AudioNudgeSearchPerformed,AudioNudgeAccepted,AudioNudgeSearchMode,AudioNudgeReason,AudioNudgeScheduledCenterSec,AudioNudgeCandidateRawCenterSec,AudioNudgeCandidateRawShiftMs,AudioNudgeCandidateConfidence,AudioNudgeRequiredConfidence,AudioNudgeAvailableBeforeSearchMs,AudioNudgeAvailableAudioStartSec,AudioNudgeAvailableAudioEndSec\n");
        const FString LineString = bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"));
        const float ObservedBufferDurationSec = LipsyncRuntimeSession ? LipsyncRuntimeSession->GetSpeechDetector().GetObservedAudioBufferEndSec() : 0.0f;
        for (const FOffgridAIAlignedVisemeEvent& Event : RuntimeTrack.Events)
        {
            TArray<FString> Columns;
            Columns.Add(LineString);
            Columns.Add(FString::FromInt(Event.EventIndex));
            Columns.Add(FString::FromInt(Event.TextIslandIndex));
            Columns.Add(FString::FromInt(Event.AudioIslandIndex));
            Columns.Add(Event.PoseID.ToString());
            Columns.Add(EscapeDebugCSVString(Event.SourceWord));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.FinalRenderCenterSeconds));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.CommitPlaybackSeconds));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.CommitLeadSeconds));
            Columns.Add(Event.CommitReason.ToString());
            Columns.Add(FString::Printf(TEXT("%.6f"), ObservedBufferDurationSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.IslandAudioStartSeconds));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.IslandAudioEndSeconds));
            Columns.Add(FString::FromInt(RuntimeTrack.Events.Num()));
            Columns.Add(Event.bAudioNudgeEligible ? TEXT("1") : TEXT("0"));
            Columns.Add(Event.bAudioNudgeSearchPerformed ? TEXT("1") : TEXT("0"));
            Columns.Add(Event.bAudioNudgeAccepted ? TEXT("1") : TEXT("0"));
            Columns.Add(EscapeDebugCSVString(Event.AudioNudgeSearchMode.ToString()));
            Columns.Add(EscapeDebugCSVString(Event.AudioNudgeRejectReason.ToString()));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeScheduledCenterSeconds));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeCandidateRawCenterSeconds));
            Columns.Add(FString::Printf(TEXT("%.3f"), Event.AudioNudgeCandidateRawShiftSeconds * 1000.0f));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeCandidateConfidence));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeRequiredConfidence));
            Columns.Add(FString::Printf(TEXT("%.3f"), Event.AudioNudgeAvailableBeforeSearchSeconds * 1000.0f));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeAvailableAudioStartSeconds));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeAvailableAudioEndSeconds));
            PreRetimingCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
        }
        FFileHelper::SaveStringToFile(PreRetimingCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("runtime_commit_events_pre_retime.csv")));
    }

    // AU38 Unreal probe: do not apply the old full-WAV/offline evidence retimer here.
    // The effective track for FaceDriver is now the shared streaming committed track
    // plus ApplyAU38StreamingProsodyGroupReflow(), which is constrained to the
    // currently buffered audio horizon derived from LineCoach preroll. Full audio
    // may still be written for diagnostics, but it must not be used as a timing
    // oracle during playback.
    bSharedLipsyncOfflineEvidenceRetimingApplied = false;
    UE_LOG(LogOffgridAI, Log, TEXT("LineCoach finalized AU39 shared-runtime lipsync track npc=%s line=%s events=%d sample_rate=%d"),
        *NPCID.ToString(),
        *(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))),
        RuntimeTrack.Events.Num(),
        ActiveSampleRate);

    SyncSharedLipsyncRuntimeMirrors();
    bSharedLipsyncRuntimeFinalized = true;
    SharedLipsyncPostRetimingEventCount = ActiveAlignedVisemeTrack.Events.Num();

    WriteLipsyncDebugLineMetadata();
    WriteLipsyncDebugPlannedEventsCSV();
    WriteLipsyncDebugDurationScalingDiagnosticsCSV();
    WriteLipsyncDebugTimingCoverageDiagnosticsCSV();
    WriteLipsyncDebugMotionQualityCSV();
    if (bLipsyncDebugFileInitialized && !LipsyncDebugLineDirectory.IsEmpty())
    {
        FString FinalRuntimeCommitCSV;
        const FString FinalRuntimeCommitPath = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("runtime_commit_events.csv"));
        if (FFileHelper::LoadFileToString(FinalRuntimeCommitCSV, *FinalRuntimeCommitPath))
        {
            FFileHelper::SaveStringToFile(FinalRuntimeCommitCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("runtime_commit_events_post_retime.csv")));
        }
    }
}





FOffgridAILipsyncRuntimeSession& UOffgridAILineCoach::GetOrCreateSharedLipsyncRuntimeSession()
{
    if (!LipsyncRuntimeSession)
    {
        LipsyncRuntimeSession = new FOffgridAILipsyncRuntimeSession();
    }
    return *LipsyncRuntimeSession;
}

void UOffgridAILineCoach::BeginSharedLipsyncRuntimeSession()
{
    if (!bHasActiveLineRequest)
    {
        return;
    }

    FOffgridAILipsyncRuntimeBeginInput Input;
    Input.DialogueText = ActiveLineRequest.Dialogue.ToString();
    Input.NPCID = NPCID;
    Input.LineID = ActiveLineRequest.LineID;
    Input.PrerollSec = GetInitialPrerollSeconds();

    GetOrCreateSharedLipsyncRuntimeSession().BeginLine(Input);
    SyncSharedLipsyncRuntimeMirrors();
}

void UOffgridAILineCoach::SyncSharedLipsyncRuntimeMirrors()
{
    if (!LipsyncRuntimeSession)
    {
        return;
    }

    ActiveTextVisemePlan = LipsyncRuntimeSession->GetTextPlan();
    ActiveAlignedVisemeTrack = LipsyncRuntimeSession->GetCommittedTrack();
    bActiveAlignedVisemeTrackBuilt = LipsyncRuntimeSession->IsCommittedTrackBuilt();
    if (ActiveAlignedVisemeTrack.LineID.IsNone() && bHasActiveLineRequest)
    {
        ActiveAlignedVisemeTrack.LineID = ActiveLineRequest.LineID;
        ActiveAlignedVisemeTrack.NPCID = NPCID;
    }
    // AU39 rollback: AU38/AU38b reflow caused visible low-framerate/stutter in Unreal.
    // Keep the shared streaming committed track authoritative; keep topology metrics only.
    AU38ResetCounters(
        bAU38StreamingGroupReflowApplied,
        AU38StreamingGroupReflowGroupCount,
        AU38StreamingGroupReflowAffineGroupCount,
        AU38StreamingGroupReflowSingleAnchorGroupCount,
        AU38StreamingGroupReflowForwardShiftGroupCount,
        AU38StreamingGroupReflowAppliedEventCount,
        AU38StreamingGroupReflowAnchorCount,
        AU38StreamingGroupReflowMeanAbsEventDeltaMs,
        AU38StreamingGroupReflowMaxAbsEventDeltaMs);
    if (bLipsyncDebugFileInitialized && !LipsyncDebugLineDirectory.IsEmpty())
    {
        WriteAU39RuntimeTopologyMetricsCsv(LipsyncDebugLineDirectory, ActiveAlignedVisemeTrack, GetInitialPrerollSeconds());
    }
    LipsyncEstimatedTextDurationSeconds = ActiveTextVisemePlan.EstimatedDurationSeconds;
}

void UOffgridAILineCoach::ApplyAU38StreamingProsodyGroupReflow()
{
    AU38ResetCounters(
        bAU38StreamingGroupReflowApplied,
        AU38StreamingGroupReflowGroupCount,
        AU38StreamingGroupReflowAffineGroupCount,
        AU38StreamingGroupReflowSingleAnchorGroupCount,
        AU38StreamingGroupReflowForwardShiftGroupCount,
        AU38StreamingGroupReflowAppliedEventCount,
        AU38StreamingGroupReflowAnchorCount,
        AU38StreamingGroupReflowMeanAbsEventDeltaMs,
        AU38StreamingGroupReflowMaxAbsEventDeltaMs);

    if (!bActiveAlignedVisemeTrackBuilt || ActiveAlignedVisemeTrack.Events.Num() <= 0 ||
        ActiveSampleRate <= 0 || LipsyncAnalysisSamples.Num() <= 0)
    {
        return;
    }

    const float PrerollSec = FMath::Max(0.025f, GetInitialPrerollSeconds());
    const float PlaybackSec = GetCurrentOutputPlaybackSeconds();
    const float VisibleAudioEndSec = FMath::Min(
        LipsyncObservedAudioDurationSeconds,
        PlaybackSec + PrerollSec);
    if (VisibleAudioEndSec <= 0.0f)
    {
        return;
    }

    TArray<int16> MonoPCM16;
    MonoPCM16.Reserve(LipsyncAnalysisSamples.Num());
    for (const float Sample : LipsyncAnalysisSamples)
    {
        const int32 Scaled = FMath::RoundToInt(FMath::Clamp(Sample, -1.0f, 1.0f) * 32767.0f);
        MonoPCM16.Add(static_cast<int16>(FMath::Clamp(Scaled, -32768, 32767)));
    }

    TArray<FOffgridAIAudioPlannerFeaturePoint> Features = BuildAudioPlannerFeaturesFromPCM16(MonoPCM16, ActiveSampleRate);
    if (Features.Num() <= 0)
    {
        return;
    }

    const float AudioTimeBaseSec = static_cast<float>(LipsyncAnalysisSampleBaseIndex) / static_cast<float>(FMath::Max(ActiveSampleRate, 1));
    // AU38b guardrails: conservative runtime-feasible reflow. All timing derives from preroll.
    const float MinGapSec = FMath::Max(0.025f, PrerollSec * 0.18f);
    const float MaxEventDeltaSec = FMath::Max(0.025f, PrerollSec * 0.50f);
    const float MaxGroupShiftSec = FMath::Max(0.025f, PrerollSec * 0.40f);
    const float MaxForwardShiftSec = FMath::Max(0.020f, PrerollSec * 0.35f);
    const float MaxDeadAirSec = FMath::Max(0.060f, PrerollSec * 0.75f);
    const float MinScale = 0.85f;
    const float MaxScale = 1.15f;
    int32 AU38bScaleClampedGroupCount = 0;
    int32 AU38bGroupShiftClampedCount = 0;
    int32 AU38bForwardShiftClampedCount = 0;
    int32 AU38bEventDeltaClampedCount = 0;

    TMap<FString, FOffgridAIAU38GroupTransform> Groups;
    for (int32 I = 0; I < ActiveAlignedVisemeTrack.Events.Num(); ++I)
    {
        const FOffgridAIAlignedVisemeEvent& Event = ActiveAlignedVisemeTrack.Events[I];
        const int32 GroupIndex = AU38GetEventProsodyGroupIndex(Event);
        const FString Key = AU38GroupKey(Event.TextIslandIndex, GroupIndex);
        FOffgridAIAU38GroupTransform& Group = Groups.FindOrAdd(Key);
        Group.IslandIndex = Event.TextIslandIndex;
        Group.GroupIndex = GroupIndex;
        Group.TrackOrdinals.Add(I);
        Group.OriginalStartSec = FMath::Min(Group.OriginalStartSec, Event.FinalRenderCenterSeconds);
        Group.OriginalEndSec = FMath::Max(Group.OriginalEndSec, Event.FinalRenderCenterSeconds);
    }

    for (int32 I = 0; I < ActiveAlignedVisemeTrack.Events.Num(); ++I)
    {
        const FOffgridAIAlignedVisemeEvent& Event = ActiveAlignedVisemeTrack.Events[I];
        FOffgridAIAU38AnchorMatch Match;
        if (!AU38FindBestAnchorForEvent(Event, I, Features, AudioTimeBaseSec, VisibleAudioEndSec, PrerollSec, Match))
        {
            continue;
        }

        const FString Key = AU38GroupKey(Match.IslandIndex, Match.GroupIndex);
        if (FOffgridAIAU38GroupTransform* Group = Groups.Find(Key))
        {
            Group->UsableAnchors.Add(Match);
            ++AU38StreamingGroupReflowAnchorCount;
        }
    }

    for (TPair<FString, FOffgridAIAU38GroupTransform>& Pair : Groups)
    {
        FOffgridAIAU38GroupTransform& Group = Pair.Value;
        if (!FMath::IsFinite(Group.OriginalStartSec) || !FMath::IsFinite(Group.OriginalEndSec))
        {
            Group.OriginalStartSec = 0.0f;
            Group.OriginalEndSec = 0.0f;
        }

        Group.UsableAnchors.Sort([](const FOffgridAIAU38AnchorMatch& A, const FOffgridAIAU38AnchorMatch& B)
        {
            return A.CommittedCenterSec < B.CommittedCenterSec;
        });

        if (Group.UsableAnchors.Num() == 1)
        {
            Group.Mode = TEXT("single_anchor_shift");
            const float RawShift = Group.UsableAnchors[0].DeltaSec;
            Group.ShiftSec = FMath::Clamp(RawShift, -MaxGroupShiftSec, MaxGroupShiftSec);
            if (FMath::Abs(Group.ShiftSec - RawShift) > 0.0005f)
            {
                ++AU38bGroupShiftClampedCount;
                Group.Mode = TEXT("single_anchor_shift_clamped");
            }
            ++AU38StreamingGroupReflowSingleAnchorGroupCount;
        }
        else if (Group.UsableAnchors.Num() >= 2)
        {
            Group.Mode = Group.UsableAnchors.Num() == 2 ? TEXT("two_anchor_affine") : TEXT("multi_anchor_affine");
            const FOffgridAIAU38AnchorMatch& First = Group.UsableAnchors[0];
            const FOffgridAIAU38AnchorMatch& Last = Group.UsableAnchors.Last();
            const float PlannedSpan = FMath::Max(0.001f, Last.CommittedCenterSec - First.CommittedCenterSec);
            const float AudioSpan = FMath::Max(0.001f, Last.CandidateTimeSec - First.CandidateTimeSec);
            const float RawScale = AudioSpan / PlannedSpan;
            Group.Scale = FMath::Clamp(RawScale, MinScale, MaxScale);
            if (FMath::Abs(Group.Scale - RawScale) > 0.0005f)
            {
                ++AU38bScaleClampedGroupCount;
                Group.Mode = FName(*(Group.Mode.ToString() + TEXT("_scale_clamped")));
            }
            const float RawShift = First.CandidateTimeSec - (Group.Scale * First.CommittedCenterSec);
            const float ShiftAtGroupStart = (Group.Scale * Group.OriginalStartSec + RawShift) - Group.OriginalStartSec;
            const float ClampedShiftAtStart = FMath::Clamp(ShiftAtGroupStart, -MaxGroupShiftSec, MaxGroupShiftSec);
            if (FMath::Abs(ClampedShiftAtStart - ShiftAtGroupStart) > 0.0005f)
            {
                ++AU38bGroupShiftClampedCount;
                Group.Mode = FName(*(Group.Mode.ToString() + TEXT("_shift_clamped")));
            }
            Group.ShiftSec = RawShift + (ClampedShiftAtStart - ShiftAtGroupStart);
            ++AU38StreamingGroupReflowAffineGroupCount;
        }
    }

    TArray<FOffgridAIAU38GroupTransform*> OrderedGroups;
    OrderedGroups.Reserve(Groups.Num());
    for (TPair<FString, FOffgridAIAU38GroupTransform>& Pair : Groups)
    {
        OrderedGroups.Add(&Pair.Value);
    }
    OrderedGroups.Sort([](const FOffgridAIAU38GroupTransform& A, const FOffgridAIAU38GroupTransform& B)
    {
        if (A.IslandIndex != B.IslandIndex)
        {
            return A.IslandIndex < B.IslandIndex;
        }
        if (!FMath::IsNearlyEqual(A.OriginalStartSec, B.OriginalStartSec))
        {
            return A.OriginalStartSec < B.OriginalStartSec;
        }
        return A.GroupIndex < B.GroupIndex;
    });

    bool bHasPrevInIsland = false;
    int32 PrevIsland = INDEX_NONE;
    float PrevOriginalEnd = 0.0f;
    float PrevCorrectedEnd = 0.0f;
    for (FOffgridAIAU38GroupTransform* GroupPtr : OrderedGroups)
    {
        if (!GroupPtr)
        {
            continue;
        }
        FOffgridAIAU38GroupTransform& Group = *GroupPtr;
        if (PrevIsland != Group.IslandIndex)
        {
            bHasPrevInIsland = false;
            PrevIsland = Group.IslandIndex;
        }

        float CorrectedStart = AU38ApplyGroupTransform(Group, Group.OriginalStartSec);
        float CorrectedEnd = AU38ApplyGroupTransform(Group, Group.OriginalEndSec);
        if (bHasPrevInIsland)
        {
            const float OriginalGap = Group.OriginalStartSec - PrevOriginalEnd;
            const float TargetGap = FMath::Clamp(OriginalGap, MinGapSec, MaxDeadAirSec);
            const float DesiredStart = PrevCorrectedEnd + TargetGap;
            const float RawForwardDelta = DesiredStart - CorrectedStart;
            const float ClampedForwardDelta = FMath::Clamp(RawForwardDelta, -MaxForwardShiftSec, MaxForwardShiftSec);
            if (FMath::Abs(ClampedForwardDelta - RawForwardDelta) > 0.0005f)
            {
                ++AU38bForwardShiftClampedCount;
            }
            Group.ForwardShiftSec += ClampedForwardDelta;
            CorrectedStart = AU38ApplyGroupTransform(Group, Group.OriginalStartSec);
            CorrectedEnd = AU38ApplyGroupTransform(Group, Group.OriginalEndSec);
            if (FMath::Abs(Group.ForwardShiftSec) > 0.0005f)
            {
                ++AU38StreamingGroupReflowForwardShiftGroupCount;
            }
        }

        PrevOriginalEnd = Group.OriginalEndSec;
        PrevCorrectedEnd = CorrectedEnd;
        bHasPrevInIsland = true;
    }

    TArray<float> NewCenters;
    TArray<FName> EventModes;
    NewCenters.SetNumZeroed(ActiveAlignedVisemeTrack.Events.Num());
    EventModes.SetNum(ActiveAlignedVisemeTrack.Events.Num());
    for (int32 I = 0; I < EventModes.Num(); ++I)
    {
        EventModes[I] = TEXT("identity");
    }

    for (const TPair<FString, FOffgridAIAU38GroupTransform>& Pair : Groups)
    {
        const FOffgridAIAU38GroupTransform& Group = Pair.Value;
        for (const int32 Ordinal : Group.TrackOrdinals)
        {
            if (ActiveAlignedVisemeTrack.Events.IsValidIndex(Ordinal))
            {
                const float OldCenter = ActiveAlignedVisemeTrack.Events[Ordinal].FinalRenderCenterSeconds;
                const float RawCenter = AU38ApplyGroupTransform(Group, OldCenter);
                const float ClampedCenter = OldCenter + FMath::Clamp(RawCenter - OldCenter, -MaxEventDeltaSec, MaxEventDeltaSec);
                if (FMath::Abs(ClampedCenter - RawCenter) > 0.0005f)
                {
                    ++AU38bEventDeltaClampedCount;
                }
                NewCenters[Ordinal] = ClampedCenter;
                EventModes[Ordinal] = Group.Mode;
            }
        }
    }

    int32 MonotonicRepairCount = 0;
    for (int32 I = 0; I < NewCenters.Num(); ++I)
    {
        if (I > 0 && NewCenters[I] < NewCenters[I - 1] + MinGapSec)
        {
            NewCenters[I] = NewCenters[I - 1] + MinGapSec;
            ++MonotonicRepairCount;
        }
    }

    float AbsDeltaSumMs = 0.0f;
    for (int32 I = 0; I < ActiveAlignedVisemeTrack.Events.Num(); ++I)
    {
        FOffgridAIAlignedVisemeEvent& Event = ActiveAlignedVisemeTrack.Events[I];
        const float OldCenter = Event.FinalRenderCenterSeconds;
        float NewCenter = NewCenters[I];

        // AU38 is deliberately a probe of the LipLab policy, but do not rewrite
        // events that are already materially in the past for the live FaceDriver.
        if (OldCenter < PlaybackSec - 0.020f)
        {
            NewCenter = OldCenter;
        }

        const float Delta = NewCenter - OldCenter;
        if (FMath::Abs(Delta) > 0.0005f)
        {
            ++AU38StreamingGroupReflowAppliedEventCount;
            const float AbsDeltaMs = FMath::Abs(Delta) * 1000.0f;
            AbsDeltaSumMs += AbsDeltaMs;
            AU38StreamingGroupReflowMaxAbsEventDeltaMs = FMath::Max(AU38StreamingGroupReflowMaxAbsEventDeltaMs, AbsDeltaMs);

            Event.FinalRenderCenterSeconds = NewCenter;
            Event.RenderStartSeconds += Delta;
            Event.RenderEndSeconds += Delta;
            Event.AppliedShiftSeconds += Delta;
            Event.RawShiftSeconds += Delta;
            Event.CommitReason = EventModes[I] == FName(TEXT("identity")) ? FName(TEXT("au38_group_reflow")) : EventModes[I];
        }
    }

    AU38StreamingGroupReflowGroupCount = Groups.Num();
    AU38StreamingGroupReflowMeanAbsEventDeltaMs = AU38StreamingGroupReflowAppliedEventCount > 0
        ? AbsDeltaSumMs / static_cast<float>(AU38StreamingGroupReflowAppliedEventCount)
        : 0.0f;
    bAU38StreamingGroupReflowApplied = AU38StreamingGroupReflowAppliedEventCount > 0 || AU38StreamingGroupReflowAnchorCount > 0;

    if (bLipsyncDebugFileInitialized && !LipsyncDebugLineDirectory.IsEmpty())
    {
        FString CSV = TEXT("LineID,EventIndex,TextIslandIndex,GroupIndex,PoseID,SourceWord,OriginalCenterSec,AU38CenterSec,DeltaSec,ReflowMode,PrerollSec,VisibleAudioEndSec,PlaybackSec,MonotonicRepairCount,AU38bMinGapSec,AU38bMaxEventDeltaSec,AU38bMaxGroupShiftSec,AU38bMaxForwardShiftSec,AU38bScaleClampedGroupCount,AU38bGroupShiftClampedCount,AU38bForwardShiftClampedCount,AU38bEventDeltaClampedCount\n");
        const FString LineString = bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"));
        for (int32 I = 0; I < ActiveAlignedVisemeTrack.Events.Num(); ++I)
        {
            const FOffgridAIAlignedVisemeEvent& Event = ActiveAlignedVisemeTrack.Events[I];
            TArray<FString> Columns;
            Columns.Add(LineString);
            Columns.Add(FString::FromInt(Event.EventIndex));
            Columns.Add(FString::FromInt(Event.TextIslandIndex));
            Columns.Add(FString::FromInt(AU38GetEventProsodyGroupIndex(Event)));
            Columns.Add(Event.PoseID.ToString());
            Columns.Add(EscapeDebugCSVString(Event.SourceWord));
            const float OriginalCenter = Event.FinalRenderCenterSeconds - Event.AppliedShiftSeconds;
            Columns.Add(FString::Printf(TEXT("%.6f"), OriginalCenter));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.FinalRenderCenterSeconds));
            Columns.Add(FString::Printf(TEXT("%.6f"), Event.FinalRenderCenterSeconds - OriginalCenter));
            Columns.Add(Event.CommitReason.ToString());
            Columns.Add(FString::Printf(TEXT("%.6f"), PrerollSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), VisibleAudioEndSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), PlaybackSec));
            Columns.Add(FString::FromInt(MonotonicRepairCount));
            Columns.Add(FString::Printf(TEXT("%.6f"), MinGapSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), MaxEventDeltaSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), MaxGroupShiftSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), MaxForwardShiftSec));
            Columns.Add(FString::FromInt(AU38bScaleClampedGroupCount));
            Columns.Add(FString::FromInt(AU38bGroupShiftClampedCount));
            Columns.Add(FString::FromInt(AU38bForwardShiftClampedCount));
            Columns.Add(FString::FromInt(AU38bEventDeltaClampedCount));
            CSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
        }
        FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("au38_reflow_runtime_commit_events.csv")));
    }
}

void UOffgridAILineCoach::ForceNeutral()

{
    ResetLipsyncRuntimeState();

    bHasReceivedAnalysis = false;
    bOutputPlaybackStarted = false;
    bOutputPlaybackPausedForUnderrun = false;
    SubmittedOutputBytes = 0;
    QueuedOutputBytes = 0;
    OutputPlaybackStartTimeSeconds = 0.0;
    OutputDrainZeroSinceSeconds = 0.0;
    PendingOutputPCM.Reset();
    PendingOutputPCMReadOffset = 0;

    InitializeEmotionMapsIfNeeded();

    ActiveLineEmotion = NAME_None;
    ActiveLineEmotionMagnitude = 0.0f;

    CurrentFacialFrame = FOffgridAIFacialFrame();
    CurrentFacialFrame.NPCID = NPCID;
    CurrentFacialFrame.LineID = bHasActiveLineRequest ? ActiveLineRequest.LineID : NAME_None;
    CurrentFacialFrame.EmotionWeights.Reset();

    if (UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
    {
        FaceDriver->ForceNeutral();
    }

    BroadcastFacialFrameIfChanged();
}

void UOffgridAILineCoach::UpdateLipsyncEnergyFromPCM(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels)
{
    if (SampleRate <= 0 || NumChannels <= 0 || BytesToUse <= 0)
    {
        return;
    }

    const int32 BytesPerFrame = NumChannels * static_cast<int32>(sizeof(int16));
    const int32 FrameCount = BytesToUse / BytesPerFrame;
    if (FrameCount <= 0)
    {
        return;
    }

    const int16* Samples = reinterpret_cast<const int16*>(PCMChunk.GetData());
    double SumSquares = 0.0;
    for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
    {
        float Mono = 0.0f;
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 SampleIndex = (FrameIndex * NumChannels) + ChannelIndex;
            Mono += static_cast<float>(Samples[SampleIndex]) / 32768.0f;
        }
        Mono /= static_cast<float>(NumChannels);
        SumSquares += static_cast<double>(Mono) * static_cast<double>(Mono);
    }

    LipsyncRawRMS = FMath::Sqrt(static_cast<float>(SumSquares / static_cast<double>(FrameCount)));
    LipsyncEnergyPeak = FMath::Max(LipsyncEnergyPeak * 0.997f, LipsyncRawRMS);

    const float BaseThreshold = GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanSpeechOnsetRMSThreshold, 0.010f);
    const float EnvelopeFloor = BaseThreshold * 0.35f;
    const float Normalized = Clamp01((LipsyncRawRMS - EnvelopeFloor) / FMath::Max(LipsyncEnergyPeak - EnvelopeFloor, 0.0001f));
    const float SpeechActivity = FMath::Pow(SmoothStep01(0.0f, 1.0f, Normalized), 0.35f);

    const float AttackMs = GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanEnergyAttackMs, 12.0f);
    const float ReleaseMs = GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanEnergyReleaseMs, 85.0f);
    const float DeltaSeconds = static_cast<float>(FrameCount) / static_cast<float>(SampleRate);
    ApplyLipsyncSmoothing(LipsyncEnergyEnvelope, SpeechActivity, AttackMs, ReleaseMs, DeltaSeconds);

    LipsyncObservedAudioDurationSeconds += DeltaSeconds;
}

void UOffgridAILineCoach::AppendPCMToLipsyncAnalysisBuffer(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels)
{
    const int32 BytesPerFrame = NumChannels > 0 ? NumChannels * static_cast<int32>(sizeof(int16)) : static_cast<int32>(sizeof(int16));
    const int32 InferredSampleCount = BytesPerFrame > 0 ? BytesToUse / BytesPerFrame : 0;
    AppendPCMToLipsyncAnalysisBuffer(PCMChunk, BytesToUse, SampleRate, NumChannels, LipsyncAnalysisSampleBaseIndex + LipsyncAnalysisSamples.Num(), InferredSampleCount);
}

void UOffgridAILineCoach::AppendPCMToLipsyncAnalysisBuffer(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample, int32 ChunkSampleCount)
{
    if (SampleRate <= 0 || NumChannels <= 0 || BytesToUse <= 0)
    {
        return;
    }

    const int32 BytesPerFrame = NumChannels * static_cast<int32>(sizeof(int16));
    const int32 FrameCount = BytesToUse / BytesPerFrame;
    if (FrameCount <= 0)
    {
        return;
    }

    const int64 ExpectedNextSample = LipsyncAnalysisSampleBaseIndex + LipsyncAnalysisSamples.Num();
    if (ChunkStartSample >= 0 && ChunkStartSample != ExpectedNextSample)
    {
        // Phase 1A: the TTS service now annotates the synthesized sample range for
        // each chunk. If a future service ever drops, coalesces, or reorders chunks,
        // keep the lipsync analysis timeline aligned to synthesized sample time
        // rather than silently drifting on receive order. Current qwen transport
        // should be strictly contiguous, so this should only log during protocol bugs.
        UE_LOG(LogOffgridAI, Warning, TEXT("LineCoach TTS chunk sample timeline discontinuity npc=%s line=%s expected_start=%lld actual_start=%lld samples=%d"),
            *NPCID.ToString(),
            *(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))),
            ExpectedNextSample,
            ChunkStartSample,
            ChunkSampleCount);

        LipsyncAnalysisSamples.Reset();
        LipsyncAnalysisSampleBaseIndex = ChunkStartSample;
        LipsyncNextFrameStartSample = FMath::Max<int64>(LipsyncNextFrameStartSample, ChunkStartSample);
    }

    UpdateLipsyncEnergyFromPCM(PCMChunk, BytesToUse, SampleRate, NumChannels);

    const int16* Samples = reinterpret_cast<const int16*>(PCMChunk.GetData());
    LipsyncAnalysisSamples.Reserve(LipsyncAnalysisSamples.Num() + FrameCount);

    for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
    {
        float Mono = 0.0f;
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 SampleIndex = (FrameIndex * NumChannels) + ChannelIndex;
            Mono += static_cast<float>(Samples[SampleIndex]) / 32768.0f;
        }
        const float MonoSample = Mono / static_cast<float>(NumChannels);
        LipsyncAnalysisSamples.Add(MonoSample);
        AppendLipsyncDebugInputSample(MonoSample, SampleRate);
    }

    bHasReceivedAnalysis = true;
}

void UOffgridAILineCoach::ApplyLipsyncSmoothing(float& SmoothedValue, float RawValue, float AttackMs, float ReleaseMs, float DeltaTimeSeconds) const
{
    const float TauMs = RawValue > SmoothedValue ? AttackMs : ReleaseMs;
    const float TauSeconds = FMath::Max(TauMs * 0.001f, 0.001f);
    const float Alpha = 1.0f - FMath::Exp(-DeltaTimeSeconds / TauSeconds);
    SmoothedValue = Clamp01(SmoothedValue + Alpha * (RawValue - SmoothedValue));
}


float UOffgridAILineCoach::ComputePCMDrivenSpeechActivityAtPlaybackTime(float PlaybackSeconds) const
{
    if (ActiveSampleRate <= 0 || LipsyncAnalysisSamples.Num() <= 0 || PlaybackSeconds < 0.0f)
    {
        return 0.0f;
    }

    const int64 CenterSample = LipsyncAnalysisSampleBaseIndex + FMath::RoundToInt64(static_cast<double>(PlaybackSeconds) * static_cast<double>(ActiveSampleRate));
    const int32 HalfWindowFrames = FMath::Clamp(FMath::RoundToInt(static_cast<float>(ActiveSampleRate) * 0.0125f), 96, 512);
    const int64 StartSample = FMath::Max<int64>(CenterSample - HalfWindowFrames, LipsyncAnalysisSampleBaseIndex);
    const int64 EndSample = FMath::Min<int64>(CenterSample + HalfWindowFrames, LipsyncAnalysisSampleBaseIndex + LipsyncAnalysisSamples.Num());
    if (EndSample <= StartSample)
    {
        return 0.0f;
    }

    double SumSquares = 0.0;
    for (int64 Sample = StartSample; Sample < EndSample; ++Sample)
    {
        const int32 LocalIndex = static_cast<int32>(Sample - LipsyncAnalysisSampleBaseIndex);
        const float V = LipsyncAnalysisSamples.IsValidIndex(LocalIndex) ? LipsyncAnalysisSamples[LocalIndex] : 0.0f;
        SumSquares += static_cast<double>(V) * static_cast<double>(V);
    }

    const float RMS = FMath::Sqrt(static_cast<float>(SumSquares / static_cast<double>(EndSample - StartSample)));
    const float Threshold = GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanSpeechOnsetRMSThreshold, 0.010f) * 0.65f;
    const float Peak = FMath::Max(LipsyncEnergyPeak, Threshold + 0.0001f);
    return Clamp01((RMS - Threshold * 0.65f) / FMath::Max(Peak - Threshold * 0.65f, 0.0001f));
}


const FOffgridAIAlignedVisemeEvent* UOffgridAILineCoach::FindActiveAlignedVisemeEvent(int32 EventIndex) const
{
    if (!bActiveAlignedVisemeTrackBuilt)
    {
        return nullptr;
    }

    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        if (Event.EventIndex == EventIndex)
        {
            return &Event;
        }
    }
    return nullptr;
}



TArray<FOffgridAISubmittedVisemeSample> UOffgridAILineCoach::SampleCommittedVisemeSamples(float PlaybackSeconds) const
{
    LastSubmittedVisemeSamples.Reset();
    if (!bActiveAlignedVisemeTrackBuilt || ActiveAlignedVisemeTrack.Events.Num() <= 0)
    {
        return LastSubmittedVisemeSamples;
    }

    LastSubmittedVisemeSamples = FOffgridAIVisemePerformer::Sample(ActiveAlignedVisemeTrack, PlaybackSeconds, true);
    return LastSubmittedVisemeSamples;
}

TMap<FName, float> UOffgridAILineCoach::CollapseSubmittedVisemeSamplesToPoseWeights(const TArray<FOffgridAISubmittedVisemeSample>& Samples) const
{
    return FOffgridAIVisemePerformer::CollapseByPoseID(Samples);
}

TMap<FName, float> UOffgridAILineCoach::SampleDirectVisemePoseWeights(float PlaybackSeconds) const
{
    return CollapseSubmittedVisemeSamplesToPoseWeights(SampleCommittedVisemeSamples(PlaybackSeconds));
}


void UOffgridAILineCoach::UpdateLipsyncFromPlayback(float DeltaTime)
{
    if (!bOutputPlaybackStarted)
    {
        return;
    }


    const float PlaybackSec = GetCurrentOutputPlaybackSeconds();
    const float Layer3SamplePlaybackSec = (IsStreamingLipsyncRuntimeEnabled() && LipsyncRuntimeSession)
        ? LipsyncRuntimeSession->GetPlaybackSeconds()
        : PlaybackSec;
    const float RestClosed = GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanRestClosedWeight, 0.035f);

    auto SetRestTarget = [&]()
    {
        FOffgridAILipsyncPoseRuntimeState RestPose;
        RestPose.Closed = RestClosed;
        TargetDisplayedLipsyncPoseState = RestPose;
        LastCurveSampledPoseState = RestPose;
        bHasDisplayedLipsyncTarget = true;
        CurrentFacialFrame.NPCID = NPCID;
        CurrentFacialFrame.LineID = bHasActiveLineRequest ? ActiveLineRequest.LineID : NAME_None;
        LastResolvedDriverVisemeWeights.Reset();
        LastSubmittedVisemeSamples.Reset();
        LastResolvedDriverVisemePlaybackSeconds = Layer3SamplePlaybackSec;

        AppendLipsyncDebugCSV(TEXT("TEXT_PLAN"), PlaybackSec, nullptr, FOffgridAILipsyncPoseRuntimeState(), TargetDisplayedLipsyncPoseState, TargetDisplayedLipsyncPoseState);
    };

    // AU28: the shared runtime's aligned track is the only lipsync timing source.
    // LineCoach never runs a competing speech-onset or duration clock.
    const bool bUseAuthoritativeAlignedTrack = bActiveAlignedVisemeTrackBuilt && ActiveAlignedVisemeTrack.Events.Num() > 0;

    if (!bUseAuthoritativeAlignedTrack)
    {
        // No legacy fail-open or PCM bucket fallback in text-planned mode.
        // If the committed aligned track is not ready, hold rest.  Playback
        // should normally be gated until the track exists; animating raw plans or
        // analysis buckets here reintroduces the old out-of-phase behavior.
        SetRestTarget();
        return;
    }

    if (bUseAuthoritativeAlignedTrack)
    {
        // Authoritative phase contract: once playback has a committed aligned
        // track, the submitter samples that immutable timing domain.  Do not
        // rebuild/refine provisional tracks from inside the playback loop; that
        // reintroduced mixed raw/provisional centers into submitted_poses.csv.

        // v32d: Never animate the mouth through leading breath/silence just
        // because a provisional text event exists. Allow a small anticipation
        // window before detected speech, but otherwise hold rest until the
        // authoritative layer-2 speech span begins.
        constexpr float PreSpeechAnticipationAllowanceSeconds = 0.025f;
        const float AuthoritativeSpeechStartSec = ActiveAlignedVisemeTrack.SpeechStartSeconds;
        if (Layer3SamplePlaybackSec < AuthoritativeSpeechStartSec - PreSpeechAnticipationAllowanceSeconds)
        {
            SetRestTarget();
            return;
        }
    }

    // v22: stop only after the authoritative aligned track has no possible active
    // event left. Do not use detected speech-end fade to cut off final phonemes.
    if (!bOutputStreamOpen && bUseAuthoritativeAlignedTrack)
    {
        float LastEventSec = 0.0f;
        for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
        {
            LastEventSec = FMath::Max(LastEventSec, Event.FinalRenderCenterSeconds);
        }
        if (Layer3SamplePlaybackSec > LastEventSec + 0.24f)
        {
            SetRestTarget();
            return;
        }
    }
    else if (!bOutputStreamOpen && !bUseAuthoritativeAlignedTrack && LipsyncObservedAudioDurationSeconds > 0.0f && PlaybackSec >= LipsyncObservedAudioDurationSeconds + 0.08f)
    {
        SetRestTarget();
        return;
    }

    // v21 replacement layer 3: sample exact planned PoseIDs at layer-2 final centers.
    // No PCM carrier, canonical bucket rebuild, perceptual hierarchy, terminal fade,
    // or hidden strength attenuation is allowed in this path.
    LastResolvedDriverVisemeWeights = SampleDirectVisemePoseWeights(Layer3SamplePlaybackSec);
    LastResolvedDriverVisemePlaybackSeconds = Layer3SamplePlaybackSec;

    FOffgridAILipsyncPoseRuntimeState SampledPose = BuildPoseStateFromDirectVisemeWeights(LastResolvedDriverVisemeWeights);
    SampledPose.Closed = FMath::Max(SampledPose.Closed, RestClosed);

    TargetDisplayedLipsyncPoseState = SampledPose;
    LastCurveSampledPoseState = SampledPose;
    bHasDisplayedLipsyncTarget = true;
    CurrentFacialFrame.NPCID = NPCID;
    CurrentFacialFrame.LineID = bHasActiveLineRequest ? ActiveLineRequest.LineID : NAME_None;

    AppendLipsyncDebugCSV(TEXT("TEXT_PLAN"), PlaybackSec, nullptr, FOffgridAILipsyncPoseRuntimeState(), TargetDisplayedLipsyncPoseState, TargetDisplayedLipsyncPoseState);
}

void UOffgridAILineCoach::UpdateDisplayedLipsyncPose(float DeltaTime)
{
    if (!bHasDisplayedLipsyncTarget)
    {
        return;
    }

    const float SafeDeltaTime = FMath::Max(DeltaTime, 0.0f);
    const float RestClosed = GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanRestClosedWeight, 0.035f);

    // After the shared aligned track has ended, force both target and displayed
    // pose to rest.
    if (bActiveAlignedVisemeTrackBuilt
        && !bOutputStreamOpen
        && ActiveAlignedVisemeTrack.SpeechEndSeconds > 0.0f
        && GetCurrentOutputPlaybackSeconds() >= ActiveAlignedVisemeTrack.SpeechEndSeconds + 0.070f)
    {
        FOffgridAILipsyncPoseRuntimeState RestPose;
        RestPose.Closed = RestClosed;
        TargetDisplayedLipsyncPoseState = RestPose;
        CurrentDisplayedLipsyncPoseState = RestPose;
        LastCurveSampledPoseState = RestPose;
        LastResolvedDriverVisemeWeights.Reset();
        LastSubmittedVisemeSamples.Reset();
        LastResolvedDriverVisemePlaybackSeconds = GetCurrentOutputPlaybackSeconds();
        if (UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
        {
            // v27: end release should ease out, not hard-pop. Direct viseme
            // submission has already stopped here; let FaceDriver use its
            // explicit post-speech release envelope.
            FaceDriver->ClearVisemes();
        }
        AppendLipsyncDebugCSV(TEXT("DISPLAY"), GetCurrentOutputPlaybackSeconds(), nullptr, FOffgridAILipsyncPoseRuntimeState(), TargetDisplayedLipsyncPoseState, CurrentDisplayedLipsyncPoseState);
        SetCurrentFacialMouthPoseFromState(CurrentDisplayedLipsyncPoseState);
        return;
    }

    auto StepPose = [SafeDeltaTime](float Current, float Target, float AttackMs, float ReleaseMs) -> float
    {
        const float TauMs = Target > Current ? AttackMs : ReleaseMs;
        const float TauSeconds = FMath::Max(TauMs * 0.001f, 0.001f);
        const float Alpha = Clamp01(1.0f - FMath::Exp(-SafeDeltaTime / TauSeconds));
        return Clamp01(FMath::Lerp(Current, Target, Alpha));
    };

    // One global output blend made the face feel like curves were being driven,
    // not muscles. Use pose-family dynamics instead: MBP snaps shut, vowels glide,
    // FVS/WUH are quick accents, and line-end release is firm enough to prevent
    // between-line twitching.
    const bool bLineEndingOrIdle = !bOutputPlaybackStarted || (!bOutputStreamOpen && !HasPendingOrBufferedOutputAudio());
    const float EndMultiplier = bLineEndingOrIdle ? 0.45f : 1.0f;

    CurrentDisplayedLipsyncPoseState.Closed = StepPose(
        CurrentDisplayedLipsyncPoseState.Closed,
        FMath::Max(TargetDisplayedLipsyncPoseState.Closed, RestClosed),
        GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanMBPAttackMs, 18.0f),
        FMath::Min(GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanMBPReleaseMs, 72.0f), 46.0f) * EndMultiplier);
    CurrentDisplayedLipsyncPoseState.Open = StepPose(
        CurrentDisplayedLipsyncPoseState.Open,
        TargetDisplayedLipsyncPoseState.Open,
        GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanVowelAttackMs, 56.0f),
        FMath::Min(GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanVowelReleaseMs, 124.0f), 78.0f) * EndMultiplier);
    CurrentDisplayedLipsyncPoseState.Wide = StepPose(
        CurrentDisplayedLipsyncPoseState.Wide,
        TargetDisplayedLipsyncPoseState.Wide,
        GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanVowelAttackMs, 56.0f),
        FMath::Min(GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanVowelReleaseMs, 124.0f), 78.0f) * EndMultiplier);
    CurrentDisplayedLipsyncPoseState.Round = StepPose(
        CurrentDisplayedLipsyncPoseState.Round,
        TargetDisplayedLipsyncPoseState.Round,
        GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanRoundAttackMs, 48.0f),
        FMath::Min(GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanRoundReleaseMs, 140.0f), 86.0f) * EndMultiplier);
    CurrentDisplayedLipsyncPoseState.Funnel = StepPose(
        CurrentDisplayedLipsyncPoseState.Funnel,
        TargetDisplayedLipsyncPoseState.Funnel,
        GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanAccentAttackMs, 24.0f),
        FMath::Min(GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanAccentReleaseMs, 84.0f), 54.0f) * EndMultiplier);
    CurrentDisplayedLipsyncPoseState.Teeth = StepPose(
        CurrentDisplayedLipsyncPoseState.Teeth,
        TargetDisplayedLipsyncPoseState.Teeth,
        GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanAccentAttackMs, 24.0f),
        FMath::Min(GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanAccentReleaseMs, 84.0f), 54.0f) * EndMultiplier);

    // Final display smoothing should not re-apply dominance shaping; doing so
    // creates visible pops as the smoothed pose crosses threshold boundaries.

    AppendLipsyncDebugCSV(TEXT("DISPLAY"), GetCurrentOutputPlaybackSeconds(), nullptr, FOffgridAILipsyncPoseRuntimeState(), TargetDisplayedLipsyncPoseState, CurrentDisplayedLipsyncPoseState);

    SetCurrentFacialMouthPoseFromState(CurrentDisplayedLipsyncPoseState);
}

void UOffgridAILineCoach::SetCurrentFacialMouthPoseFromState(const FOffgridAILipsyncPoseRuntimeState& PoseState)
{
    CurrentFacialFrame.NPCID = NPCID;
    CurrentFacialFrame.LineID = bHasActiveLineRequest ? ActiveLineRequest.LineID : NAME_None;
    SetPoseMapFromState(CurrentFacialFrame.MouthPoseWeights, PoseState);
    CurrentFacialFrame.JawOpen = CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("AAA"));
    CurrentFacialFrame.LipRound = FMath::Max(CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("OOO")), CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("WUH")));
    CurrentFacialFrame.TeethShow = CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("FVS"));
    CurrentFacialFrame.LipCompression = CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("LipCompression"));
    CurrentFacialFrame.CornerPull = CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("CornerPull"));
    CurrentFacialFrame.LipFunnel = CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("LipFunnel"));
    CurrentFacialFrame.LipStretch = CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("LipStretch"));
    CurrentFacialFrame.LowerLipDown = CurrentFacialFrame.MouthPoseWeights.FindRef(TEXT("LowerLipDown"));

    if (UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
    {
        if (!bDriverVisemeSubmissionEnabled || !bOutputPlaybackStarted)
        {
            return;
        }

        const float PlaybackSec = GetCurrentOutputPlaybackSeconds();
        const float AuthoritativeSpeechStartSec = bActiveAlignedVisemeTrackBuilt ? ActiveAlignedVisemeTrack.SpeechStartSeconds : 0.0f;

        // Final submit-path hard gate. The shared aligned track is the only timing
        // authority; LineCoach does not run its own speech-onset clock.
        if (bActiveAlignedVisemeTrackBuilt && ActiveAlignedVisemeTrack.Events.Num() > 0 && PlaybackSec < AuthoritativeSpeechStartSec)
        {
            LastSubmittedVisemeSamples.Reset();
            TMap<FName, float> EmptyDriverWeights;
            FaceDriver->SubmitVisemePoseWeights(EmptyDriverWeights);
            AppendLipsyncDebugSubmittedPosesCSV(PlaybackSec, EmptyDriverWeights, FaceDriver->GetDebugActivePoseWeights());
            return;
        }

        // Authoritative aligned-track submission must use the same playback clock
        // as submitted_poses.csv and must not depend on a cached/stale
        // LastResolvedDriverVisemeWeights value.  The cache is useful for the
        // display smoothing path, but it can be one tick out of date or empty
        // during streaming rebuilds.  Sampling the committed aligned track here
        // prevents aligned events such as bodega MBP / today AY from existing in
        // aligned_track.csv but disappearing before FaceDriver submission.
        const bool bHasAuthoritativeAlignedTrack = bActiveAlignedVisemeTrackBuilt && ActiveAlignedVisemeTrack.Events.Num() > 0;
        const TMap<FName, float> AuthoritativeDriverWeights = bHasAuthoritativeAlignedTrack
            ? SampleDirectVisemePoseWeights(PlaybackSec)
            : TMap<FName, float>();

        TMap<FName, float> SubmittedDriverWeights;
        if (bHasAuthoritativeAlignedTrack)
        {
            // Submit only exact planned/aligned PoseIDs. If the sample is empty
            // because playback is before speech start or after the final event,
            // submit an empty map rather than falling back to canonical buckets;
            // fallback buckets can create pre-speech pops and can hide exact
            // visemes by replacing them with broad surrogate families.
            for (const TPair<FName, float>& Pair : AuthoritativeDriverWeights)
            {
                SubmittedDriverWeights.Add(Pair.Key, FMath::Clamp(Pair.Value, 0.0f, 1.0f));
            }
        }
        else
        {
            // Text-planned runtime has no canonical-bucket fallback.  If there is
            // no committed aligned track, submit an empty map.  This intentionally
            // removes the old experimental path that could submit 08_Ah/03_Ee/
            // 11_Oo/12_Ww-Oo-/20_FV from smoothed audio buckets and mask missing
            // exact PoseIDs.
        }

        // Layer 3 -> Layer 4 boundary: submit abstract viseme weights only.
        // FaceDriver owns all MetaHuman/rig-control translation.
        FaceDriver->SubmitVisemePoseWeights(SubmittedDriverWeights);
        AppendLipsyncDebugSubmittedPosesCSV(PlaybackSec, SubmittedDriverWeights, FaceDriver->GetDebugActivePoseWeights());
    }
}

FOffgridAILipsyncPoseRuntimeState UOffgridAILineCoach::GetPoseRuntimeStateFromMap(const TMap<FName, float>& PoseMap) const
{
    FOffgridAILipsyncPoseRuntimeState State;
    State.Closed = Clamp01(PoseMap.FindRef(TEXT("MBP")));
    State.Open = Clamp01(PoseMap.FindRef(TEXT("AAA")));
    State.Wide = Clamp01(PoseMap.FindRef(TEXT("EEE")));
    State.Round = Clamp01(PoseMap.FindRef(TEXT("OOO")));
    State.Funnel = Clamp01(PoseMap.FindRef(TEXT("WUH")));
    State.Teeth = Clamp01(PoseMap.FindRef(TEXT("FVS")));
    return State;
}

void UOffgridAILineCoach::ResetLipsyncRuntimeState()
{
    LipsyncAnalysisSamples.Reset();
    LipsyncAnalysisSampleBaseIndex = 0;
    LipsyncNextFrameStartSample = 0;
    LipsyncRMSPeak = 0.0001f;
    LipsyncPreviousRMSNorm = 0.0f;
    LipsyncObservedAudioDurationSeconds = 0.0f;
    LipsyncEnergyEnvelope = 0.0f;
    LipsyncEnergyPeak = 0.0001f;
    LipsyncRawRMS = 0.0f;
    LipsyncPoseState = FOffgridAILipsyncPoseRuntimeState();
    TargetDisplayedLipsyncPoseState = FOffgridAILipsyncPoseRuntimeState();
    CurrentDisplayedLipsyncPoseState = FOffgridAILipsyncPoseRuntimeState();
    bHasDisplayedLipsyncTarget = false;
    LastResolvedDriverVisemeWeights.Reset();
    LastSubmittedVisemeSamples.Reset();
    LastResolvedDriverVisemePlaybackSeconds = -1.0;
    bDriverVisemeSubmissionEnabled = false;
    LastCurveSampledPoseState = FOffgridAILipsyncPoseRuntimeState();
    LastLipsyncFrameApplyTimeSeconds = 0.0;
    LipsyncSpeechState = EOffgridAILipsyncSpeechState::Silence;
    bLipsyncClosureHoldActive = false;
    bLipsyncClosureLerpActive = false;
    LipsyncClosureHoldRemainingSeconds = 0.0f;
    LipsyncClosureLerpAlpha = 0.0f;
    LipsyncClosureStartState = FOffgridAILipsyncPoseRuntimeState();
    LipsyncHeldDominantPose = NAME_None;
    LipsyncHeldDominantWeight = 0.0f;
    LipsyncHeldDominantRemainingSeconds = 0.0f;
    LipsyncBilabialClosureRemainingSeconds = 0.0f;
    LipsyncBilabialClosureStrength = 0.0f;
    ActiveAlignedVisemeTrack = FOffgridAIAlignedVisemeTrack();
    bActiveAlignedVisemeTrackBuilt = false;
    bSharedLipsyncRuntimeFinalized = false;
    bSharedLipsyncOfflineEvidenceRetimingApplied = false;
    SharedLipsyncPreRetimingEventCount = 0;
    SharedLipsyncPostRetimingEventCount = 0;
    AU38ResetCounters(
        bAU38StreamingGroupReflowApplied,
        AU38StreamingGroupReflowGroupCount,
        AU38StreamingGroupReflowAffineGroupCount,
        AU38StreamingGroupReflowSingleAnchorGroupCount,
        AU38StreamingGroupReflowForwardShiftGroupCount,
        AU38StreamingGroupReflowAppliedEventCount,
        AU38StreamingGroupReflowAnchorCount,
        AU38StreamingGroupReflowMeanAbsEventDeltaMs,
        AU38StreamingGroupReflowMaxAbsEventDeltaMs);
    if (LipsyncRuntimeSession)
    {
        LipsyncRuntimeSession->Reset();
    }
    bStreamingPlaybackAudioBufferMapValid = false;
    StreamingPlaybackAudioBufferStartSec = 0.0f;
}

float UOffgridAILineCoach::GetCurrentOutputPlaybackSeconds() const
{
    if (!bOutputPlaybackStarted)
    {
        return 0.0f;
    }

    // v56 timing-only fix:
    // The procedural queue byte counter can jump far ahead of audible playback
    // on the game thread because SubmittedBytes - AvailableBytes measures queue
    // transfer/drain state, not the exact speaker-time sample currently being
    // heard. In v55 this made some lines start lipsync at playback_sec ~=0.6 on
    // the first tick, skipping the whole first word. For lipsync inception, the
    // stable clock is the monotonic wall clock from the actual AudioComponent
    // Play() call. Keep preroll and text-onset detection unchanged; only replace
    // the jittery/drain-derived sample clock with a continuous audible-playback
    // clock.
    if (OutputPlaybackStartTimeSeconds > 0.0)
    {
        return static_cast<float>(FMath::Max(FPlatformTime::Seconds() - OutputPlaybackStartTimeSeconds, 0.0));
    }

    return 0.0f;
}

float UOffgridAILineCoach::GetLipsyncSettingFloat(float UOffgridAILipsyncSettingsDataAsset::*Member, float DefaultValue) const
{
    return LipsyncSettingsAsset ? (LipsyncSettingsAsset.Get()->*Member) : DefaultValue;
}

int32 UOffgridAILineCoach::GetLipsyncSettingInt(int32 UOffgridAILipsyncSettingsDataAsset::*Member, int32 DefaultValue) const
{
    return LipsyncSettingsAsset ? (LipsyncSettingsAsset.Get()->*Member) : DefaultValue;
}

bool UOffgridAILineCoach::IsLipsyncDebugLoggingEnabled() const
{
    return LipsyncSettingsAsset && LipsyncSettingsAsset->bEnableLipsyncDebugLogging;
}

FString UOffgridAILineCoach::GetLipsyncDebugLogPath() const
{
    return GOffgridAILipsyncDebugSessionPath;
}

FString UOffgridAILineCoach::GetLipsyncDebugLineDirectory() const
{
    return LipsyncDebugLineDirectory;
}

void UOffgridAILineCoach::ResetLipsyncDebugLog()
{
    bLipsyncDebugFileInitialized = false;
    LipsyncDebugLineDirectory.Reset();

    if (!IsLipsyncDebugLoggingEnabled())
    {
        return;
    }

    const FDateTime Now = FDateTime::Now();
    const FString Timestamp = Now.ToString(TEXT("%Y%m%d_%H%M%S")) + FString::Printf(TEXT("_%03d"), Now.GetMillisecond());
    const FString LineToken = SanitizeDebugFilenameToken(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None")));
    const FString NPCToken = SanitizeDebugFilenameToken(NPCID.ToString());
    LipsyncDebugLineDirectory = FPaths::Combine(
        FPaths::ProjectLogDir(),
        TEXT("OffgridAI"),
        TEXT("LipsyncDebug"),
        FString::Printf(TEXT("%s_%s_%s"), *Timestamp, *NPCToken, *LineToken));

    IFileManager::Get().MakeDirectory(*LipsyncDebugLineDirectory, true);

    GOffgridAILipsyncDebugSessionPath = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timeline.csv"));

    const FString Header = TEXT("RealTimeSec,NPCID,LineID,Stage,PlaybackSec,FrameTimeSec,PCMSpeechActivity,AuthoritativeSpeechStartSec,AuthoritativeSpeechEndSec,RMSNorm,PrevRMSNorm,DeltaRMS,Voiced,Centroid,Low,Mid,High,Transient,RawClosed,RawOpen,RawWide,RawRound,RawFunnel,RawTeeth,SmoothedClosed,SmoothedOpen,SmoothedWide,SmoothedRound,SmoothedFunnel,SmoothedTeeth,FinalClosed,FinalOpen,FinalWide,FinalRound,FinalFunnel,FinalTeeth,AudioSampleRate,AudioNumSamples,AudioDurationSec,AudioPeakAbs,AudioRMS,AudioLeadingSilenceMs,AudioTrailingSilenceMs,AudioWavPath,DriverClosedFamily,DriverOpenFamily,DriverWideFamily,DriverRoundFamily,DriverFunnelFamily,DriverTeethFamily,DriverTongueFamily,DriverDominantPose,DriverDominantWeight,FaceJawOpen,FaceMouthCornerPull,FaceMouthCornerDepress,FaceMouthStretch,FaceLipPress,FaceLipPurse,FaceLipFunnel,FaceTongue,SubmittedPoseWeights,HybridSpeechEnergy,HybridAudioJawOpen,HybridAudioLipRound,HybridMouthWide,HybridLipClosed,HybridTeethEnergy,HybridTextRoundBias,HybridTextWideBias,HybridTextOpenBias,HybridAccentOverlayWeight,HybridFinalJawOpen,HybridFinalLipRound,HybridFinalMouthWide,HybridFinalLipClosed,HybridFinalTeethShow,HybridFinalTongue\n");
    if (FFileHelper::SaveStringToFile(Header, *GOffgridAILipsyncDebugSessionPath))
    {
        bGOffgridAILipsyncDebugSessionInitialized = true;
        bLipsyncDebugFileInitialized = true;
        WriteLipsyncDebugLineMetadata();
        WriteLipsyncDebugPlannedEventsCSV();
        WriteLipsyncDebugDurationScalingDiagnosticsCSV();
        WriteLipsyncDebugMotionQualityCSV();
        UE_LOG(LogOffgridAI, Log, TEXT("LipSyncDebug enabled dir=%s timeline=%s"), *LipsyncDebugLineDirectory, *GOffgridAILipsyncDebugSessionPath);
    }
    else
    {
        bLipsyncDebugFileInitialized = false;
        UE_LOG(LogOffgridAI, Warning, TEXT("LipSyncDebug failed to create timeline CSV path=%s"), *GOffgridAILipsyncDebugSessionPath);
    }
}

void UOffgridAILineCoach::ResetLipsyncDebugInputAudio()
{
    LipsyncDebugInputPCM16.Reset();
    LipsyncDebugInputSampleRate = ActiveSampleRate;
    bLipsyncDebugInputAudioWritten = false;
}

void UOffgridAILineCoach::WriteLipsyncDebugLineMetadata() const
{
    if (!bLipsyncDebugFileInitialized || LipsyncDebugLineDirectory.IsEmpty())
    {
        return;
    }

    const FString NPCIDString = NPCID.ToString();
    const FString LineIDString = bHasActiveLineRequest
        ? ActiveLineRequest.LineID.ToString()
        : FString(TEXT("None"));
    const FString ConversationIDString = bHasActiveLineRequest
        ? ActiveLineRequest.ConversationID.ToString(EGuidFormats::DigitsWithHyphens)
        : FString(TEXT("None"));
    const FString VoiceIDString = bHasActiveLineRequest
        ? ActiveLineRequest.VoiceID.ToString()
        : VoiceID.ToString();
    const FString EmotionString = bHasActiveLineRequest
        ? ActiveLineRequest.Emotion.ToString()
        : FString(TEXT("None"));
    const FString DialogueString = bHasActiveLineRequest
        ? ActiveLineRequest.Dialogue.ToString()
        : FString();

    FString Metadata;
    Metadata.Reserve(2048);

    Metadata.Appendf(TEXT("NPCID=%s\n"), *NPCIDString);
    Metadata.Appendf(TEXT("LineID=%s\n"), *LineIDString);
    Metadata.Appendf(TEXT("ConversationID=%s\n"), *ConversationIDString);
    Metadata.Appendf(TEXT("VoiceID=%s\n"), *VoiceIDString);
    Metadata.Appendf(TEXT("Emotion=%s\n"), *EmotionString);
    Metadata.Appendf(TEXT("Dialogue=%s\n"), *DialogueString);
    Metadata.Appendf(TEXT("ActiveSampleRate=%d\n"), ActiveSampleRate);
    Metadata.Appendf(TEXT("ActiveNumChannels=%d\n"), ActiveNumChannels);
    Metadata.Appendf(TEXT("LipsyncIntegrationVersion=AU39_shared_runtime_reflow_disabled_topology_probe\n"));
    Metadata.Appendf(TEXT("SharedLipsyncRuntimeSession=1\n"));
    Metadata.Appendf(TEXT("SharedLipsyncRuntimeFinalized=%d\n"), bSharedLipsyncRuntimeFinalized ? 1 : 0);
    Metadata.Appendf(TEXT("SharedLipsyncOfflineEvidenceRetimingApplied=%d\n"), bSharedLipsyncOfflineEvidenceRetimingApplied ? 1 : 0);
    Metadata.Appendf(TEXT("SharedLipsyncPreRetimingEventCount=%d\n"), SharedLipsyncPreRetimingEventCount);
    Metadata.Appendf(TEXT("SharedLipsyncPostRetimingEventCount=%d\n"), SharedLipsyncPostRetimingEventCount);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowApplied=%d\n"), bAU38StreamingGroupReflowApplied ? 1 : 0);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowGroupCount=%d\n"), AU38StreamingGroupReflowGroupCount);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowAffineGroupCount=%d\n"), AU38StreamingGroupReflowAffineGroupCount);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowSingleAnchorGroupCount=%d\n"), AU38StreamingGroupReflowSingleAnchorGroupCount);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowForwardShiftGroupCount=%d\n"), AU38StreamingGroupReflowForwardShiftGroupCount);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowAnchorCount=%d\n"), AU38StreamingGroupReflowAnchorCount);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowAppliedEventCount=%d\n"), AU38StreamingGroupReflowAppliedEventCount);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowMeanAbsEventDeltaMs=%.3f\n"), AU38StreamingGroupReflowMeanAbsEventDeltaMs);
    Metadata.Appendf(TEXT("AU38StreamingGroupReflowMaxAbsEventDeltaMs=%.3f\n"), AU38StreamingGroupReflowMaxAbsEventDeltaMs);
    Metadata.Appendf(TEXT("AU38bGuardrails=0\n"));
    Metadata.Appendf(TEXT("AU39RuntimeReflowDisabled=1\n"));
    Metadata.Appendf(TEXT("AuthoritativeTrackSource=SharedRuntimeCommittedTrack\n"));
    Metadata.Appendf(TEXT("AuthoritativeSpeechStartSeconds=%.6f\n"), ActiveAlignedVisemeTrack.SpeechStartSeconds);
    Metadata.Appendf(TEXT("AuthoritativeSpeechEndSeconds=%.6f\n"), ActiveAlignedVisemeTrack.SpeechEndSeconds);
    Metadata.Appendf(TEXT("AuthoritativeTrackEventCount=%d\n"), ActiveAlignedVisemeTrack.Events.Num());
    Metadata.Appendf(TEXT("V56TimingClock=wall_clock_from_audio_component_play_timing_only\n"));
    Metadata.Appendf(TEXT("CSV=%s\n"), *GOffgridAILipsyncDebugSessionPath);
    Metadata.Appendf(TEXT("WAV=%s\n"), *GetLipsyncDebugInputWavPath());

    FFileHelper::SaveStringToFile(
        Metadata,
        *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("line_metadata.txt")));
}

void UOffgridAILineCoach::AppendLipsyncDebugInputSample(float MonoSample, int32 SampleRate)
{
    if (!bLipsyncDebugFileInitialized)
    {
        return;
    }

    if (LipsyncDebugInputSampleRate <= 0)
    {
        LipsyncDebugInputSampleRate = SampleRate;
    }

    const int32 Scaled = FMath::RoundToInt(FMath::Clamp(MonoSample, -1.0f, 1.0f) * 32767.0f);
    LipsyncDebugInputPCM16.Add(static_cast<int16>(FMath::Clamp(Scaled, -32768, 32767)));
}

FString UOffgridAILineCoach::GetLipsyncDebugInputWavPath() const
{
    const FString LineToken = SanitizeDebugFilenameToken(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None")));
    const FString NPCToken = SanitizeDebugFilenameToken(NPCID.ToString());
    if (!LipsyncDebugLineDirectory.IsEmpty())
    {
        return FPaths::Combine(LipsyncDebugLineDirectory, FString::Printf(TEXT("tts_line_%s_%s.wav"), *NPCToken, *LineToken));
    }
    return FPaths::Combine(
        FPaths::ProjectLogDir(),
        FString::Printf(TEXT("OffgridAI_LipSyncInput_%s_%s.wav"), *NPCToken, *LineToken));
}

void UOffgridAILineCoach::WriteLipsyncDebugInputAudio()
{
    if (bLipsyncDebugInputAudioWritten || !bLipsyncDebugFileInitialized || LipsyncDebugInputPCM16.Num() <= 0 || LipsyncDebugInputSampleRate <= 0)
    {
        return;
    }

    bLipsyncDebugInputAudioWritten = true;

    const FString WavPath = GetLipsyncDebugInputWavPath();
    const bool bWroteWav = WritePCM16MonoWav(WavPath, LipsyncDebugInputPCM16, LipsyncDebugInputSampleRate);

    double SumSquares = 0.0;
    float PeakAbs = 0.0f;
    constexpr float SilenceThreshold = 0.003f;
    int32 FirstAudible = INDEX_NONE;
    int32 LastAudible = INDEX_NONE;

    for (int32 Index = 0; Index < LipsyncDebugInputPCM16.Num(); ++Index)
    {
        const float Sample = static_cast<float>(LipsyncDebugInputPCM16[Index]) / 32768.0f;
        const float AbsSample = FMath::Abs(Sample);
        PeakAbs = FMath::Max(PeakAbs, AbsSample);
        SumSquares += static_cast<double>(Sample * Sample);

        if (AbsSample >= SilenceThreshold)
        {
            if (FirstAudible == INDEX_NONE)
            {
                FirstAudible = Index;
            }
            LastAudible = Index;
        }
    }

    const int32 NumSamples = LipsyncDebugInputPCM16.Num();
    const float DurationSec = static_cast<float>(NumSamples) / static_cast<float>(LipsyncDebugInputSampleRate);
    const float RMS = NumSamples > 0 ? FMath::Sqrt(static_cast<float>(SumSquares / static_cast<double>(NumSamples))) : 0.0f;
    const float LeadingSilenceMs = FirstAudible == INDEX_NONE ? DurationSec * 1000.0f : (static_cast<float>(FirstAudible) / static_cast<float>(LipsyncDebugInputSampleRate)) * 1000.0f;
    const float TrailingSilenceMs = LastAudible == INDEX_NONE ? DurationSec * 1000.0f : (static_cast<float>(NumSamples - 1 - LastAudible) / static_cast<float>(LipsyncDebugInputSampleRate)) * 1000.0f;

    UE_LOG(LogOffgridAI, Log, TEXT("LipSyncDebug input wav %s path=%s samples=%d sample_rate=%d duration=%.3fs peak=%.4f rms=%.4f leading_silence=%.1fms trailing_silence=%.1fms"),
        bWroteWav ? TEXT("written") : TEXT("FAILED"),
        *WavPath,
        NumSamples,
        LipsyncDebugInputSampleRate,
        DurationSec,
        PeakAbs,
        RMS,
        LeadingSilenceMs,
        TrailingSilenceMs);

    AppendLipsyncDebugAudioSummaryCSV(WavPath, LipsyncDebugInputSampleRate, NumSamples, DurationSec, PeakAbs, RMS, LeadingSilenceMs, TrailingSilenceMs);
}

void UOffgridAILineCoach::AppendLipsyncDebugAudioSummaryCSV(const FString& WavPath, int32 SampleRate, int32 NumSamples, float DurationSec, float PeakAbs, float RMS, float LeadingSilenceMs, float TrailingSilenceMs) const
{
    if (!bLipsyncDebugFileInitialized)
    {
        return;
    }

    // Keep this row exactly aligned with the debug CSV header.
    // Columns 7-33 are lipsync feature/pose columns and intentionally blank for
    // AUDIO_SUMMARY rows. Driver/face columns are also blank because this row is
    // a line-level audio summary rather than a pose sample.
    TArray<FString> Columns;
    Columns.Reserve(62);
    Columns.Add(FString::Printf(TEXT("%.6f"), FPlatformTime::Seconds()));
    Columns.Add(NPCID.ToString());
    Columns.Add(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None")));
    Columns.Add(TEXT("AUDIO_SUMMARY"));
    Columns.Add(FString::Printf(TEXT("%.6f"), GetCurrentOutputPlaybackSeconds()));
    Columns.Add(TEXT("0.000000"));

    while (Columns.Num() < 33)
    {
        Columns.Add(TEXT(""));
    }

    Columns.Add(FString::Printf(TEXT("%d"), SampleRate));
    Columns.Add(FString::Printf(TEXT("%d"), NumSamples));
    Columns.Add(FString::Printf(TEXT("%.6f"), DurationSec));
    Columns.Add(FString::Printf(TEXT("%.6f"), PeakAbs));
    Columns.Add(FString::Printf(TEXT("%.6f"), RMS));
    Columns.Add(FString::Printf(TEXT("%.3f"), LeadingSilenceMs));
    Columns.Add(FString::Printf(TEXT("%.3f"), TrailingSilenceMs));
    Columns.Add(EscapeDebugCSVString(WavPath));

    while (Columns.Num() < 62)
    {
        Columns.Add(TEXT(""));
    }

    FString Row = FString::Join(Columns, TEXT(","));
    Row.Append(TEXT("\n"));
    FFileHelper::SaveStringToFile(Row, *GOffgridAILipsyncDebugSessionPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}



void UOffgridAILineCoach::WriteLipsyncDebugTimingCoverageDiagnosticsCSV() const
{
    if (!bLipsyncDebugFileInitialized || LipsyncDebugLineDirectory.IsEmpty())
    {
        return;
    }

    const float PrerollSec = FMath::Max(0.025f, GetInitialPrerollSeconds());
    TArray<FOffgridAIStreamingSpeechIsland> EmptyIslands;
    const TArray<FOffgridAIStreamingSpeechIsland>& SpeechIslands = LipsyncRuntimeSession ? LipsyncRuntimeSession->GetSpeechIslands() : EmptyIslands;
    const float ObservedAudioEndSec = LipsyncRuntimeSession ? LipsyncRuntimeSession->GetStreamTailDiagnosticRow().ObservedAudioBufferEndSec : GetCurrentOutputPlaybackSeconds();

    auto IslandEndSec = [ObservedAudioEndSec](const FOffgridAIStreamingSpeechIsland& Island) -> float
    {
        if (Island.bEnded && Island.AudioBufferEndSec > Island.AudioBufferStartSec)
        {
            return Island.AudioBufferEndSec;
        }
        if (Island.AudioBufferLastSpeechSec > Island.AudioBufferStartSec)
        {
            return Island.AudioBufferLastSpeechSec;
        }
        return FMath::Max(Island.AudioBufferStartSec, ObservedAudioEndSec);
    };

    auto IsInsideSpeech = [&SpeechIslands, &IslandEndSec](float TimeSec) -> bool
    {
        for (const FOffgridAIStreamingSpeechIsland& Island : SpeechIslands)
        {
            if (!Island.bStarted)
            {
                continue;
            }
            if (TimeSec >= Island.AudioBufferStartSec && TimeSec <= IslandEndSec(Island))
            {
                return true;
            }
        }
        return false;
    };

    auto DistanceToNearestSpeechMs = [&SpeechIslands, &IslandEndSec](float TimeSec) -> float
    {
        float BestSec = TNumericLimits<float>::Max();
        for (const FOffgridAIStreamingSpeechIsland& Island : SpeechIslands)
        {
            if (!Island.bStarted)
            {
                continue;
            }
            const float StartSec = Island.AudioBufferStartSec;
            const float EndSec = IslandEndSec(Island);
            float DistanceSec = 0.0f;
            if (TimeSec < StartSec)
            {
                DistanceSec = StartSec - TimeSec;
            }
            else if (TimeSec > EndSec)
            {
                DistanceSec = TimeSec - EndSec;
            }
            BestSec = FMath::Min(BestSec, DistanceSec);
        }
        return BestSec == TNumericLimits<float>::Max() ? 0.0f : BestSec * 1000.0f;
    };

    auto IntervalOverlapSec = [](float A0, float A1, float B0, float B1) -> float
    {
        return FMath::Max(0.0f, FMath::Min(A1, B1) - FMath::Max(A0, B0));
    };

    auto EventPauseOverlapSec = [&SpeechIslands, &IslandEndSec, &IntervalOverlapSec](const FOffgridAIAlignedVisemeEvent& Event) -> float
    {
        const float EventStart = FMath::Max(0.0f, Event.RenderStartSeconds);
        const float EventEnd = FMath::Max(EventStart, Event.RenderEndSeconds);
        float SpeechOverlap = 0.0f;
        for (const FOffgridAIStreamingSpeechIsland& Island : SpeechIslands)
        {
            if (!Island.bStarted)
            {
                continue;
            }
            SpeechOverlap += IntervalOverlapSec(EventStart, EventEnd, Island.AudioBufferStartSec, IslandEndSec(Island));
        }
        return FMath::Max(0.0f, EventEnd - EventStart - SpeechOverlap);
    };

    float ObservedStartSec = 0.0f;
    float ObservedEndSec = 0.0f;
    bool bHasSpeech = false;
    for (const FOffgridAIStreamingSpeechIsland& Island : SpeechIslands)
    {
        if (!Island.bStarted)
        {
            continue;
        }
        const float StartSec = Island.AudioBufferStartSec;
        const float EndSec = IslandEndSec(Island);
        if (EndSec <= StartSec)
        {
            continue;
        }
        if (!bHasSpeech)
        {
            ObservedStartSec = StartSec;
            ObservedEndSec = EndSec;
            bHasSpeech = true;
        }
        else
        {
            ObservedStartSec = FMath::Min(ObservedStartSec, StartSec);
            ObservedEndSec = FMath::Max(ObservedEndSec, EndSec);
        }
    }

    float FirstCenterSec = 0.0f;
    float LastCenterSec = 0.0f;
    bool bHasEvent = false;
    int32 TailFlushCount = 0;
    float PauseOverlapSec = 0.0f;
    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        if (!bHasEvent)
        {
            FirstCenterSec = Event.FinalRenderCenterSeconds;
            LastCenterSec = Event.FinalRenderCenterSeconds;
            bHasEvent = true;
        }
        else
        {
            FirstCenterSec = FMath::Min(FirstCenterSec, Event.FinalRenderCenterSeconds);
            LastCenterSec = FMath::Max(LastCenterSec, Event.FinalRenderCenterSeconds);
        }
        if (Event.CommitReason == FName(TEXT("end_of_stream_flush")) || Event.CommitReason.ToString().Contains(TEXT("tail_drain")))
        {
            ++TailFlushCount;
        }
        PauseOverlapSec += EventPauseOverlapSec(Event);
    }

    constexpr float FreshRadiusSec = 0.120f;
    constexpr float StepSec = 0.010f;
    float SpeechWithoutFreshVisemeSec = 0.0f;
    for (const FOffgridAIStreamingSpeechIsland& Island : SpeechIslands)
    {
        if (!Island.bStarted)
        {
            continue;
        }
        const float StartSec = Island.AudioBufferStartSec;
        const float EndSec = IslandEndSec(Island);
        for (float T = StartSec; T < EndSec; T += StepSec)
        {
            const float SampleT = T + StepSec * 0.5f;
            bool bFresh = false;
            for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
            {
                if (FMath::Abs(Event.FinalRenderCenterSeconds - SampleT) <= FreshRadiusSec)
                {
                    bFresh = true;
                    break;
                }
            }
            if (!bFresh)
            {
                SpeechWithoutFreshVisemeSec += FMath::Min(StepSec, EndSec - T);
            }
        }
    }

    FString SummaryCSV = TEXT("LineID,ObservedAudioStartSec,ObservedAudioEndSec,FirstCommittedCenterSec,LastCommittedCenterSec,AnimationStartsBeforeSpeechMs,AnimationStartsAfterSpeechMs,AudioTailUncoveredMs,AnimationPastAudioEndMs,SpeechActiveWithoutFreshVisemeMs,AnimationDuringPauseMs,TailFlushCount\n");
    TArray<FString> Summary;
    Summary.Add(EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))));
    Summary.Add(FString::Printf(TEXT("%.6f"), ObservedStartSec));
    Summary.Add(FString::Printf(TEXT("%.6f"), ObservedEndSec));
    Summary.Add(FString::Printf(TEXT("%.6f"), FirstCenterSec));
    Summary.Add(FString::Printf(TEXT("%.6f"), LastCenterSec));
    Summary.Add(FString::Printf(TEXT("%.3f"), bHasEvent ? FMath::Max(0.0f, ObservedStartSec - FirstCenterSec) * 1000.0f : 0.0f));
    Summary.Add(FString::Printf(TEXT("%.3f"), bHasEvent ? FMath::Max(0.0f, FirstCenterSec - ObservedStartSec) * 1000.0f : 0.0f));
    Summary.Add(FString::Printf(TEXT("%.3f"), bHasEvent ? FMath::Max(0.0f, ObservedEndSec - LastCenterSec) * 1000.0f : 0.0f));
    Summary.Add(FString::Printf(TEXT("%.3f"), bHasEvent ? FMath::Max(0.0f, LastCenterSec - ObservedEndSec) * 1000.0f : 0.0f));
    Summary.Add(FString::Printf(TEXT("%.3f"), SpeechWithoutFreshVisemeSec * 1000.0f));
    Summary.Add(FString::Printf(TEXT("%.3f"), PauseOverlapSec * 1000.0f));
    Summary.Add(FString::FromInt(TailFlushCount));
    SummaryCSV += FString::Join(Summary, TEXT(",")) + TEXT("\n");
    FFileHelper::SaveStringToFile(SummaryCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_coverage_summary.csv")));

    FString PauseCSV = TEXT("LineID,EventIndex,PoseID,SourceWord,CenterSec,RenderStartSec,RenderEndSec,InsideDetectedPause,DistanceToNearestSpeechMs,PauseOverlapMs,PauseOverlapSeverity\n");
    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        const bool bInsideSpeech = IsInsideSpeech(Event.FinalRenderCenterSeconds);
        const float DistanceMs = bInsideSpeech ? 0.0f : DistanceToNearestSpeechMs(Event.FinalRenderCenterSeconds);
        const float PauseOverlapMs = EventPauseOverlapSec(Event) * 1000.0f;
        FString Severity = TEXT("none");
        if (!bInsideSpeech && DistanceMs > 120.0f)
        {
            Severity = TEXT("high");
        }
        else if (!bInsideSpeech && DistanceMs > 60.0f)
        {
            Severity = TEXT("medium");
        }
        else if (!bInsideSpeech || PauseOverlapMs > 30.0f)
        {
            Severity = TEXT("low");
        }
        TArray<FString> Columns;
        Columns.Add(EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))));
        Columns.Add(FString::FromInt(Event.EventIndex));
        Columns.Add(EscapeDebugCSVString(Event.PoseID.ToString()));
        Columns.Add(EscapeDebugCSVString(Event.SourceWord));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.FinalRenderCenterSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.RenderStartSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.RenderEndSeconds));
        Columns.Add(!bInsideSpeech ? TEXT("1") : TEXT("0"));
        Columns.Add(FString::Printf(TEXT("%.3f"), DistanceMs));
        Columns.Add(FString::Printf(TEXT("%.3f"), PauseOverlapMs));
        Columns.Add(Severity);
        PauseCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    }
    FFileHelper::SaveStringToFile(PauseCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_pause_overlap.csv")));

    FString GapsCSV = TEXT("LineID,SpeechGapStartSec,SpeechGapEndSec,GapDurationMs,SpeechIslandIndex,PreviousViseme,NextViseme\n");
    constexpr float MinGapSec = 0.080f;
    for (int32 IslandIndex = 0; IslandIndex < SpeechIslands.Num(); ++IslandIndex)
    {
        const FOffgridAIStreamingSpeechIsland& Island = SpeechIslands[IslandIndex];
        if (!Island.bStarted)
        {
            continue;
        }
        const float SpeechStart = Island.AudioBufferStartSec;
        const float SpeechEnd = IslandEndSec(Island);
        float Cursor = SpeechStart;
        for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
        {
            const float CoverStart = FMath::Max(SpeechStart, Event.FinalRenderCenterSeconds - FreshRadiusSec);
            const float CoverEnd = FMath::Min(SpeechEnd, Event.FinalRenderCenterSeconds + FreshRadiusSec);
            if (CoverEnd <= SpeechStart || CoverStart >= SpeechEnd)
            {
                continue;
            }
            if (CoverStart - Cursor >= MinGapSec)
            {
                const FOffgridAIAlignedVisemeEvent* Prev = nullptr;
                const FOffgridAIAlignedVisemeEvent* Next = nullptr;
                for (const FOffgridAIAlignedVisemeEvent& Candidate : ActiveAlignedVisemeTrack.Events)
                {
                    if (Candidate.FinalRenderCenterSeconds <= Cursor)
                    {
                        Prev = &Candidate;
                    }
                    if (!Next && Candidate.FinalRenderCenterSeconds >= CoverStart)
                    {
                        Next = &Candidate;
                    }
                }
                TArray<FString> Columns;
                Columns.Add(EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))));
                Columns.Add(FString::Printf(TEXT("%.6f"), Cursor));
                Columns.Add(FString::Printf(TEXT("%.6f"), CoverStart));
                Columns.Add(FString::Printf(TEXT("%.3f"), (CoverStart - Cursor) * 1000.0f));
                Columns.Add(FString::FromInt(IslandIndex));
                Columns.Add(EscapeDebugCSVString(Prev ? Prev->PoseID.ToString() : FString()));
                Columns.Add(EscapeDebugCSVString(Next ? Next->PoseID.ToString() : FString()));
                GapsCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
            }
            Cursor = FMath::Max(Cursor, CoverEnd);
        }
        if (SpeechEnd - Cursor >= MinGapSec)
        {
            const FOffgridAIAlignedVisemeEvent* Prev = nullptr;
            for (const FOffgridAIAlignedVisemeEvent& Candidate : ActiveAlignedVisemeTrack.Events)
            {
                if (Candidate.FinalRenderCenterSeconds <= Cursor)
                {
                    Prev = &Candidate;
                }
            }
            TArray<FString> Columns;
            Columns.Add(EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))));
            Columns.Add(FString::Printf(TEXT("%.6f"), Cursor));
            Columns.Add(FString::Printf(TEXT("%.6f"), SpeechEnd));
            Columns.Add(FString::Printf(TEXT("%.3f"), (SpeechEnd - Cursor) * 1000.0f));
            Columns.Add(FString::FromInt(IslandIndex));
            Columns.Add(EscapeDebugCSVString(Prev ? Prev->PoseID.ToString() : FString()));
            Columns.Add(TEXT(""));
            GapsCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
        }
    }
    FFileHelper::SaveStringToFile(GapsCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_speech_gaps.csv")));

    FString RegionsCSV = TEXT("LineID,RegionType,RegionIndex,TextRegionStartSec,TextRegionEndSec,AudioRegionStartSec,AudioRegionEndSec,FirstVisemeCenterSec,LastVisemeCenterSec,LeadingGapMs,TrailingGapMs,CoverageRatio,EventCount\n");
    auto AppendRegion = [&](const FString& RegionType, int32 RegionIndex, int32 FirstArrayIndex, int32 LastArrayIndex)
    {
        if (!ActiveAlignedVisemeTrack.Events.IsValidIndex(FirstArrayIndex) || !ActiveAlignedVisemeTrack.Events.IsValidIndex(LastArrayIndex) || LastArrayIndex < FirstArrayIndex)
        {
            return;
        }
        float TextStart = TNumericLimits<float>::Max();
        float TextEnd = 0.0f;
        float FirstViseme = TNumericLimits<float>::Max();
        float LastViseme = 0.0f;
        float AudioStart = TNumericLimits<float>::Max();
        float AudioEnd = 0.0f;
        bool bHasAudio = false;
        int32 Count = 0;
        for (int32 I = FirstArrayIndex; I <= LastArrayIndex; ++I)
        {
            const FOffgridAIAlignedVisemeEvent& Event = ActiveAlignedVisemeTrack.Events[I];
            ++Count;
            TextStart = FMath::Min(TextStart, Event.TextDiagnosticCenterSeconds);
            TextEnd = FMath::Max(TextEnd, Event.TextDiagnosticCenterSeconds);
            FirstViseme = FMath::Min(FirstViseme, Event.FinalRenderCenterSeconds);
            LastViseme = FMath::Max(LastViseme, Event.FinalRenderCenterSeconds);
            if (SpeechIslands.IsValidIndex(Event.AudioIslandIndex))
            {
                bHasAudio = true;
                AudioStart = FMath::Min(AudioStart, SpeechIslands[Event.AudioIslandIndex].AudioBufferStartSec);
                AudioEnd = FMath::Max(AudioEnd, IslandEndSec(SpeechIslands[Event.AudioIslandIndex]));
            }
        }
        if (!bHasAudio)
        {
            AudioStart = FirstViseme;
            AudioEnd = LastViseme;
        }
        const float LeadingGapMs = (FirstViseme - AudioStart) * 1000.0f;
        const float TrailingGapMs = (AudioEnd - LastViseme) * 1000.0f;
        const float CoverageRatio = (AudioEnd > AudioStart) ? FMath::Clamp((LastViseme - FirstViseme) / (AudioEnd - AudioStart), 0.0f, 10.0f) : 0.0f;
        TArray<FString> Columns;
        Columns.Add(EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))));
        Columns.Add(RegionType);
        Columns.Add(FString::FromInt(RegionIndex));
        Columns.Add(FString::Printf(TEXT("%.6f"), TextStart));
        Columns.Add(FString::Printf(TEXT("%.6f"), TextEnd));
        Columns.Add(FString::Printf(TEXT("%.6f"), AudioStart));
        Columns.Add(FString::Printf(TEXT("%.6f"), AudioEnd));
        Columns.Add(FString::Printf(TEXT("%.6f"), FirstViseme));
        Columns.Add(FString::Printf(TEXT("%.6f"), LastViseme));
        Columns.Add(FString::Printf(TEXT("%.3f"), LeadingGapMs));
        Columns.Add(FString::Printf(TEXT("%.3f"), TrailingGapMs));
        Columns.Add(FString::Printf(TEXT("%.6f"), CoverageRatio));
        Columns.Add(FString::FromInt(Count));
        RegionsCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    };

    int32 RegionStart = 0;
    int32 RegionId = 0;
    while (RegionStart < ActiveAlignedVisemeTrack.Events.Num())
    {
        const int32 CurrentTextIsland = ActiveAlignedVisemeTrack.Events[RegionStart].TextIslandIndex;
        int32 RegionEnd = RegionStart;
        while (RegionEnd + 1 < ActiveAlignedVisemeTrack.Events.Num() && ActiveAlignedVisemeTrack.Events[RegionEnd + 1].TextIslandIndex == CurrentTextIsland)
        {
            ++RegionEnd;
        }
        AppendRegion(TEXT("punctuation"), RegionId++, RegionStart, RegionEnd);
        RegionStart = RegionEnd + 1;
    }
    constexpr int32 SyntheticRegionEventCount = 8;
    for (int32 Start = 0, SyntheticIndex = 0; Start < ActiveAlignedVisemeTrack.Events.Num(); Start += SyntheticRegionEventCount, ++SyntheticIndex)
    {
        AppendRegion(TEXT("synthetic"), SyntheticIndex, Start, FMath::Min(Start + SyntheticRegionEventCount - 1, ActiveAlignedVisemeTrack.Events.Num() - 1));
    }
    FFileHelper::SaveStringToFile(RegionsCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_coverage_regions.csv")));

    auto IsN11LandmarkPose = [](FName PoseID) -> bool
    {
        return PoseID == FName(TEXT("22_MBP")) || PoseID == FName(TEXT("20_FV")) || PoseID == FName(TEXT("19_FV-Or-")) || PoseID == FName(TEXT("21_FV-Ee-"))
            || PoseID == FName(TEXT("12_Ww-Oo-")) || PoseID == FName(TEXT("16_Ww-Ew-")) || PoseID == FName(TEXT("11_Oo")) || PoseID == FName(TEXT("10_Or"))
            || PoseID == FName(TEXT("14_ChJjSh")) || PoseID == FName(TEXT("07_Aa")) || PoseID == FName(TEXT("08_Ah")) || PoseID == FName(TEXT("09_Oh"))
            || PoseID == FName(TEXT("03_Ee")) || PoseID == FName(TEXT("04_Ih")) || PoseID == FName(TEXT("05_Ay")) || PoseID == FName(TEXT("06_Eh"));
    };
    auto N11PlanStartSec = [&](int32 EventIndex) -> float
    {
        if (ActiveTextVisemePlan.Events.IsValidIndex(EventIndex))
        {
            return FMath::Clamp(ActiveTextVisemePlan.Events[EventIndex].StartNorm, 0.0f, 1.0f) * FMath::Max(LipsyncEstimatedTextDurationSeconds, 0.001f);
        }
        return 0.0f;
    };
    auto N11PlanEndSec = [&](int32 EventIndex) -> float
    {
        if (ActiveTextVisemePlan.Events.IsValidIndex(EventIndex))
        {
            const FOffgridAITextVisemeEvent& PlanEvent = ActiveTextVisemePlan.Events[EventIndex];
            return FMath::Clamp(PlanEvent.EndNorm, PlanEvent.StartNorm, 1.0f) * FMath::Max(LipsyncEstimatedTextDurationSeconds, 0.001f);
        }
        return 0.0f;
    };
    auto N12PlanCenterSec = [&](int32 EventIndex) -> float
    {
        if (ActiveTextVisemePlan.Events.IsValidIndex(EventIndex))
        {
            const FOffgridAITextVisemeEvent& PlanEvent = ActiveTextVisemePlan.Events[EventIndex];
            const float Start = FMath::Clamp(PlanEvent.StartNorm, 0.0f, 1.0f);
            const float End = FMath::Clamp(PlanEvent.EndNorm, PlanEvent.StartNorm, 1.0f);
            return ((Start + End) * 0.5f) * FMath::Max(LipsyncEstimatedTextDurationSeconds, 0.001f);
        }
        return 0.0f;
    };

    float ObservedActiveSec = 0.0f;
    float TotalPauseSec = 0.0f;
    float PrevIslandEnd = -1.0f;
    float IslandDurationSum = 0.0f;
    float IslandDurationSqSum = 0.0f;
    int32 StartedIslandCount = 0;
    for (const FOffgridAIStreamingSpeechIsland& Island : SpeechIslands)
    {
        if (!Island.bStarted)
        {
            continue;
        }
        const float StartSec = Island.AudioBufferStartSec;
        const float EndSec = IslandEndSec(Island);
        const float DurationSec = FMath::Max(0.0f, EndSec - StartSec);
        ObservedActiveSec += DurationSec;
        IslandDurationSum += DurationSec;
        IslandDurationSqSum += DurationSec * DurationSec;
        ++StartedIslandCount;
        if (PrevIslandEnd >= 0.0f)
        {
            TotalPauseSec += FMath::Max(0.0f, StartSec - PrevIslandEnd);
        }
        PrevIslandEnd = EndSec;
    }
    const float MeanIslandSec = StartedIslandCount > 0 ? IslandDurationSum / static_cast<float>(StartedIslandCount) : 0.0f;
    const float IslandVariance = StartedIslandCount > 0 ? FMath::Max(0.0f, IslandDurationSqSum / static_cast<float>(StartedIslandCount) - MeanIslandSec * MeanIslandSec) : 0.0f;
    int32 LandmarkEventCount = 0;
    int32 MatchedLandmarkCount = 0;
    int32 UsableLead50Count = 0;
    float LandmarkAbsDeltaMsSum = 0.0f;
    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        if (!IsN11LandmarkPose(Event.PoseID))
        {
            continue;
        }
        ++LandmarkEventCount;
        if (Event.bAudioNudgeCorrespondenceMatched || Event.bAudioNudgeAccepted)
        {
            ++MatchedLandmarkCount;
            LandmarkAbsDeltaMsSum += FMath::Abs((Event.AudioNudgeCandidateAudioTimeSeconds > 0.0f ? Event.AudioNudgeCandidateAudioTimeSeconds - Event.FinalRenderCenterSeconds : Event.AudioNudgeCandidateAppliedShiftSeconds) * 1000.0f);
            if ((Event.FinalRenderCenterSeconds + PrerollSec) - Event.AudioNudgeCandidateAvailableAtSeconds >= 0.050f)
            {
                ++UsableLead50Count;
            }
        }
    }

    FString PacingSummaryCSV = TEXT("LineID,PlannedActiveSec,ObservedActiveSec,ObservedToPlannedScale,SpeechIslandCount,TotalPauseSec,MeanSpeechIslandSec,SpeechIslandDurationCv,EventCount\n");
    PacingSummaryCSV += FString::Printf(TEXT("%s,%.6f,%.6f,%.6f,%d,%.6f,%.6f,%.6f,%d\n"),
        *EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))),
        FMath::Max(LipsyncEstimatedTextDurationSeconds, 0.001f),
        ObservedActiveSec,
        ObservedActiveSec / FMath::Max(LipsyncEstimatedTextDurationSeconds, 0.001f),
        StartedIslandCount,
        TotalPauseSec,
        MeanIslandSec,
        MeanIslandSec > 0.001f ? FMath::Sqrt(IslandVariance) / MeanIslandSec : 0.0f,
        ActiveAlignedVisemeTrack.Events.Num());
    FFileHelper::SaveStringToFile(PacingSummaryCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_pacing_summary.csv")));

    FString OccupancyCSV = TEXT("LineID,SpeechIslandIndex,AudioStartSec,AudioEndSec,AudioDurationSec,PrevPauseSec,NextPauseSec,EventCount,FirstEventIndex,LastEventIndex,PlanStartSec,PlanEndSec,PlanDurationSec,CommittedStartSec,CommittedEndSec,CommittedDurationSec,ObservedToPlanScale,CommittedToAudioScale\n");
    for (int32 IslandIndex = 0; IslandIndex < SpeechIslands.Num(); ++IslandIndex)
    {
        const FOffgridAIStreamingSpeechIsland& Island = SpeechIslands[IslandIndex];
        if (!Island.bStarted)
        {
            continue;
        }
        const float AudioStartSec = Island.AudioBufferStartSec;
        const float AudioEndSec = IslandEndSec(Island);
        const float AudioDurationSec = FMath::Max(0.0f, AudioEndSec - AudioStartSec);
        const float PrevPauseSec = (IslandIndex > 0 && SpeechIslands[IslandIndex - 1].bStarted) ? FMath::Max(0.0f, AudioStartSec - IslandEndSec(SpeechIslands[IslandIndex - 1])) : 0.0f;
        const float NextPauseSec = (IslandIndex + 1 < SpeechIslands.Num() && SpeechIslands[IslandIndex + 1].bStarted) ? FMath::Max(0.0f, SpeechIslands[IslandIndex + 1].AudioBufferStartSec - AudioEndSec) : 0.0f;
        int32 Count = 0;
        int32 FirstEvent = INDEX_NONE;
        int32 LastEvent = INDEX_NONE;
        float PlanStartSec = TNumericLimits<float>::Max();
        float PlanEndSec = 0.0f;
        float CommitStartSec = TNumericLimits<float>::Max();
        float CommitEndSec = 0.0f;
        for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
        {
            if (Event.AudioIslandIndex != IslandIndex)
            {
                continue;
            }
            ++Count;
            FirstEvent = FirstEvent == INDEX_NONE ? Event.EventIndex : FMath::Min(FirstEvent, Event.EventIndex);
            LastEvent = LastEvent == INDEX_NONE ? Event.EventIndex : FMath::Max(LastEvent, Event.EventIndex);
            PlanStartSec = FMath::Min(PlanStartSec, N11PlanStartSec(Event.EventIndex));
            PlanEndSec = FMath::Max(PlanEndSec, N11PlanEndSec(Event.EventIndex));
            CommitStartSec = FMath::Min(CommitStartSec, Event.FinalRenderCenterSeconds);
            CommitEndSec = FMath::Max(CommitEndSec, Event.FinalRenderCenterSeconds);
        }
        if (Count == 0)
        {
            PlanStartSec = PlanEndSec = CommitStartSec = CommitEndSec = 0.0f;
        }
        const float PlanDurationSec = FMath::Max(0.0f, PlanEndSec - PlanStartSec);
        const float CommitDurationSec = FMath::Max(0.0f, CommitEndSec - CommitStartSec);
        OccupancyCSV += FString::Printf(TEXT("%s,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"),
            *EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))), IslandIndex, AudioStartSec, AudioEndSec, AudioDurationSec, PrevPauseSec, NextPauseSec,
            Count, FirstEvent, LastEvent, PlanStartSec, PlanEndSec, PlanDurationSec, CommitStartSec, CommitEndSec, CommitDurationSec,
            PlanDurationSec > 0.001f ? AudioDurationSec / PlanDurationSec : 0.0f,
            AudioDurationSec > 0.001f ? CommitDurationSec / AudioDurationSec : 0.0f);
    }
    FFileHelper::SaveStringToFile(OccupancyCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_occupancy_modulation.csv")));

    struct FN12Region
    {
        FString RegionType;
        int32 RegionIndex = 0;
        int32 FirstTrackIndex = INDEX_NONE;
        int32 LastTrackIndex = INDEX_NONE;
        int32 FirstEventIndex = INDEX_NONE;
        int32 LastEventIndex = INDEX_NONE;
        int32 EventCount = 0;
        int32 AudioIslandCount = 0;
        float FirstPlanCenterSec = 0.0f;
        float LastPlanCenterSec = 0.0f;
        float PlanCenterSpanSec = 0.0f;
        float AudioStartSec = 0.0f;
        float AudioEndSec = 0.0f;
        float AudioDurationSec = 0.0f;
        float CurrentFirstCenterSec = 0.0f;
        float CurrentLastCenterSec = 0.0f;
        float CurrentCenterSpanSec = 0.0f;
        bool bReliable = false;
    };

    auto BuildN12Region = [&](const FString& RegionType, int32 RegionIndex, int32 FirstTrackIndex, int32 LastTrackIndex) -> FN12Region
    {
        FN12Region R;
        R.RegionType = RegionType;
        R.RegionIndex = RegionIndex;
        R.FirstTrackIndex = FirstTrackIndex;
        R.LastTrackIndex = LastTrackIndex;
        if (!ActiveAlignedVisemeTrack.Events.IsValidIndex(FirstTrackIndex) || !ActiveAlignedVisemeTrack.Events.IsValidIndex(LastTrackIndex) || LastTrackIndex < FirstTrackIndex)
        {
            return R;
        }
        R.FirstEventIndex = ActiveAlignedVisemeTrack.Events[FirstTrackIndex].EventIndex;
        R.LastEventIndex = ActiveAlignedVisemeTrack.Events[LastTrackIndex].EventIndex;
        R.EventCount = LastTrackIndex - FirstTrackIndex + 1;
        R.FirstPlanCenterSec = N12PlanCenterSec(R.FirstEventIndex);
        R.LastPlanCenterSec = N12PlanCenterSec(R.LastEventIndex);
        R.PlanCenterSpanSec = FMath::Max(0.0f, R.LastPlanCenterSec - R.FirstPlanCenterSec);
        R.CurrentFirstCenterSec = ActiveAlignedVisemeTrack.Events[FirstTrackIndex].FinalRenderCenterSeconds;
        R.CurrentLastCenterSec = ActiveAlignedVisemeTrack.Events[LastTrackIndex].FinalRenderCenterSeconds;
        R.CurrentCenterSpanSec = FMath::Max(0.0f, R.CurrentLastCenterSec - R.CurrentFirstCenterSec);
        float AudioStart = TNumericLimits<float>::Max();
        float AudioEnd = 0.0f;
        TSet<int32> AudioIslandIds;
        for (int32 I = FirstTrackIndex; I <= LastTrackIndex; ++I)
        {
            const FOffgridAIAlignedVisemeEvent& Event = ActiveAlignedVisemeTrack.Events[I];
            if (SpeechIslands.IsValidIndex(Event.AudioIslandIndex))
            {
                AudioIslandIds.Add(Event.AudioIslandIndex);
                AudioStart = FMath::Min(AudioStart, SpeechIslands[Event.AudioIslandIndex].AudioBufferStartSec);
                AudioEnd = FMath::Max(AudioEnd, IslandEndSec(SpeechIslands[Event.AudioIslandIndex]));
            }
        }
        R.AudioIslandCount = AudioIslandIds.Num();
        if (R.AudioIslandCount > 0)
        {
            R.AudioStartSec = AudioStart;
            R.AudioEndSec = AudioEnd;
        }
        else
        {
            R.AudioStartSec = R.CurrentFirstCenterSec;
            R.AudioEndSec = R.CurrentLastCenterSec;
        }
        R.AudioDurationSec = FMath::Max(0.0f, R.AudioEndSec - R.AudioStartSec);
        const float Scale = R.PlanCenterSpanSec > 0.001f ? R.AudioDurationSec / R.PlanCenterSpanSec : 0.0f;
        R.bReliable = R.EventCount >= 2 && R.PlanCenterSpanSec >= 0.120f && R.AudioDurationSec >= 0.120f && Scale >= 0.45f && Scale <= 2.25f && R.AudioIslandCount > 0;
        return R;
    };

    auto RegionPacedCenter = [&](const FN12Region& R, int32 EventIndex) -> float
    {
        if (!R.bReliable)
        {
            return 0.0f;
        }
        if (R.PlanCenterSpanSec <= 0.001f)
        {
            return (R.AudioStartSec + R.AudioEndSec) * 0.5f;
        }
        const float Alpha = FMath::Clamp((N12PlanCenterSec(EventIndex) - R.FirstPlanCenterSec) / R.PlanCenterSpanSec, 0.0f, 1.0f);
        return R.AudioStartSec + Alpha * R.AudioDurationSec;
    };

    TArray<FN12Region> N12Regions;
    int32 N12RegionStart = 0;
    int32 N12RegionId = 0;
    while (N12RegionStart < ActiveAlignedVisemeTrack.Events.Num())
    {
        const int32 CurrentTextIsland = ActiveAlignedVisemeTrack.Events[N12RegionStart].TextIslandIndex;
        int32 RegionEnd = N12RegionStart;
        while (RegionEnd + 1 < ActiveAlignedVisemeTrack.Events.Num() && ActiveAlignedVisemeTrack.Events[RegionEnd + 1].TextIslandIndex == CurrentTextIsland)
        {
            ++RegionEnd;
        }
        N12Regions.Add(BuildN12Region(TEXT("punctuation"), N12RegionId++, N12RegionStart, RegionEnd));
        N12RegionStart = RegionEnd + 1;
    }
    for (int32 Start = 0, SyntheticIndex = 0; Start < ActiveAlignedVisemeTrack.Events.Num(); Start += SyntheticRegionEventCount, ++SyntheticIndex)
    {
        N12Regions.Add(BuildN12Region(TEXT("synthetic8"), SyntheticIndex, Start, FMath::Min(Start + SyntheticRegionEventCount - 1, ActiveAlignedVisemeTrack.Events.Num() - 1)));
    }

    FString RegionSummaryCSV = TEXT("LineID,RegionType,RegionIndex,Reliable,FirstEventIndex,LastEventIndex,EventCount,AudioIslandCount,FirstPlanCenterSec,LastPlanCenterSec,PlanCenterSpanSec,AudioStartSec,AudioEndSec,AudioDurationSec,ObservedToPlanCenterScale,CurrentFirstCenterSec,CurrentLastCenterSec,CurrentCenterSpanSec,CurrentToAudioSpanScale,CurrentLeadingGapMs,CurrentTrailingGapMs,MeanAbsRegionRetimingMs,MaxAbsRegionRetimingMs,CurrentPauseCenterCount,RegionPacedPauseCenterCount,CurrentMeanDistanceToSpeechMs,RegionPacedMeanDistanceToSpeechMs\n");
    FString RegionEventsCSV = TEXT("LineID,RegionType,RegionIndex,Reliable,EventIndex,PoseID,SourceWord,PlanCenterSec,CurrentCenterSec,RegionPacedCenterSec,RegionRetimingDeltaMs,CurrentInsideSpeech,RegionPacedInsideSpeech,CurrentDistanceToSpeechMs,RegionPacedDistanceToSpeechMs\n");
    int32 ReliableRegionCount = 0;
    int32 ReliableEventCount = 0;
    int32 LineCurrentPauseCount = 0;
    int32 LineRegionPauseCount = 0;
    float LineCurrentDistanceMs = 0.0f;
    float LineRegionDistanceMs = 0.0f;
    float LineAbsRetimingMs = 0.0f;
    float LineMaxAbsRetimingMs = 0.0f;
    float ScaleSum = 0.0f;
    float ScaleSqSum = 0.0f;
    int32 ScaleCount = 0;

    for (const FN12Region& R : N12Regions)
    {
        float AbsRetimingMs = 0.0f;
        float MaxAbsRetimingMs = 0.0f;
        int32 CurrentPauseCount = 0;
        int32 RegionPauseCount = 0;
        float CurrentDistanceMs = 0.0f;
        float RegionDistanceMs = 0.0f;
        for (int32 I = R.FirstTrackIndex; I <= R.LastTrackIndex && ActiveAlignedVisemeTrack.Events.IsValidIndex(I); ++I)
        {
            const FOffgridAIAlignedVisemeEvent& Event = ActiveAlignedVisemeTrack.Events[I];
            const float RegionCenter = RegionPacedCenter(R, Event.EventIndex);
            const bool bCurrentInside = IsInsideSpeech(Event.FinalRenderCenterSeconds);
            const float CurrentDist = bCurrentInside ? 0.0f : DistanceToNearestSpeechMs(Event.FinalRenderCenterSeconds);
            const bool bRegionInside = R.bReliable ? IsInsideSpeech(RegionCenter) : false;
            const float RegionDist = (R.bReliable && !bRegionInside) ? DistanceToNearestSpeechMs(RegionCenter) : 0.0f;
            const float DeltaMs = R.bReliable ? (RegionCenter - Event.FinalRenderCenterSeconds) * 1000.0f : 0.0f;
            if (R.bReliable)
            {
                AbsRetimingMs += FMath::Abs(DeltaMs);
                MaxAbsRetimingMs = FMath::Max(MaxAbsRetimingMs, FMath::Abs(DeltaMs));
                RegionDistanceMs += RegionDist;
                if (!bRegionInside) { ++RegionPauseCount; }
            }
            CurrentDistanceMs += CurrentDist;
            if (!bCurrentInside) { ++CurrentPauseCount; }

            RegionEventsCSV += FString::Printf(TEXT("%s,%s,%d,%d,%d,%s,%s,%.6f,%.6f,%.6f,%.3f,%d,%d,%.3f,%.3f\n"),
                *EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))), *EscapeDebugCSVString(R.RegionType), R.RegionIndex, R.bReliable ? 1 : 0,
                Event.EventIndex, *EscapeDebugCSVString(Event.PoseID.ToString()), *EscapeDebugCSVString(Event.SourceWord), N12PlanCenterSec(Event.EventIndex), Event.FinalRenderCenterSeconds,
                R.bReliable ? RegionCenter : 0.0f, DeltaMs, bCurrentInside ? 1 : 0, (R.bReliable && bRegionInside) ? 1 : 0, CurrentDist, R.bReliable ? RegionDist : 0.0f);
        }
        const float Denom = R.EventCount > 0 ? static_cast<float>(R.EventCount) : 1.0f;
        RegionSummaryCSV += FString::Printf(TEXT("%s,%s,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.3f,%.3f\n"),
            *EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))), *EscapeDebugCSVString(R.RegionType), R.RegionIndex, R.bReliable ? 1 : 0,
            R.FirstEventIndex, R.LastEventIndex, R.EventCount, R.AudioIslandCount, R.FirstPlanCenterSec, R.LastPlanCenterSec, R.PlanCenterSpanSec,
            R.AudioStartSec, R.AudioEndSec, R.AudioDurationSec, R.PlanCenterSpanSec > 0.001f ? R.AudioDurationSec / R.PlanCenterSpanSec : 0.0f,
            R.CurrentFirstCenterSec, R.CurrentLastCenterSec, R.CurrentCenterSpanSec, R.AudioDurationSec > 0.001f ? R.CurrentCenterSpanSec / R.AudioDurationSec : 0.0f,
            (R.CurrentFirstCenterSec - R.AudioStartSec) * 1000.0f, (R.AudioEndSec - R.CurrentLastCenterSec) * 1000.0f,
            R.bReliable ? AbsRetimingMs / Denom : 0.0f, R.bReliable ? MaxAbsRetimingMs : 0.0f, CurrentPauseCount, R.bReliable ? RegionPauseCount : 0,
            CurrentDistanceMs / Denom, R.bReliable ? RegionDistanceMs / Denom : 0.0f);
        if (R.bReliable)
        {
            ++ReliableRegionCount;
            ReliableEventCount += R.EventCount;
            LineCurrentPauseCount += CurrentPauseCount;
            LineRegionPauseCount += RegionPauseCount;
            LineCurrentDistanceMs += CurrentDistanceMs;
            LineRegionDistanceMs += RegionDistanceMs;
            LineAbsRetimingMs += AbsRetimingMs;
            LineMaxAbsRetimingMs = FMath::Max(LineMaxAbsRetimingMs, MaxAbsRetimingMs);
            const float Scale = R.PlanCenterSpanSec > 0.001f ? R.AudioDurationSec / R.PlanCenterSpanSec : 0.0f;
            ScaleSum += Scale;
            ScaleSqSum += Scale * Scale;
            ++ScaleCount;
        }
    }
    FFileHelper::SaveStringToFile(RegionSummaryCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_region_duration_pacing_summary.csv")));
    FFileHelper::SaveStringToFile(RegionEventsCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_region_duration_pacing_events.csv")));
    const float EventDenom = ReliableEventCount > 0 ? static_cast<float>(ReliableEventCount) : 1.0f;
    const float ScaleMean = ScaleCount > 0 ? ScaleSum / static_cast<float>(ScaleCount) : 0.0f;
    const float ScaleVar = ScaleCount > 0 ? FMath::Max(0.0f, ScaleSqSum / static_cast<float>(ScaleCount) - ScaleMean * ScaleMean) : 0.0f;
    FString LineSummaryCSV = TEXT("LineID,RegionCount,ReliableRegionCount,ReliableEventCount,CurrentPauseCenterCount,RegionPacedPauseCenterCount,CurrentMeanDistanceToSpeechMs,RegionPacedMeanDistanceToSpeechMs,MeanAbsRegionRetimingMs,MaxAbsRegionRetimingMs,MeanRegionScale,RegionScaleCv\n");
    LineSummaryCSV += FString::Printf(TEXT("%s,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.6f,%.6f\n"),
        *EscapeDebugCSVString(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))), N12Regions.Num(), ReliableRegionCount, ReliableEventCount, LineCurrentPauseCount, LineRegionPauseCount,
        LineCurrentDistanceMs / EventDenom, LineRegionDistanceMs / EventDenom, LineAbsRetimingMs / EventDenom, LineMaxAbsRetimingMs, ScaleMean, ScaleMean > 0.001f ? FMath::Sqrt(ScaleVar) / ScaleMean : 0.0f);
    FFileHelper::SaveStringToFile(LineSummaryCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("timing_region_duration_pacing_line_summary.csv")));

}


void UOffgridAILineCoach::WriteLipsyncDebugPlannedEventsCSV() const
{
    if (!bLipsyncDebugFileInitialized || LipsyncDebugLineDirectory.IsEmpty())
    {
        return;
    }

    const FString Path = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("planned_events.csv"));
    FString CSV = TEXT("EventIndex,NPCID,LineID,SourceText,PoseID,TextViseme,Family,StartNorm,EndNorm,StartSec,EndSec,Strength,EnvelopeType\n");

    const float Duration = FMath::Max(LipsyncEstimatedTextDurationSeconds, 0.10f);
    for (int32 Index = 0; Index < ActiveTextVisemePlan.Events.Num(); ++Index)
    {
        const FOffgridAITextVisemeEvent& Event = ActiveTextVisemePlan.Events[Index];
        const float StartNorm = FMath::Clamp(Event.StartNorm, 0.0f, 1.0f);
        const float EndNorm = FMath::Clamp(Event.EndNorm, StartNorm, 1.0f);
        TArray<FString> Columns;
        Columns.Add(FString::Printf(TEXT("%d"), Index));
        Columns.Add(NPCID.ToString());
        Columns.Add(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None")));
        Columns.Add(EscapeDebugCSVString(Event.SourceText));
        Columns.Add(Event.PoseID.ToString());
        Columns.Add(FOffgridAITextVisemePlanner::ToDebugString(Event.Viseme));
        Columns.Add(DebugFamilyForPose(Event.PoseID));
        Columns.Add(FString::Printf(TEXT("%.6f"), StartNorm));
        Columns.Add(FString::Printf(TEXT("%.6f"), EndNorm));
        Columns.Add(FString::Printf(TEXT("%.6f"), StartNorm * Duration));
        Columns.Add(FString::Printf(TEXT("%.6f"), EndNorm * Duration));
        Columns.Add(FString::Printf(TEXT("%.6f"), Clamp01(Event.Strength)));
        Columns.Add(TextVisemeEnvelopeDebugName(Event.Viseme));
        CSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    }

    FFileHelper::SaveStringToFile(CSV, *Path);
}




void UOffgridAILineCoach::WriteLipsyncDebugDurationScalingDiagnosticsCSV() const
{
    if (!bLipsyncDebugFileInitialized || LipsyncDebugLineDirectory.IsEmpty())
    {
        return;
    }

    const FString EventPath = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("duration_scaling_diagnostics.csv"));
    const FString SummaryPath = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("duration_scaling_summary.csv"));

    TArray<FOffgridAIStreamingSpeechIsland> AudioIslands;
    if (LipsyncRuntimeSession)
    {
        AudioIslands = LipsyncRuntimeSession->GetSpeechDetector().GetIslands();
    }

    struct FDiagnosticRegion
    {
        int32 RegionIndex = INDEX_NONE;
        int32 FirstEventIndex = INDEX_NONE;
        int32 LastEventIndex = INDEX_NONE;
        float PlanStartSec = 0.0f;
        float PlanEndSec = 0.0f;
        float AudioStartSec = 0.0f;
        float AudioEndSec = 0.0f;
        bool bHasAudio = false;
    };

    const float PlannedActiveSec = FMath::Max(
        LipsyncEstimatedTextDurationSeconds > 0.0f ? LipsyncEstimatedTextDurationSeconds : ActiveTextVisemePlan.EstimatedDurationSeconds,
        0.001f);

    auto PlanEventStartSec = [&](int32 EventIndex, float FallbackCenter) -> float
    {
        if (ActiveTextVisemePlan.Events.IsValidIndex(EventIndex))
        {
            const FOffgridAITextVisemeEvent& PlanEvent = ActiveTextVisemePlan.Events[EventIndex];
            return FMath::Clamp(PlanEvent.StartNorm, 0.0f, 1.0f) * PlannedActiveSec;
        }
        return FallbackCenter;
    };
    auto PlanEventEndSec = [&](int32 EventIndex, float FallbackCenter) -> float
    {
        if (ActiveTextVisemePlan.Events.IsValidIndex(EventIndex))
        {
            const FOffgridAITextVisemeEvent& PlanEvent = ActiveTextVisemePlan.Events[EventIndex];
            return FMath::Clamp(PlanEvent.EndNorm, PlanEvent.StartNorm, 1.0f) * PlannedActiveSec;
        }
        return FallbackCenter;
    };
    auto IsSoftBoundaryWord = [](const FString& In) -> bool
    {
        const FString Word = In.ToLower().TrimStartAndEnd();
        return Word == TEXT("and") || Word == TEXT("but") || Word == TEXT("or") || Word == TEXT("then") || Word == TEXT("so")
            || Word == TEXT("because") || Word == TEXT("with") || Word == TEXT("for") || Word == TEXT("to") || Word == TEXT("from")
            || Word == TEXT("while") || Word == TEXT("after") || Word == TEXT("before") || Word == TEXT("would") || Word == TEXT("could")
            || Word == TEXT("can") || Word == TEXT("let");
    };
    auto EventAudioStart = [](const FOffgridAIAlignedVisemeEvent& Event) -> float
    {
        return Event.IslandAudioStartSeconds;
    };
    auto EventAudioEnd = [](const FOffgridAIAlignedVisemeEvent& Event) -> float
    {
        return Event.IslandAudioSpanSeconds > 0.0f
            ? Event.IslandAudioStartSeconds + Event.IslandAudioSpanSeconds
            : FMath::Max(Event.IslandAudioEndSeconds, Event.IslandAudioStartSeconds);
    };

    TArray<FDiagnosticRegion> SyntheticRegions;
    {
        const int32 MinEventsPerRegion = 5;
        const int32 TargetEventsPerRegion = 10;
        const int32 MaxEventsPerRegion = 12;
        const float MaxRegionPlanSec = 1.25f;

        int32 RegionStartArrayIndex = 0;
        while (RegionStartArrayIndex < ActiveAlignedVisemeTrack.Events.Num())
        {
            int32 RegionEndArrayIndex = RegionStartArrayIndex;
            int32 LastWordIndex = ActiveAlignedVisemeTrack.Events[RegionStartArrayIndex].WordIndex;
            for (int32 Index = RegionStartArrayIndex + 1; Index < ActiveAlignedVisemeTrack.Events.Num(); ++Index)
            {
                const int32 EventCount = Index - RegionStartArrayIndex + 1;
                const float RegionPlanSpan = PlanEventEndSec(ActiveAlignedVisemeTrack.Events[Index].EventIndex, ActiveAlignedVisemeTrack.Events[Index].TextDiagnosticCenterSeconds)
                    - PlanEventStartSec(ActiveAlignedVisemeTrack.Events[RegionStartArrayIndex].EventIndex, ActiveAlignedVisemeTrack.Events[RegionStartArrayIndex].TextDiagnosticCenterSeconds);
                const bool bWordAdvanced = ActiveAlignedVisemeTrack.Events[Index].WordIndex != LastWordIndex;
                const bool bSoftBoundary = bWordAdvanced && IsSoftBoundaryWord(ActiveAlignedVisemeTrack.Events[Index].SourceWord);
                const bool bHitTargetAtWordBoundary = EventCount >= TargetEventsPerRegion && bWordAdvanced;
                const bool bHitSoftBoundary = EventCount >= MinEventsPerRegion && bSoftBoundary;
                const bool bHitMaxCount = EventCount >= MaxEventsPerRegion;
                const bool bHitMaxDuration = EventCount >= MinEventsPerRegion && RegionPlanSpan >= MaxRegionPlanSec && bWordAdvanced;
                if (bHitSoftBoundary || bHitTargetAtWordBoundary || bHitMaxCount || bHitMaxDuration)
                {
                    break;
                }
                RegionEndArrayIndex = Index;
                LastWordIndex = ActiveAlignedVisemeTrack.Events[Index].WordIndex;
            }

            FDiagnosticRegion Region;
            Region.RegionIndex = SyntheticRegions.Num();
            Region.FirstEventIndex = ActiveAlignedVisemeTrack.Events[RegionStartArrayIndex].EventIndex;
            Region.LastEventIndex = ActiveAlignedVisemeTrack.Events[RegionEndArrayIndex].EventIndex;
            Region.PlanStartSec = PlanEventStartSec(ActiveAlignedVisemeTrack.Events[RegionStartArrayIndex].EventIndex, ActiveAlignedVisemeTrack.Events[RegionStartArrayIndex].TextDiagnosticCenterSeconds);
            Region.PlanEndSec = PlanEventEndSec(ActiveAlignedVisemeTrack.Events[RegionEndArrayIndex].EventIndex, ActiveAlignedVisemeTrack.Events[RegionEndArrayIndex].TextDiagnosticCenterSeconds);
            Region.AudioStartSec = TNumericLimits<float>::Max();
            Region.AudioEndSec = 0.0f;
            for (int32 Index = RegionStartArrayIndex; Index <= RegionEndArrayIndex; ++Index)
            {
                const FOffgridAIAlignedVisemeEvent& Event = ActiveAlignedVisemeTrack.Events[Index];
                if (Event.AudioIslandIndex != INDEX_NONE || Event.IslandAudioSpanSeconds > 0.0f)
                {
                    const float AudioStart = EventAudioStart(Event);
                    const float AudioEnd = EventAudioEnd(Event);
                    if (AudioEnd > AudioStart)
                    {
                        Region.AudioStartSec = FMath::Min(Region.AudioStartSec, AudioStart);
                        Region.AudioEndSec = FMath::Max(Region.AudioEndSec, AudioEnd);
                        Region.bHasAudio = true;
                    }
                }
            }
            if (!Region.bHasAudio)
            {
                Region.AudioStartSec = 0.0f;
                Region.AudioEndSec = 0.0f;
            }
            SyntheticRegions.Add(Region);
            RegionStartArrayIndex = RegionEndArrayIndex + 1;
        }
    }

    auto FindSyntheticRegionForEvent = [&](int32 EventIndex) -> const FDiagnosticRegion*
    {
        for (const FDiagnosticRegion& Region : SyntheticRegions)
        {
            if (EventIndex >= Region.FirstEventIndex && EventIndex <= Region.LastEventIndex)
            {
                return &Region;
            }
        }
        return nullptr;
    };

    float ObservedActiveSec = 0.0f;
    float FirstSpeechStartSec = 0.0f;
    bool bHasSpeech = false;
    for (const FOffgridAIStreamingSpeechIsland& Island : AudioIslands)
    {
        const float Start = Island.AudioBufferStartSec;
        const float End = FMath::Max(Island.AudioBufferLastSpeechSec, Island.AudioBufferEndSec);
        if (End > Start)
        {
            if (!bHasSpeech)
            {
                FirstSpeechStartSec = Start;
                bHasSpeech = true;
            }
            ObservedActiveSec += End - Start;
        }
    }

    const float GlobalScale = bHasSpeech ? (ObservedActiveSec / PlannedActiveSec) : 1.0f;

    int32 TailFlushCount = 0;
    int32 FallbackCount = 0;
    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        const FString Reason = Event.CommitReason.ToString();
        if (Reason.Contains(TEXT("flush")))
        {
            ++TailFlushCount;
        }
        if (Event.AudioIslandIndex == INDEX_NONE || Reason.Contains(TEXT("fallback")) || Reason.Contains(TEXT("missing")))
        {
            ++FallbackCount;
        }
    }

    FString CSV = TEXT("EventIndex,PoseID,SourceWord,WordIndex,TextIslandIndex,AudioIslandIndex,SyntheticRegionIndex,SyntheticRegionFirstEventIndex,SyntheticRegionLastEventIndex,PlanTextCenterSec,ActualCommittedCenterSec,GlobalScaledCounterfactualCenterSec,IslandScaledCounterfactualCenterSec,SyntheticRegionScaledCounterfactualCenterSec,ActualMinusPlanMs,ActualMinusGlobalScaledMs,ActualMinusIslandScaledMs,ActualMinusSyntheticRegionScaledMs,ObservedActiveSec,PlannedActiveSec,GlobalScale,IslandObservedSec,IslandPlannedSec,IslandScale,SyntheticRegionObservedSec,SyntheticRegionPlannedSec,SyntheticRegionScale,CommitReason,PlaybackModeHint\n");

    double ActualAbsMs = 0.0;
    double GlobalAbsMs = 0.0;
    double IslandAbsMs = 0.0;
    double SyntheticAbsMs = 0.0;
    int32 ComparedCount = 0;
    int32 SyntheticCoveredEventCount = 0;

    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        const float PlanCenter = Event.TextDiagnosticCenterSeconds;
        const float ActualCenter = Event.FinalRenderCenterSeconds;
        const float GlobalCenter = bHasSpeech ? (FirstSpeechStartSec + PlanCenter * GlobalScale) : PlanCenter;
        const float IslandObserved = Event.IslandAudioSpanSeconds > 0.0f
            ? Event.IslandAudioSpanSeconds
            : FMath::Max(0.0f, Event.IslandAudioEndSeconds - Event.IslandAudioStartSeconds);
        const float IslandPlanned = Event.PlannerIslandPredictedDurationSeconds > 0.0f
            ? Event.PlannerIslandPredictedDurationSeconds
            : PlannedActiveSec;
        const float IslandScale = IslandPlanned > 0.001f ? IslandObserved / IslandPlanned : 1.0f;
        const float IslandOffset = Event.PlannerEventPredictedOffsetSeconds > 0.0f
            ? Event.PlannerEventPredictedOffsetSeconds
            : FMath::Max(0.0f, PlanCenter - Event.IslandTextStartSeconds);
        const float IslandCenter = (Event.IslandAudioStartSeconds > 0.0f || IslandObserved > 0.0f)
            ? (Event.IslandAudioStartSeconds + IslandOffset * IslandScale)
            : GlobalCenter;

        int32 SyntheticRegionIndex = INDEX_NONE;
        int32 SyntheticRegionFirstEventIndex = INDEX_NONE;
        int32 SyntheticRegionLastEventIndex = INDEX_NONE;
        float SyntheticObserved = 0.0f;
        float SyntheticPlanned = 0.0f;
        float SyntheticScale = 1.0f;
        float SyntheticCenter = GlobalCenter;
        if (const FDiagnosticRegion* Region = FindSyntheticRegionForEvent(Event.EventIndex))
        {
            SyntheticRegionIndex = Region->RegionIndex;
            SyntheticRegionFirstEventIndex = Region->FirstEventIndex;
            SyntheticRegionLastEventIndex = Region->LastEventIndex;
            SyntheticObserved = Region->bHasAudio ? FMath::Max(0.0f, Region->AudioEndSec - Region->AudioStartSec) : 0.0f;
            SyntheticPlanned = FMath::Max(0.001f, Region->PlanEndSec - Region->PlanStartSec);
            SyntheticScale = Region->bHasAudio ? SyntheticObserved / SyntheticPlanned : GlobalScale;
            SyntheticCenter = Region->bHasAudio
                ? (Region->AudioStartSec + FMath::Max(0.0f, PlanCenter - Region->PlanStartSec) * SyntheticScale)
                : GlobalCenter;
            ++SyntheticCoveredEventCount;
        }

        const float ActualMinusPlanMs = (ActualCenter - PlanCenter) * 1000.0f;
        const float ActualMinusGlobalMs = (ActualCenter - GlobalCenter) * 1000.0f;
        const float ActualMinusIslandMs = (ActualCenter - IslandCenter) * 1000.0f;
        const float ActualMinusSyntheticMs = (ActualCenter - SyntheticCenter) * 1000.0f;
        ActualAbsMs += FMath::Abs(ActualMinusPlanMs);
        GlobalAbsMs += FMath::Abs(ActualMinusGlobalMs);
        IslandAbsMs += FMath::Abs(ActualMinusIslandMs);
        SyntheticAbsMs += FMath::Abs(ActualMinusSyntheticMs);
        ++ComparedCount;

        TArray<FString> Columns;
        Columns.Add(FString::Printf(TEXT("%d"), Event.EventIndex));
        Columns.Add(Event.PoseID.ToString());
        Columns.Add(EscapeDebugCSVString(Event.SourceWord));
        Columns.Add(FString::Printf(TEXT("%d"), Event.WordIndex));
        Columns.Add(FString::Printf(TEXT("%d"), Event.TextIslandIndex));
        Columns.Add(FString::Printf(TEXT("%d"), Event.AudioIslandIndex));
        Columns.Add(FString::Printf(TEXT("%d"), SyntheticRegionIndex));
        Columns.Add(FString::Printf(TEXT("%d"), SyntheticRegionFirstEventIndex));
        Columns.Add(FString::Printf(TEXT("%d"), SyntheticRegionLastEventIndex));
        Columns.Add(FString::Printf(TEXT("%.6f"), PlanCenter));
        Columns.Add(FString::Printf(TEXT("%.6f"), ActualCenter));
        Columns.Add(FString::Printf(TEXT("%.6f"), GlobalCenter));
        Columns.Add(FString::Printf(TEXT("%.6f"), IslandCenter));
        Columns.Add(FString::Printf(TEXT("%.6f"), SyntheticCenter));
        Columns.Add(FString::Printf(TEXT("%.3f"), ActualMinusPlanMs));
        Columns.Add(FString::Printf(TEXT("%.3f"), ActualMinusGlobalMs));
        Columns.Add(FString::Printf(TEXT("%.3f"), ActualMinusIslandMs));
        Columns.Add(FString::Printf(TEXT("%.3f"), ActualMinusSyntheticMs));
        Columns.Add(FString::Printf(TEXT("%.6f"), ObservedActiveSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), PlannedActiveSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), GlobalScale));
        Columns.Add(FString::Printf(TEXT("%.6f"), IslandObserved));
        Columns.Add(FString::Printf(TEXT("%.6f"), IslandPlanned));
        Columns.Add(FString::Printf(TEXT("%.6f"), IslandScale));
        Columns.Add(FString::Printf(TEXT("%.6f"), SyntheticObserved));
        Columns.Add(FString::Printf(TEXT("%.6f"), SyntheticPlanned));
        Columns.Add(FString::Printf(TEXT("%.6f"), SyntheticScale));
        Columns.Add(Event.CommitReason.ToString());
        Columns.Add(Event.AudioIslandIndex == INDEX_NONE ? TEXT("fallback_or_missing_audio") : TEXT("speech_active_or_tail"));
        CSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    }

    FFileHelper::SaveStringToFile(CSV, *EventPath);

    const double Den = ComparedCount > 0 ? static_cast<double>(ComparedCount) : 1.0;
    FString Summary = TEXT("PlannedEventCount,CommittedEventCount,ObservedSpeechIslandCount,SyntheticRegionCount,SyntheticRegionCoveredEventCount,PlannedActiveSec,ObservedActiveSec,GlobalScale,TailFlushCount,FallbackOrMissingAudioCount,ActualVsPlanMAEMs,ActualVsGlobalScaledMAEMs,ActualVsIslandScaledMAEMs,ActualVsSyntheticRegionScaledMAEMs\n");
    Summary += FString::Printf(TEXT("%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%d,%d,%.3f,%.3f,%.3f,%.3f\n"),
        ActiveTextVisemePlan.Events.Num(),
        ActiveAlignedVisemeTrack.Events.Num(),
        AudioIslands.Num(),
        SyntheticRegions.Num(),
        SyntheticCoveredEventCount,
        PlannedActiveSec,
        ObservedActiveSec,
        GlobalScale,
        TailFlushCount,
        FallbackCount,
        ActualAbsMs / Den,
        GlobalAbsMs / Den,
        IslandAbsMs / Den,
        SyntheticAbsMs / Den);
    FFileHelper::SaveStringToFile(Summary, *SummaryPath);
}


void UOffgridAILineCoach::WriteLipsyncDebugMotionQualityCSV() const
{
    if (!bLipsyncDebugFileInitialized || LipsyncDebugLineDirectory.IsEmpty())
    {
        return;
    }

    const FString Path = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("motion_quality_plan.csv"));
    const FString SummaryPath = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("motion_quality_summary.csv"));

    const float Duration = FMath::Max(LipsyncEstimatedTextDurationSeconds > 0.0f ? LipsyncEstimatedTextDurationSeconds : ActiveTextVisemePlan.EstimatedDurationSeconds, 0.10f);
    FString CSV = TEXT("EventIndex,NPCID,LineID,SourceText,PoseID,Family,EnvelopeType,TextStartSec,TextPeakSec,TextEndSec,AttackStartSec,PeakStartSec,PeakSec,PeakEndSec,ReleaseEndSec,BaseStrength,MotionPeakStrength,OvershootAmount,JawTarget,RoundTarget,TeethTarget,JawVelocityProxy,JawVelocityLimited,ReleaseSmoothingApplied,MinimumReadablePresenceApplied,RepeatedFamilyMerged,CoarticulationNote\n");

    int32 EnvelopeCollisionCount = 0;
    int32 MergedRepeatedFamilies = 0;
    int32 VelocityLimitedCount = 0;
    int32 ReleaseSmoothedCount = 0;
    int32 MinimumPresenceCount = 0;
    float PeakStrengthSum = 0.0f;
    float PeakStrengthMin = 1.0f;
    float PeakStrengthMax = 0.0f;
    float MaxJawVelocityProxy = 0.0f;
    int32 CountedEvents = 0;

    float PreviousPeakSec = -1.0f;
    float PreviousJawTarget = 0.20f;
    FString PreviousFamily;

    for (int32 Index = 0; Index < ActiveTextVisemePlan.Events.Num(); ++Index)
    {
        const FOffgridAITextVisemeEvent& Event = ActiveTextVisemePlan.Events[Index];
        if (Event.PoseID == NAME_None || Event.Viseme == EOffgridAITextViseme::Rest)
        {
            continue;
        }

        const FOffgridAIDebugMotionEnvelopeProfile Profile = GetDebugMotionEnvelopeProfile(Event.Viseme);
        const FString Family = DebugFamilyForPose(Event.PoseID);
        const float TextStartSec = FMath::Clamp(Event.StartNorm, 0.0f, 1.0f) * Duration;
        const float TextEndSec = FMath::Clamp(Event.EndNorm, Event.StartNorm, 1.0f) * Duration;
        const float TextPeakSec = (TextStartSec + TextEndSec) * 0.5f;
        const float EventDuration = FMath::Max(TextEndSec - TextStartSec, 0.025f);

        const bool bRepeatedFamily = !PreviousFamily.IsEmpty() && PreviousFamily.Equals(Family, ESearchCase::IgnoreCase) && PreviousPeakSec >= 0.0f && (TextPeakSec - PreviousPeakSec) < 0.180f;
        if (bRepeatedFamily)
        {
            ++MergedRepeatedFamilies;
        }

        float PeakStartSec = FMath::Max(TextStartSec, TextPeakSec - Profile.MinPeakHoldSeconds * 0.50f);
        float PeakEndSec = FMath::Min(TextEndSec, TextPeakSec + Profile.MinPeakHoldSeconds * 0.50f);
        bool bMinimumPresenceApplied = false;
        if (Profile.bVowel && (PeakEndSec - PeakStartSec) < Profile.MinPeakHoldSeconds)
        {
            const float Half = Profile.MinPeakHoldSeconds * 0.5f;
            PeakStartSec = FMath::Max(TextStartSec, TextPeakSec - Half);
            PeakEndSec = FMath::Min(TextEndSec + 0.050f, TextPeakSec + Half);
            bMinimumPresenceApplied = true;
            ++MinimumPresenceCount;
        }

        float AttackStartSec = FMath::Max(0.0f, PeakStartSec - Profile.AttackLeadSeconds);
        float ReleaseEndSec = FMath::Min(Duration + 0.250f, PeakEndSec + Profile.ReleaseTailSeconds);
        bool bReleaseSmoothingApplied = false;

        if (Index + 1 < ActiveTextVisemePlan.Events.Num())
        {
            const FOffgridAITextVisemeEvent& NextEvent = ActiveTextVisemePlan.Events[Index + 1];
            const FString NextFamily = DebugFamilyForPose(NextEvent.PoseID);
            const float NextStartSec = FMath::Clamp(NextEvent.StartNorm, 0.0f, 1.0f) * Duration;
            const bool bSimilarNext = Family.Equals(NextFamily, ESearchCase::IgnoreCase);
            if (bSimilarNext && NextStartSec > TextStartSec && NextStartSec - TextEndSec < 0.160f)
            {
                ReleaseEndSec = FMath::Max(ReleaseEndSec, FMath::Min(Duration + 0.250f, NextStartSec + 0.055f));
                bReleaseSmoothingApplied = true;
            }
            else if (NextStartSec - TextEndSec > 0.0f && NextStartSec - TextEndSec < 0.070f)
            {
                ReleaseEndSec = FMath::Max(ReleaseEndSec, NextStartSec + 0.025f);
                bReleaseSmoothingApplied = true;
            }
        }
        if (bReleaseSmoothingApplied)
        {
            ++ReleaseSmoothedCount;
        }

        const float GapToPreviousPeak = PreviousPeakSec >= 0.0f ? FMath::Max(TextPeakSec - PreviousPeakSec, 0.001f) : 1.0f;
        const float JawVelocityProxy = PreviousPeakSec >= 0.0f ? FMath::Abs(Profile.JawTarget - PreviousJawTarget) / GapToPreviousPeak : 0.0f;
        const bool bJawVelocityLimited = JawVelocityProxy > Profile.MaxJawVelocityPerSecond;
        if (bJawVelocityLimited)
        {
            ++VelocityLimitedCount;
        }
        MaxJawVelocityProxy = FMath::Max(MaxJawVelocityProxy, JawVelocityProxy);

        float MotionPeakStrength = FMath::Clamp(Event.Strength * Profile.PeakStrengthMultiplier, 0.0f, 1.0f);
        if (Profile.bVowel)
        {
            MotionPeakStrength = FMath::Max(MotionPeakStrength, 0.42f); // Keep weak syllables readable after v21 simplification.
        }
        if (bRepeatedFamily)
        {
            MotionPeakStrength *= 0.88f; // Same-family continuation should evolve rather than re-trigger at full strength.
        }
        if (bJawVelocityLimited)
        {
            MotionPeakStrength *= 0.92f; // Diagnostic proxy for inertia: prefer reduced amplitude over impossible speed.
        }
        MotionPeakStrength = FMath::Clamp(MotionPeakStrength + Profile.OvershootAmount * 0.25f, 0.0f, 1.0f);

        PeakStrengthSum += MotionPeakStrength;
        PeakStrengthMin = FMath::Min(PeakStrengthMin, MotionPeakStrength);
        PeakStrengthMax = FMath::Max(PeakStrengthMax, MotionPeakStrength);
        ++CountedEvents;

        if (PreviousPeakSec >= 0.0f && AttackStartSec < PreviousPeakSec + 0.025f)
        {
            ++EnvelopeCollisionCount;
        }

        FString Note;
        if (bRepeatedFamily)
        {
            Note += TEXT("SameFamilyContinuation; ");
        }
        if (bJawVelocityLimited)
        {
            Note += TEXT("JawVelocityProxyLimited; ");
        }
        if (bReleaseSmoothingApplied)
        {
            Note += TEXT("ReleaseSmoothedIntoNeighbor; ");
        }
        if (bMinimumPresenceApplied)
        {
            Note += TEXT("MinimumReadableVowelPresence; ");
        }
        if (Note.IsEmpty())
        {
            Note = TEXT("None");
        }

        TArray<FString> Columns;
        Columns.Add(FString::Printf(TEXT("%d"), Index));
        Columns.Add(NPCID.ToString());
        Columns.Add(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None")));
        Columns.Add(EscapeDebugCSVString(Event.SourceText));
        Columns.Add(Event.PoseID.ToString());
        Columns.Add(Family);
        Columns.Add(Profile.Name);
        Columns.Add(FString::Printf(TEXT("%.6f"), TextStartSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), TextPeakSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), TextEndSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), AttackStartSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), PeakStartSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), TextPeakSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), PeakEndSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), ReleaseEndSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Clamp01(Event.Strength)));
        Columns.Add(FString::Printf(TEXT("%.6f"), MotionPeakStrength));
        Columns.Add(FString::Printf(TEXT("%.6f"), Profile.OvershootAmount));
        Columns.Add(FString::Printf(TEXT("%.6f"), Profile.JawTarget));
        Columns.Add(FString::Printf(TEXT("%.6f"), Profile.RoundTarget));
        Columns.Add(FString::Printf(TEXT("%.6f"), Profile.TeethTarget));
        Columns.Add(FString::Printf(TEXT("%.6f"), JawVelocityProxy));
        Columns.Add(bJawVelocityLimited ? TEXT("1") : TEXT("0"));
        Columns.Add(bReleaseSmoothingApplied ? TEXT("1") : TEXT("0"));
        Columns.Add(bMinimumPresenceApplied ? TEXT("1") : TEXT("0"));
        Columns.Add(bRepeatedFamily ? TEXT("1") : TEXT("0"));
        Columns.Add(EscapeDebugCSVString(Note));
        CSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");

        PreviousPeakSec = TextPeakSec;
        PreviousJawTarget = Profile.JawTarget;
        PreviousFamily = Family;
    }

    FFileHelper::SaveStringToFile(CSV, *Path);

    const float MeanPeakStrength = CountedEvents > 0 ? PeakStrengthSum / static_cast<float>(CountedEvents) : 0.0f;
    const float ReadabilityScore = FMath::Clamp(1.0f
        - static_cast<float>(EnvelopeCollisionCount) * 0.035f
        - static_cast<float>(VelocityLimitedCount) * 0.045f
        + static_cast<float>(ReleaseSmoothedCount) * 0.015f
        + static_cast<float>(MinimumPresenceCount) * 0.010f,
        0.0f,
        1.0f);

    FString Summary = TEXT("LineID,NPCID,CountedEvents,EnvelopeCollisionCount,MergedRepeatedVisemes,ReleaseSmoothedCount,MinimumReadablePresenceCount,JawVelocityLimitedCount,PeakStrengthMean,PeakStrengthMin,PeakStrengthMax,MaxJawVelocityProxy,ReadabilityScore,TimingAuthority\n");
    Summary += FString::Printf(TEXT("%s,%s,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%s\n"),
        *(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None"))),
        *NPCID.ToString(),
        CountedEvents,
        EnvelopeCollisionCount,
        MergedRepeatedFamilies,
        ReleaseSmoothedCount,
        MinimumPresenceCount,
        VelocityLimitedCount,
        MeanPeakStrength,
        CountedEvents > 0 ? PeakStrengthMin : 0.0f,
        CountedEvents > 0 ? PeakStrengthMax : 0.0f,
        MaxJawVelocityProxy,
        ReadabilityScore,
        TEXT("AudioAnchorAlignerFinalTrack"));
    FFileHelper::SaveStringToFile(Summary, *SummaryPath);
}





namespace
{
    struct FOffgridAILipsyncScoreOracleRow
    {
        int32 EventIndex = INDEX_NONE;
        float PlannedWeakSec = 0.0f;
        float BestAudioSec = 0.0f;
        float Confidence = 0.0f;
        bool bHasOracle = false;
    };

    struct FOffgridAILipsyncScorePeakRow
    {
        int32 EventIndex = INDEX_NONE;
        float PeakPlaybackSec = 0.0f;
        float PeakWeight = 0.0f;
        float SourceStrength = 0.0f;
        bool bHasPeak = false;
    };

    static TArray<FString> ParseLipsyncScoreCSVLineLoose(const FString& Line)
    {
        TArray<FString> Fields;
        FString Current;
        bool bInQuotes = false;
        for (int32 I = 0; I < Line.Len(); ++I)
        {
            const TCHAR C = Line[I];
            if (C == TEXT('"'))
            {
                if (bInQuotes && I + 1 < Line.Len() && Line[I + 1] == TEXT('"'))
                {
                    Current.AppendChar(TEXT('"'));
                    ++I;
                }
                else
                {
                    bInQuotes = !bInQuotes;
                }
            }
            else if (C == TEXT(',') && !bInQuotes)
            {
                Fields.Add(Current);
                Current.Reset();
            }
            else
            {
                Current.AppendChar(C);
            }
        }
        Fields.Add(Current);
        return Fields;
    }

    static int32 FindLipsyncScoreCSVColumn(const TArray<FString>& Header, const TCHAR* Name)
    {
        for (int32 I = 0; I < Header.Num(); ++I)
        {
            if (Header[I].Equals(Name, ESearchCase::IgnoreCase))
            {
                return I;
            }
        }
        return INDEX_NONE;
    }

    static float GetLipsyncScoreFloatField(const TArray<FString>& Fields, int32 Index, float DefaultValue = 0.0f)
    {
        if (!Fields.IsValidIndex(Index))
        {
            return DefaultValue;
        }
        return FCString::Atof(*Fields[Index]);
    }

    static int32 GetLipsyncScoreIntField(const TArray<FString>& Fields, int32 Index, int32 DefaultValue = INDEX_NONE)
    {
        if (!Fields.IsValidIndex(Index))
        {
            return DefaultValue;
        }
        return FCString::Atoi(*Fields[Index]);
    }

    static float MeanAbsMs(const TArray<float>& ErrorsSec)
    {
        if (ErrorsSec.Num() <= 0)
        {
            return 0.0f;
        }
        double Sum = 0.0;
        for (float E : ErrorsSec)
        {
            Sum += FMath::Abs(E) * 1000.0;
        }
        return static_cast<float>(Sum / static_cast<double>(ErrorsSec.Num()));
    }

    static float MeanMs(const TArray<float>& ErrorsSec)
    {
        if (ErrorsSec.Num() <= 0)
        {
            return 0.0f;
        }
        double Sum = 0.0;
        for (float E : ErrorsSec)
        {
            Sum += E * 1000.0;
        }
        return static_cast<float>(Sum / static_cast<double>(ErrorsSec.Num()));
    }

    static float P90AbsMs(TArray<float> ErrorsSec)
    {
        if (ErrorsSec.Num() <= 0)
        {
            return 0.0f;
        }
        for (float& E : ErrorsSec)
        {
            E = FMath::Abs(E) * 1000.0f;
        }
        ErrorsSec.Sort();
        const int32 Index = FMath::Clamp(FMath::CeilToInt(static_cast<float>(ErrorsSec.Num()) * 0.90f) - 1, 0, ErrorsSec.Num() - 1);
        return ErrorsSec[Index];
    }
}

void UOffgridAILineCoach::WriteLipsyncDebugScorecards() const
{
    static const TArray<FOffgridAIStreamingSpeechIsland> EmptySpeechIslands;

    if (!bLipsyncDebugFileInitialized || LipsyncDebugLineDirectory.IsEmpty())
    {
        return;
    }

    const FString NPCString = NPCID.ToString();
    const FString LineString = bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : ActiveAlignedVisemeTrack.LineID.ToString();
    const int32 PlannedCount = ActiveTextVisemePlan.Events.Num();

    TMap<int32, FOffgridAILipsyncScoreOracleRow> OracleRows;
    const FString AlignmentPath = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("audio_plan_alignment.csv"));
    FString AlignmentText;
    if (FFileHelper::LoadFileToString(AlignmentText, *AlignmentPath))
    {
        TArray<FString> Lines;
        AlignmentText.ParseIntoArrayLines(Lines, false);
        if (Lines.Num() > 0)
        {
            const TArray<FString> Header = ParseLipsyncScoreCSVLineLoose(Lines[0]);
            const int32 EventIndexCol = FindLipsyncScoreCSVColumn(Header, TEXT("EventIndex"));
            const int32 WeakCol = FindLipsyncScoreCSVColumn(Header, TEXT("WeakExpectedSec_AudioWarp"));
            const int32 BestCol = FindLipsyncScoreCSVColumn(Header, TEXT("BestAudioSec"));
            const int32 ConfidenceCol = FindLipsyncScoreCSVColumn(Header, TEXT("Confidence"));
            for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
            {
                const TArray<FString> Fields = ParseLipsyncScoreCSVLineLoose(Lines[LineIndex]);
                const int32 EventIndex = GetLipsyncScoreIntField(Fields, EventIndexCol);
                if (EventIndex == INDEX_NONE)
                {
                    continue;
                }
                FOffgridAILipsyncScoreOracleRow& Row = OracleRows.FindOrAdd(EventIndex);
                Row.EventIndex = EventIndex;
                Row.PlannedWeakSec = GetLipsyncScoreFloatField(Fields, WeakCol);
                Row.BestAudioSec = GetLipsyncScoreFloatField(Fields, BestCol);
                Row.Confidence = GetLipsyncScoreFloatField(Fields, ConfidenceCol);
                Row.bHasOracle = BestCol != INDEX_NONE && Row.BestAudioSec > 0.0f;
            }
        }
    }

    TMap<int32, FOffgridAILipsyncScorePeakRow> PeakRows;
    const FString SubmittedPath = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("submitted_poses.csv"));
    FString SubmittedText;
    if (FFileHelper::LoadFileToString(SubmittedText, *SubmittedPath))
    {
        TArray<FString> Lines;
        SubmittedText.ParseIntoArrayLines(Lines, false);
        if (Lines.Num() > 0)
        {
            const TArray<FString> Header = ParseLipsyncScoreCSVLineLoose(Lines[0]);
            const int32 EventIndexCol = FindLipsyncScoreCSVColumn(Header, TEXT("SourceEventIndex"));
            const int32 PlaybackCol = FindLipsyncScoreCSVColumn(Header, TEXT("PlaybackSec"));
            const int32 FaceWeightCol = FindLipsyncScoreCSVColumn(Header, TEXT("FaceDriverWeight"));
            const int32 StrengthCol = FindLipsyncScoreCSVColumn(Header, TEXT("SourceStrength"));
            for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
            {
                const TArray<FString> Fields = ParseLipsyncScoreCSVLineLoose(Lines[LineIndex]);
                const int32 EventIndex = GetLipsyncScoreIntField(Fields, EventIndexCol);
                if (EventIndex == INDEX_NONE)
                {
                    continue;
                }
                const float FaceWeight = GetLipsyncScoreFloatField(Fields, FaceWeightCol);
                FOffgridAILipsyncScorePeakRow& Peak = PeakRows.FindOrAdd(EventIndex);
                if (!Peak.bHasPeak || FaceWeight > Peak.PeakWeight)
                {
                    Peak.EventIndex = EventIndex;
                    Peak.PeakPlaybackSec = GetLipsyncScoreFloatField(Fields, PlaybackCol);
                    Peak.PeakWeight = FaceWeight;
                    Peak.SourceStrength = GetLipsyncScoreFloatField(Fields, StrengthCol);
                    Peak.bHasPeak = true;
                }
            }
        }
    }

    TMap<int32, const FOffgridAIAlignedVisemeEvent*> CommittedByEventIndex;
    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        CommittedByEventIndex.FindOrAdd(Event.EventIndex) = &Event;
    }

    int32 CommittedCount = 0;
    int32 SubmittedEventCount = 0;
    int32 MissingCommittedCount = 0;
    int32 MissingSubmittedCount = 0;
    int32 MonotonicViolations = 0;
    int32 WeakPeakCount = 0;
    int32 StrongPeakCount = 0;
    float PreviousCenterSec = -1.0f;
    int32 PreviousEventIndex = INDEX_NONE;
    float LastCommittedCenterSec = 0.0f;
    float LastSubmittedPeakPlaybackSec = 0.0f;

    TArray<float> BudgeterErrorsSec;
    TArray<float> AlignerErrorsSec;
    TArray<float> PerformerPeakErrorsSec;
    TArray<float> FacePeakMagnitudeErrors;
    TArray<float> CommitLeadSec;
    int32 CommitReasonWithinHorizonCount = 0;
    int32 CommitReasonAheadOfHorizonCount = 0;
    int32 CommitReasonEndOfStreamCount = 0;
    int32 CommitReasonFallbackCount = 0;
    int32 CommitReasonUnknownCount = 0;
    TArray<float> EffectiveScales;
    TArray<float> EffectiveCommitHorizonsSec;

    FString OracleCSV = TEXT("EventIndex,NPCID,LineID,SourceText,PoseID,TextCenterSec,OracleBestAudioSec,CommittedPlaybackCenterSec,FaceDriverPeakPlaybackSec,FaceDriverPeakWeight,CommitPlaybackSec,CommitLeadMs,CommitReason,EffectiveSegmentScale,EffectiveCommitHorizonMs,PlannerIslandPredictedDurationSec,PlannerEventPredictedOffsetSec,PlannerProsodyGroupIndex,PlannerProsodyRole,PlannerProsodyGroupAllocatedSec,BudgeterErrorMs,AudioAlignerErrorMs,PerformerPeakErrorMs,Notes\n");

    for (int32 EventIndex = 0; EventIndex < PlannedCount; ++EventIndex)
    {
        const FOffgridAITextVisemeEvent& PlanEvent = ActiveTextVisemePlan.Events[EventIndex];
        const float StartNorm = FMath::Clamp(PlanEvent.StartNorm, 0.0f, 1.0f);
        const float EndNorm = FMath::Clamp(PlanEvent.EndNorm, StartNorm, 1.0f);
        const float TextCenterSec = ((StartNorm + EndNorm) * 0.5f) * FMath::Max(LipsyncEstimatedTextDurationSeconds > 0.0f ? LipsyncEstimatedTextDurationSeconds : ActiveTextVisemePlan.EstimatedDurationSeconds, 0.10f);

        const FOffgridAIAlignedVisemeEvent* const* CommittedPtr = CommittedByEventIndex.Find(EventIndex);
        const FOffgridAIAlignedVisemeEvent* Committed = CommittedPtr ? *CommittedPtr : nullptr;
        const FOffgridAILipsyncScoreOracleRow* Oracle = OracleRows.Find(EventIndex);
        const FOffgridAILipsyncScorePeakRow* Peak = PeakRows.Find(EventIndex);

        FString Notes;
        float BudgeterErrorMs = 0.0f;
        float AlignerErrorMs = 0.0f;
        float PerformerErrorMs = 0.0f;

        if (Committed)
        {
            ++CommittedCount;
            CommitLeadSec.Add(Committed->CommitLeadSeconds);
            const FString CommitReasonString = Committed->CommitReason.ToString();
            if (CommitReasonString == TEXT("within_horizon"))
            {
                ++CommitReasonWithinHorizonCount;
            }
            else if (CommitReasonString == TEXT("ahead_of_horizon_track_rebuild"))
            {
                ++CommitReasonAheadOfHorizonCount;
            }
            else if (CommitReasonString == TEXT("end_of_stream") || CommitReasonString == TEXT("end_of_stream_flush"))
            {
                ++CommitReasonEndOfStreamCount;
            }
            else if (CommitReasonString == TEXT("fallback_or_missing_audio") || CommitReasonString == TEXT("late_publication"))
            {
                ++CommitReasonFallbackCount;
            }
            else
            {
                ++CommitReasonUnknownCount;
            }
            EffectiveScales.Add(Committed->EffectiveSegmentScale);
            EffectiveCommitHorizonsSec.Add(Committed->EffectiveCommitHorizonSeconds);
            if (PreviousEventIndex != INDEX_NONE && Committed->FinalRenderCenterSeconds <= PreviousCenterSec + 0.001f)
            {
                ++MonotonicViolations;
                Notes += TEXT("MonotonicViolation;");
            }
            PreviousEventIndex = EventIndex;
            PreviousCenterSec = Committed->FinalRenderCenterSeconds;
            LastCommittedCenterSec = FMath::Max(LastCommittedCenterSec, Committed->FinalRenderCenterSeconds);
        }
        else
        {
            ++MissingCommittedCount;
            Notes += TEXT("MissingCommitted;");
        }

        if (Peak && Peak->bHasPeak)
        {
            ++SubmittedEventCount;
            LastSubmittedPeakPlaybackSec = FMath::Max(LastSubmittedPeakPlaybackSec, Peak->PeakPlaybackSec);
            if (Peak->PeakWeight < 0.18f && PlanEvent.Strength >= 0.45f)
            {
                ++WeakPeakCount;
                Notes += TEXT("WeakFacePeak;");
            }
            else
            {
                ++StrongPeakCount;
            }
        }
        else
        {
            ++MissingSubmittedCount;
            Notes += TEXT("MissingSubmittedPeak;");
        }

        if (Oracle && Oracle->bHasOracle)
        {
            const float BudgeterErrorSec = Oracle->PlannedWeakSec - Oracle->BestAudioSec;
            BudgeterErrorsSec.Add(BudgeterErrorSec);
            BudgeterErrorMs = BudgeterErrorSec * 1000.0f;
            if (Committed)
            {
                const float AlignerErrorSec = Committed->FinalRenderCenterSeconds - Oracle->BestAudioSec;
                AlignerErrorsSec.Add(AlignerErrorSec);
                AlignerErrorMs = AlignerErrorSec * 1000.0f;
            }
        }
        else
        {
            Notes += TEXT("NoOracle;");
        }

        if (Committed && Peak && Peak->bHasPeak)
        {
            const float PerformerErrorSec = Peak->PeakPlaybackSec - Committed->FinalRenderCenterSeconds;
            PerformerPeakErrorsSec.Add(PerformerErrorSec);
            PerformerErrorMs = PerformerErrorSec * 1000.0f;
            FacePeakMagnitudeErrors.Add(Peak->PeakWeight - PlanEvent.Strength);
        }

        TArray<FString> Columns;
        Columns.Add(FString::FromInt(EventIndex));
        Columns.Add(NPCString);
        Columns.Add(LineString);
        Columns.Add(EscapeDebugCSVString(PlanEvent.SourceText));
        Columns.Add(PlanEvent.PoseID.ToString());
        Columns.Add(FString::Printf(TEXT("%.6f"), TextCenterSec));
        Columns.Add(Oracle && Oracle->bHasOracle ? FString::Printf(TEXT("%.6f"), Oracle->BestAudioSec) : TEXT(""));
        Columns.Add(Committed ? FString::Printf(TEXT("%.6f"), Committed->FinalRenderCenterSeconds) : TEXT(""));
        Columns.Add(Peak && Peak->bHasPeak ? FString::Printf(TEXT("%.6f"), Peak->PeakPlaybackSec) : TEXT(""));
        Columns.Add(Peak && Peak->bHasPeak ? FString::Printf(TEXT("%.6f"), Peak->PeakWeight) : TEXT(""));
        Columns.Add(Committed ? FString::Printf(TEXT("%.6f"), Committed->CommitPlaybackSeconds) : TEXT(""));
        Columns.Add(Committed ? FString::Printf(TEXT("%.3f"), Committed->CommitLeadSeconds * 1000.0f) : TEXT(""));
        Columns.Add(Committed ? Committed->CommitReason.ToString() : TEXT(""));
        Columns.Add(Committed ? FString::Printf(TEXT("%.6f"), Committed->EffectiveSegmentScale) : TEXT(""));
        Columns.Add(Committed ? FString::Printf(TEXT("%.3f"), Committed->EffectiveCommitHorizonSeconds * 1000.0f) : TEXT(""));
        Columns.Add(Committed ? FString::Printf(TEXT("%.6f"), Committed->PlannerIslandPredictedDurationSeconds) : TEXT(""));
        Columns.Add(Committed ? FString::Printf(TEXT("%.6f"), Committed->PlannerEventPredictedOffsetSeconds) : TEXT(""));
        Columns.Add(Committed ? FString::FromInt(Committed->PlannerProsodyGroupIndex) : TEXT(""));
        Columns.Add(Committed ? Committed->PlannerProsodyRole.ToString() : TEXT(""));
        Columns.Add(Committed ? FString::Printf(TEXT("%.6f"), Committed->PlannerProsodyGroupAllocatedSeconds) : TEXT(""));
        Columns.Add(Oracle && Oracle->bHasOracle ? FString::Printf(TEXT("%.3f"), BudgeterErrorMs) : TEXT(""));
        Columns.Add((Oracle && Oracle->bHasOracle && Committed) ? FString::Printf(TEXT("%.3f"), AlignerErrorMs) : TEXT(""));
        Columns.Add((Committed && Peak && Peak->bHasPeak) ? FString::Printf(TEXT("%.3f"), PerformerErrorMs) : TEXT(""));
        Columns.Add(EscapeDebugCSVString(Notes));
        OracleCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    }

    FFileHelper::SaveStringToFile(OracleCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("oracle_alignment.csv")));

    FString ScaleCSV = TEXT("EventIndex,NPCID,LineID,SourceText,PoseID,CommittedPlaybackCenterSec,CommitPlaybackSec,CommitLeadMs,CommitReason,EffectiveSegmentScale,EffectiveCommitHorizonMs,RequiredActiveElapsedSec,ObservedActiveElapsedSec,ActiveProgressDeficitSec,RequiredProgressNorm,ObservedProgressNorm,ActiveProgressRatio,MappedToObservedSpeech,PlannerIslandPredictedDurationSec,PlannerIslandSpeechMaterialSec,PlannerIslandPunctuationSec,PlannerIslandShortUtteranceFloorSec,PlannerEventPredictedOffsetSec,PlannerEventNormCenter,PlannerProsodyGroupIndex,PlannerProsodyRole,PlannerProsodyGroupWeight,PlannerProsodyGroupAllocatedSec,ProsodyGroupIndex,ProsodyGroupRole,ProsodyGroupReleaseReason,ProsodyGroupExpectedReleaseSec,ProsodyGroupEarliestReleaseSec,ProsodyGroupLatestReleaseSec,ProsodyGroupActualReleaseSec,ProsodyGroupReleaseLagMs\n");
    TArray<const FOffgridAIAlignedVisemeEvent*> SortedCommittedEvents;
    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        SortedCommittedEvents.Add(&Event);
    }
    SortedCommittedEvents.Sort([](const FOffgridAIAlignedVisemeEvent& A, const FOffgridAIAlignedVisemeEvent& B)
    {
        return A.EventIndex < B.EventIndex;
    });
    for (const FOffgridAIAlignedVisemeEvent* Event : SortedCommittedEvents)
    {
        if (!Event)
        {
            continue;
        }
        TArray<FString> Columns;
        Columns.Add(FString::FromInt(Event->EventIndex));
        Columns.Add(NPCString);
        Columns.Add(LineString);
        Columns.Add(EscapeDebugCSVString(Event->SourceWord));
        Columns.Add(Event->PoseID.ToString());
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->FinalRenderCenterSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->CommitPlaybackSeconds));
        Columns.Add(FString::Printf(TEXT("%.3f"), Event->CommitLeadSeconds * 1000.0f));
        Columns.Add(Event->CommitReason.ToString());
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->EffectiveSegmentScale));
        Columns.Add(FString::Printf(TEXT("%.3f"), Event->EffectiveCommitHorizonSeconds * 1000.0f));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->RequiredActiveElapsedSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->ObservedActiveElapsedSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->ActiveProgressDeficitSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->RequiredProgressNorm));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->ObservedProgressNorm));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->ActiveProgressRatio));
        Columns.Add(Event->bMappedToObservedSpeech ? TEXT("1") : TEXT("0"));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->PlannerIslandPredictedDurationSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->PlannerIslandSpeechMaterialSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->PlannerIslandPunctuationSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->PlannerIslandShortUtteranceFloorSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->PlannerEventPredictedOffsetSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->PlannerEventNormCenter));
        Columns.Add(FString::FromInt(Event->PlannerProsodyGroupIndex));
        Columns.Add(Event->PlannerProsodyRole.ToString());
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->PlannerProsodyGroupWeight));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->PlannerProsodyGroupAllocatedSeconds));
        Columns.Add(FString::FromInt(Event->ProsodyGroupIndex));
        Columns.Add(Event->ProsodyGroupRole.ToString());
        Columns.Add(Event->ProsodyGroupReleaseReason.ToString());
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->ProsodyGroupExpectedReleaseSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->ProsodyGroupEarliestReleaseSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->ProsodyGroupLatestReleaseSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event->ProsodyGroupActualReleaseSeconds));
        Columns.Add(FString::Printf(TEXT("%.3f"), Event->ProsodyGroupReleaseLagSeconds * 1000.0f));
        ScaleCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    }
    FFileHelper::SaveStringToFile(ScaleCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("scale_estimate_over_time.csv")));

    // Alpha17 UE diagnostic: island-level duration/mapping attribution.
    // This is intentionally diagnostic-only. It lets us distinguish three cases:
    // 1) planner island budget is too short/long,
    // 2) detector island span is too short/long,
    // 3) committed event centers are being compressed/expanded inside an otherwise reasonable island.
    struct FOffgridAIIslandTimingDebugRow
    {
        int32 TextIslandIndex = INDEX_NONE;
        int32 AudioIslandIndex = INDEX_NONE;
        int32 EventCount = 0;
        int32 FirstEventIndex = INDEX_NONE;
        int32 LastEventIndex = INDEX_NONE;
        FString FirstSourceWord;
        FString LastSourceWord;
        float FirstCommittedCenterSec = 0.0f;
        float LastCommittedCenterSec = 0.0f;
        float PlannerIslandPredictedDurationSec = 0.0f;
        float PlannerIslandSpeechMaterialSec = 0.0f;
        float PlannerIslandPunctuationSec = 0.0f;
        float PlannerProsodyIslandEventCountModelSec = 0.0f;
        float PlannerProsodyIslandSpeechBudgetSec = 0.0f;
        float PlannerProsodyAllocationScale = 0.0f;
        float MappedAudioStartSec = 0.0f;
        float MappedAudioEndSec = 0.0f;
        float DetectorAudioStartSec = 0.0f;
        float DetectorAudioEndSec = 0.0f;
        bool bHasDetectorIsland = false;
    };

    TMap<int32, FOffgridAIIslandTimingDebugRow> IslandTimingRows;
    for (const FOffgridAIAlignedVisemeEvent* Event : SortedCommittedEvents)
    {
        if (!Event)
        {
            continue;
        }
        const int32 IslandKey = Event->TextIslandIndex != INDEX_NONE ? Event->TextIslandIndex : -100000 - Event->PhraseIndex;
        FOffgridAIIslandTimingDebugRow& Row = IslandTimingRows.FindOrAdd(IslandKey);
        if (Row.EventCount == 0)
        {
            Row.TextIslandIndex = Event->TextIslandIndex;
            Row.AudioIslandIndex = Event->AudioIslandIndex;
            Row.FirstEventIndex = Event->EventIndex;
            Row.LastEventIndex = Event->EventIndex;
            Row.FirstSourceWord = Event->SourceWord;
            Row.LastSourceWord = Event->SourceWord;
            Row.FirstCommittedCenterSec = Event->FinalRenderCenterSeconds;
            Row.LastCommittedCenterSec = Event->FinalRenderCenterSeconds;
            Row.PlannerIslandPredictedDurationSec = Event->PlannerIslandPredictedDurationSeconds;
            Row.PlannerIslandSpeechMaterialSec = Event->PlannerIslandSpeechMaterialSeconds;
            Row.PlannerIslandPunctuationSec = Event->PlannerIslandPunctuationSeconds;
            Row.PlannerProsodyIslandEventCountModelSec = Event->PlannerProsodyIslandEventCountModelSeconds;
            Row.PlannerProsodyIslandSpeechBudgetSec = Event->PlannerProsodyIslandSpeechBudgetSeconds;
            Row.PlannerProsodyAllocationScale = Event->PlannerProsodyAllocationScale;
            Row.MappedAudioStartSec = Event->IslandAudioStartSeconds;
            Row.MappedAudioEndSec = Event->IslandAudioEndSeconds;
        }
        else
        {
            if (Event->EventIndex < Row.FirstEventIndex || Row.FirstEventIndex == INDEX_NONE)
            {
                Row.FirstEventIndex = Event->EventIndex;
                Row.FirstSourceWord = Event->SourceWord;
                Row.FirstCommittedCenterSec = Event->FinalRenderCenterSeconds;
            }
            if (Event->EventIndex > Row.LastEventIndex || Row.LastEventIndex == INDEX_NONE)
            {
                Row.LastEventIndex = Event->EventIndex;
                Row.LastSourceWord = Event->SourceWord;
                Row.LastCommittedCenterSec = Event->FinalRenderCenterSeconds;
            }
            Row.MappedAudioStartSec = FMath::Min(Row.MappedAudioStartSec, Event->IslandAudioStartSeconds);
            Row.MappedAudioEndSec = FMath::Max(Row.MappedAudioEndSec, Event->IslandAudioEndSeconds);
            Row.PlannerIslandPredictedDurationSec = FMath::Max(Row.PlannerIslandPredictedDurationSec, Event->PlannerIslandPredictedDurationSeconds);
            Row.PlannerIslandSpeechMaterialSec = FMath::Max(Row.PlannerIslandSpeechMaterialSec, Event->PlannerIslandSpeechMaterialSeconds);
            Row.PlannerIslandPunctuationSec = FMath::Max(Row.PlannerIslandPunctuationSec, Event->PlannerIslandPunctuationSeconds);
            Row.PlannerProsodyIslandEventCountModelSec = FMath::Max(Row.PlannerProsodyIslandEventCountModelSec, Event->PlannerProsodyIslandEventCountModelSeconds);
            Row.PlannerProsodyIslandSpeechBudgetSec = FMath::Max(Row.PlannerProsodyIslandSpeechBudgetSec, Event->PlannerProsodyIslandSpeechBudgetSeconds);
            Row.PlannerProsodyAllocationScale = FMath::Max(Row.PlannerProsodyAllocationScale, Event->PlannerProsodyAllocationScale);
        }
        ++Row.EventCount;
    }

    const TArray<FOffgridAIStreamingSpeechIsland>& IslandTimingSpeechIslands = LipsyncRuntimeSession ? LipsyncRuntimeSession->GetSpeechIslands() : EmptySpeechIslands;
    for (TPair<int32, FOffgridAIIslandTimingDebugRow>& Pair : IslandTimingRows)
    {
        FOffgridAIIslandTimingDebugRow& Row = Pair.Value;
        for (const FOffgridAIStreamingSpeechIsland& SpeechIsland : IslandTimingSpeechIslands)
        {
            if (SpeechIsland.IslandIndex == Row.AudioIslandIndex)
            {
                Row.DetectorAudioStartSec = SpeechIsland.AudioBufferStartSec;
                Row.DetectorAudioEndSec = SpeechIsland.bEnded && SpeechIsland.AudioBufferEndSec > SpeechIsland.AudioBufferStartSec
                    ? SpeechIsland.AudioBufferEndSec
                    : FMath::Max(SpeechIsland.AudioBufferLastSpeechSec, SpeechIsland.AudioBufferStartSec);
                Row.bHasDetectorIsland = SpeechIsland.bStarted;
                break;
            }
        }
    }

    FString IslandTimingCSV =
        TEXT("TextIslandIndex,AudioIslandIndex,EventCount,FirstEventIndex,LastEventIndex,")
        TEXT("FirstSourceWord,LastSourceWord,FirstCommittedCenterSec,LastCommittedCenterSec,")
        TEXT("CommittedCenterSpanSec,PlannerIslandPredictedDurationSec,")
        TEXT("PlannerIslandSpeechMaterialSec,PlannerIslandPunctuationSec,")
        TEXT("PlannerProsodyIslandEventCountModelSec,")
        TEXT("PlannerProsodyIslandSpeechBudgetSec,")
        TEXT("PlannerProsodyAllocationScale,")
        TEXT("MappedAudioStartSec,MappedAudioEndSec,MappedAudioSpanSec,")
        TEXT("DetectorAudioStartSec,DetectorAudioEndSec,DetectorAudioSpanSec,")
        TEXT("StartErrorVsDetectorMs,EndErrorVsDetectorMs,DurationErrorVsDetectorMs,")
        TEXT("EventCountModelToDetectorRatio,SpeechBudgetToDetectorRatio,")
        TEXT("CommittedSpanToDetectorRatio,Notes\r\n");
    TArray<int32> IslandTimingKeys;
    IslandTimingRows.GetKeys(IslandTimingKeys);
    IslandTimingKeys.Sort();
    for (int32 IslandKey : IslandTimingKeys)
    {
        const FOffgridAIIslandTimingDebugRow* RowPtr = IslandTimingRows.Find(IslandKey);
        if (!RowPtr)
        {
            continue;
        }
        const FOffgridAIIslandTimingDebugRow& Row = *RowPtr;
        const float CommittedSpanSec = FMath::Max(0.0f, Row.LastCommittedCenterSec - Row.FirstCommittedCenterSec);
        const float MappedSpanSec = FMath::Max(0.0f, Row.MappedAudioEndSec - Row.MappedAudioStartSec);
        const float DetectorSpanSec = Row.bHasDetectorIsland ? FMath::Max(0.0f, Row.DetectorAudioEndSec - Row.DetectorAudioStartSec) : 0.0f;
        const float StartErrorMs = Row.bHasDetectorIsland ? (Row.MappedAudioStartSec - Row.DetectorAudioStartSec) * 1000.0f : 0.0f;
        const float EndErrorMs = Row.bHasDetectorIsland ? (Row.MappedAudioEndSec - Row.DetectorAudioEndSec) * 1000.0f : 0.0f;
        const float DurationErrorMs = Row.bHasDetectorIsland ? (MappedSpanSec - DetectorSpanSec) * 1000.0f : 0.0f;
        const float EventCountModelRatio = DetectorSpanSec > 0.001f ? Row.PlannerProsodyIslandEventCountModelSec / DetectorSpanSec : 0.0f;
        const float SpeechBudgetRatio = DetectorSpanSec > 0.001f ? Row.PlannerProsodyIslandSpeechBudgetSec / DetectorSpanSec : 0.0f;
        const float CommittedSpanRatio = DetectorSpanSec > 0.001f ? CommittedSpanSec / DetectorSpanSec : 0.0f;
        FString Notes;
        if (!Row.bHasDetectorIsland)
        {
            Notes += TEXT("NoDetectorIsland;");
        }
        if (Row.bHasDetectorIsland && FMath::Abs(DurationErrorMs) > 160.0f)
        {
            Notes += TEXT("MappedDurationMismatch;");
        }
        if (Row.bHasDetectorIsland && EventCountModelRatio > 0.0f && (EventCountModelRatio < 0.88f || EventCountModelRatio > 1.12f))
        {
            Notes += TEXT("EventCountPriorMismatch;");
        }
        if (Row.bHasDetectorIsland && SpeechBudgetRatio > 0.0f && (SpeechBudgetRatio < 0.88f || SpeechBudgetRatio > 1.12f))
        {
            Notes += TEXT("SpeechBudgetMismatch;");
        }
        if (Row.bHasDetectorIsland && CommittedSpanRatio > 0.0f && (CommittedSpanRatio < 0.72f || CommittedSpanRatio > 1.05f))
        {
            Notes += TEXT("CenterDistributionMismatch;");
        }

        TArray<FString> Columns;
        Columns.Add(FString::FromInt(Row.TextIslandIndex));
        Columns.Add(FString::FromInt(Row.AudioIslandIndex));
        Columns.Add(FString::FromInt(Row.EventCount));
        Columns.Add(FString::FromInt(Row.FirstEventIndex));
        Columns.Add(FString::FromInt(Row.LastEventIndex));
        Columns.Add(EscapeDebugCSVString(Row.FirstSourceWord));
        Columns.Add(EscapeDebugCSVString(Row.LastSourceWord));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.FirstCommittedCenterSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.LastCommittedCenterSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), CommittedSpanSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.PlannerIslandPredictedDurationSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.PlannerIslandSpeechMaterialSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.PlannerIslandPunctuationSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.PlannerProsodyIslandEventCountModelSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.PlannerProsodyIslandSpeechBudgetSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.PlannerProsodyAllocationScale));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.MappedAudioStartSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Row.MappedAudioEndSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), MappedSpanSec));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.6f"), Row.DetectorAudioStartSec) : TEXT(""));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.6f"), Row.DetectorAudioEndSec) : TEXT(""));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.6f"), DetectorSpanSec) : TEXT(""));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.3f"), StartErrorMs) : TEXT(""));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.3f"), EndErrorMs) : TEXT(""));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.3f"), DurationErrorMs) : TEXT(""));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.6f"), EventCountModelRatio) : TEXT(""));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.6f"), SpeechBudgetRatio) : TEXT(""));
        Columns.Add(Row.bHasDetectorIsland ? FString::Printf(TEXT("%.6f"), CommittedSpanRatio) : TEXT(""));
        Columns.Add(EscapeDebugCSVString(Notes));
        IslandTimingCSV += FString::Join(Columns, TEXT(",")) + TEXT("\r\n");
    }
    FFileHelper::SaveStringToFile(IslandTimingCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("island_timing_diagnostics.csv")));

    const float ObservedBufferDurationSec = LipsyncRuntimeSession ? LipsyncRuntimeSession->GetSpeechDetector().GetObservedAudioBufferEndSec() : 0.0f;

    FString RuntimeCommitCSV = TEXT("LineID,EventIndex,TextIslandIndex,AudioIslandIndex,PoseID,SourceWord,FinalRenderCenterSec,PlaybackSecAtCommit,CommitLeadSec,CommitReason,ObservedAudioEndSec,SpeechIslandStartSec,SpeechIslandEndSec,RequiredActiveElapsedSec,ObservedActiveElapsedSec,ActiveProgressDeficitSec,RequiredProgressNorm,ObservedProgressNorm,ActiveProgressRatio,MappedToObservedSpeech,CommittedTrackEventCount,AudioNudgeEligible,AudioNudgeSearchPerformed,AudioNudgeAccepted,AudioNudgeSearchMode,AudioNudgeReason,AudioNudgeScheduledCenterSec,AudioNudgeCandidateRawCenterSec,AudioNudgeCandidateRawShiftMs,AudioNudgeCandidateConfidence,AudioNudgeRequiredConfidence,AudioNudgeAvailableBeforeSearchMs,AudioNudgeAvailableAudioStartSec,AudioNudgeAvailableAudioEndSec\n");
    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        TArray<FString> Columns;
        Columns.Add(LineString);
        Columns.Add(FString::FromInt(Event.EventIndex));
        Columns.Add(FString::FromInt(Event.TextIslandIndex));
        Columns.Add(FString::FromInt(Event.AudioIslandIndex));
        Columns.Add(Event.PoseID.ToString());
        Columns.Add(EscapeDebugCSVString(Event.SourceWord));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.FinalRenderCenterSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.CommitPlaybackSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.CommitLeadSeconds));
        Columns.Add(Event.CommitReason.ToString());
        Columns.Add(FString::Printf(TEXT("%.6f"), ObservedBufferDurationSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.IslandAudioStartSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.IslandAudioEndSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.RequiredActiveElapsedSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.ObservedActiveElapsedSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.ActiveProgressDeficitSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.RequiredProgressNorm));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.ObservedProgressNorm));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.ActiveProgressRatio));
        Columns.Add(Event.bMappedToObservedSpeech ? TEXT("1") : TEXT("0"));
        Columns.Add(FString::FromInt(ActiveAlignedVisemeTrack.Events.Num()));
        Columns.Add(Event.bAudioNudgeEligible ? TEXT("1") : TEXT("0"));
        Columns.Add(Event.bAudioNudgeSearchPerformed ? TEXT("1") : TEXT("0"));
        Columns.Add(Event.bAudioNudgeAccepted ? TEXT("1") : TEXT("0"));
        Columns.Add(EscapeDebugCSVString(Event.AudioNudgeSearchMode.ToString()));
        Columns.Add(EscapeDebugCSVString(Event.AudioNudgeRejectReason.ToString()));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeScheduledCenterSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeCandidateRawCenterSeconds));
        Columns.Add(FString::Printf(TEXT("%.3f"), Event.AudioNudgeCandidateRawShiftSeconds * 1000.0f));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeCandidateConfidence));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeRequiredConfidence));
        Columns.Add(FString::Printf(TEXT("%.3f"), Event.AudioNudgeAvailableBeforeSearchSeconds * 1000.0f));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeAvailableAudioStartSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Event.AudioNudgeAvailableAudioEndSeconds));
        RuntimeCommitCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    }
    FFileHelper::SaveStringToFile(RuntimeCommitCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("runtime_commit_events.csv")));

    int32 N04EventCount = 0;
    int32 N04MappedCount = 0;
    int32 N04TailDrainCount = 0;
    int32 N04PositiveDeficitCount = 0;
    float N04DeficitSumSec = 0.0f;
    float N04TailDeficitSumSec = 0.0f;
    float N04MaxDeficitSec = 0.0f;
    int32 N04FirstDeficitEventIndex = INDEX_NONE;
    int32 N04LastDeficitEventIndex = INDEX_NONE;
    for (const FOffgridAIAlignedVisemeEvent& Event : ActiveAlignedVisemeTrack.Events)
    {
        ++N04EventCount;
        if (Event.bMappedToObservedSpeech)
        {
            ++N04MappedCount;
        }
        const FString ReasonString = Event.CommitReason.ToString();
        const bool bTailOrFlush = ReasonString == TEXT("M09_live_tail_drain_after_audio") || ReasonString == TEXT("M08_live_tail_drain_after_audio") || ReasonString == TEXT("end_of_stream_flush");
        if (bTailOrFlush)
        {
            ++N04TailDrainCount;
            N04TailDeficitSumSec += Event.ActiveProgressDeficitSeconds;
        }
        if (Event.ActiveProgressDeficitSeconds > 0.001f)
        {
            ++N04PositiveDeficitCount;
            N04DeficitSumSec += Event.ActiveProgressDeficitSeconds;
            N04MaxDeficitSec = FMath::Max(N04MaxDeficitSec, Event.ActiveProgressDeficitSeconds);
            if (N04FirstDeficitEventIndex == INDEX_NONE)
            {
                N04FirstDeficitEventIndex = Event.EventIndex;
            }
            N04LastDeficitEventIndex = Event.EventIndex;
        }
    }
    FString CommitStarvationSummaryCSV = TEXT("Metric,Value\n");
    CommitStarvationSummaryCSV += FString::Printf(TEXT("EventCount,%d\n"), N04EventCount);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("MappedToObservedSpeechCount,%d\n"), N04MappedCount);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("MappedToObservedSpeechRate,%.6f\n"), N04EventCount > 0 ? static_cast<float>(N04MappedCount) / static_cast<float>(N04EventCount) : 0.0f);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("TailDrainOrFinalFlushCount,%d\n"), N04TailDrainCount);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("TailDrainOrFinalFlushRate,%.6f\n"), N04EventCount > 0 ? static_cast<float>(N04TailDrainCount) / static_cast<float>(N04EventCount) : 0.0f);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("PositiveDeficitCount,%d\n"), N04PositiveDeficitCount);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("PositiveDeficitRate,%.6f\n"), N04EventCount > 0 ? static_cast<float>(N04PositiveDeficitCount) / static_cast<float>(N04EventCount) : 0.0f);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("MeanDeficitMs,%.3f\n"), N04EventCount > 0 ? 1000.0f * N04DeficitSumSec / static_cast<float>(N04EventCount) : 0.0f);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("MeanPositiveDeficitMs,%.3f\n"), N04PositiveDeficitCount > 0 ? 1000.0f * N04DeficitSumSec / static_cast<float>(N04PositiveDeficitCount) : 0.0f);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("MeanTailDeficitMs,%.3f\n"), N04TailDrainCount > 0 ? 1000.0f * N04TailDeficitSumSec / static_cast<float>(N04TailDrainCount) : 0.0f);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("MaxDeficitMs,%.3f\n"), 1000.0f * N04MaxDeficitSec);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("FirstDeficitEventIndex,%d\n"), N04FirstDeficitEventIndex);
    CommitStarvationSummaryCSV += FString::Printf(TEXT("LastDeficitEventIndex,%d\n"), N04LastDeficitEventIndex);
    FFileHelper::SaveStringToFile(CommitStarvationSummaryCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("commit_starvation_summary.csv")));

    if (LipsyncRuntimeSession)
    {
        FString AudioOccupancyCSV = TEXT("LineID,UpdateOrdinal,FinalReplay,CurrentPlaybackSec,PrerollSec,SourceEventIndex,Word,PoseID,PlannedCenterSec,CommittedCenterSec,RenderStartSec,RenderEndSec,CommitReason,PlaybackMode,SpeechIslandIndex,SpeechIslandStartSec,SpeechIslandEndSec,AudioActiveSec,TextPlayheadSec,RequiredActiveElapsedSec,ObservedActiveElapsedSec,ActiveProgressDeficitSec,RequiredProgressNorm,ObservedProgressNorm,ActiveProgressRatio,MappedToObservedSpeech,TailDrain,DiagnosticKind\n");
        for (const FOffgridAIAudioOccupancyDiagnosticRow& Row : LipsyncRuntimeSession->GetAudioOccupancyDiagnosticRows())
        {
            TArray<FString> Columns;
            Columns.Add(EscapeDebugCSVString(Row.LineID.ToString()));
            Columns.Add(FString::FromInt(Row.UpdateOrdinal));
            Columns.Add(Row.bFinalReplay ? TEXT("1") : TEXT("0"));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.CurrentPlaybackSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.PrerollSec));
            Columns.Add(FString::FromInt(Row.SourceEventIndex));
            Columns.Add(EscapeDebugCSVString(Row.Word));
            Columns.Add(EscapeDebugCSVString(Row.PoseID.ToString()));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.PlannedCenterSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.CommittedCenterSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.RenderStartSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.RenderEndSec));
            Columns.Add(EscapeDebugCSVString(Row.CommitReason.ToString()));
            Columns.Add(EscapeDebugCSVString(Row.PlaybackMode.ToString()));
            Columns.Add(FString::FromInt(Row.SpeechIslandIndex));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.SpeechIslandStartSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.SpeechIslandEndSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.AudioActiveSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.TextPlayheadSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.RequiredActiveElapsedSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.ObservedActiveElapsedSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.ActiveProgressDeficitSec));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.RequiredProgressNorm));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.ObservedProgressNorm));
            Columns.Add(FString::Printf(TEXT("%.6f"), Row.ActiveProgressRatio));
            Columns.Add(Row.bMappedToObservedSpeech ? TEXT("1") : TEXT("0"));
            Columns.Add(Row.bTailDrain ? TEXT("1") : TEXT("0"));
            Columns.Add(EscapeDebugCSVString(Row.DiagnosticKind.ToString()));
            AudioOccupancyCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
        }
        FFileHelper::SaveStringToFile(AudioOccupancyCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("runtime_audio_occupancy_diagnostics.csv")));

        const FOffgridAIStreamTailDiagnosticRow& Tail = LipsyncRuntimeSession->GetStreamTailDiagnosticRow();
        FString StreamTailDiagnosticsCSV = TEXT("LineID,PCMChunkCount,PCMBytesReceived,PCMSamplesReceived,LastSampleRate,LastNumChannels,LastChunkStartSample,LastChunkEndSample,ObservedAudioBufferEndSec,FirstSpeechAudioBufferStartSec,SpeechIslandCount,InputStreamClosed,DiagnosticKind\n");
        TArray<FString> TailColumns;
        TailColumns.Add(EscapeDebugCSVString(Tail.LineID.ToString()));
        TailColumns.Add(FString::FromInt(Tail.PCMChunkCount));
        TailColumns.Add(FString::Printf(TEXT("%lld"), Tail.PCMBytesReceived));
        TailColumns.Add(FString::Printf(TEXT("%lld"), Tail.PCMSamplesReceived));
        TailColumns.Add(FString::FromInt(Tail.LastSampleRate));
        TailColumns.Add(FString::FromInt(Tail.LastNumChannels));
        TailColumns.Add(FString::Printf(TEXT("%lld"), Tail.LastChunkStartSample));
        TailColumns.Add(FString::Printf(TEXT("%lld"), Tail.LastChunkEndSample));
        TailColumns.Add(FString::Printf(TEXT("%.6f"), Tail.ObservedAudioBufferEndSec));
        TailColumns.Add(FString::Printf(TEXT("%.6f"), Tail.FirstSpeechAudioBufferStartSec));
        TailColumns.Add(FString::FromInt(Tail.SpeechIslandCount));
        TailColumns.Add(Tail.bInputStreamClosed ? TEXT("1") : TEXT("0"));
        TailColumns.Add(EscapeDebugCSVString(Tail.DiagnosticKind.ToString()));
        StreamTailDiagnosticsCSV += FString::Join(TailColumns, TEXT(",")) + TEXT("\n");
        FFileHelper::SaveStringToFile(StreamTailDiagnosticsCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("runtime_stream_tail_diagnostics.csv")));

    }


    // Shared-runtime parity diagnostic: dump the speech islands exactly as the
    // runtime detector sees them. This avoids over-interpreting per-event
    // AudioIslandIndex rows, which only name the launch fragment and may not
    // show merged fragments owned by a text island.
    FString RuntimeSpeechCSV = TEXT("IslandIndex,Started,Ended,AudioBufferStartSec,AudioBufferLastSpeechSec,AudioBufferEndSec,SpanSec,ObservedAudioBufferEndSec\n");
    const TArray<FOffgridAIStreamingSpeechIsland>& RuntimeSpeechIslands = LipsyncRuntimeSession ? LipsyncRuntimeSession->GetSpeechIslands() : EmptySpeechIslands;
    const float RuntimeObservedEndSec = LipsyncRuntimeSession ? LipsyncRuntimeSession->GetSpeechDetector().GetObservedAudioBufferEndSec() : 0.0f;
    for (const FOffgridAIStreamingSpeechIsland& Island : RuntimeSpeechIslands)
    {
        const float IslandEndSec = Island.bEnded && Island.AudioBufferEndSec > Island.AudioBufferStartSec
            ? Island.AudioBufferEndSec
            : FMath::Max(Island.AudioBufferLastSpeechSec, Island.AudioBufferStartSec);
        TArray<FString> Columns;
        Columns.Add(FString::FromInt(Island.IslandIndex));
        Columns.Add(Island.bStarted ? TEXT("1") : TEXT("0"));
        Columns.Add(Island.bEnded ? TEXT("1") : TEXT("0"));
        Columns.Add(FString::Printf(TEXT("%.6f"), Island.AudioBufferStartSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Island.AudioBufferLastSpeechSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), Island.AudioBufferEndSec));
        Columns.Add(FString::Printf(TEXT("%.6f"), FMath::Max(0.0f, IslandEndSec - Island.AudioBufferStartSec)));
        Columns.Add(FString::Printf(TEXT("%.6f"), RuntimeObservedEndSec));
        RuntimeSpeechCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    }
    FFileHelper::SaveStringToFile(RuntimeSpeechCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("runtime_speech_islands.csv")));

    const bool bHasOracleMetrics = BudgeterErrorsSec.Num() > 0 || AlignerErrorsSec.Num() > 0;
    const float BudgeterMAE = bHasOracleMetrics ? MeanAbsMs(BudgeterErrorsSec) : -1.0f;
    const float BudgeterBias = bHasOracleMetrics ? MeanMs(BudgeterErrorsSec) : -1.0f;
    const float BudgeterP90 = bHasOracleMetrics ? P90AbsMs(BudgeterErrorsSec) : -1.0f;
    const float AlignerMAE = bHasOracleMetrics ? MeanAbsMs(AlignerErrorsSec) : -1.0f;
    const float AlignerBias = bHasOracleMetrics ? MeanMs(AlignerErrorsSec) : -1.0f;
    const float AlignerP90 = bHasOracleMetrics ? P90AbsMs(AlignerErrorsSec) : -1.0f;
    const float PerformerMAE = MeanAbsMs(PerformerPeakErrorsSec);
    const float PerformerBias = MeanMs(PerformerPeakErrorsSec);
    const float PerformerP90 = P90AbsMs(PerformerPeakErrorsSec);
    const float WeakPeakRate = PlannedCount > 0 ? static_cast<float>(WeakPeakCount) / static_cast<float>(PlannedCount) : 0.0f;
    const float CommitLeadMeanMs = MeanMs(CommitLeadSec);
    const float CommitLeadP90Ms = P90AbsMs(CommitLeadSec);
    const float CommitHorizonMeanMs = MeanMs(EffectiveCommitHorizonsSec);
    float ScaleMean = 0.0f;
    float ScaleMax = 0.0f;
    int32 ScaleCapHitCount = 0;
    if (EffectiveScales.Num() > 0)
    {
        for (float Scale : EffectiveScales)
        {
            ScaleMean += Scale;
            ScaleMax = FMath::Max(ScaleMax, Scale);
            if (Scale >= 2.245f)
            {
                ++ScaleCapHitCount;
            }
        }
        ScaleMean /= static_cast<float>(EffectiveScales.Num());
    }
    const float ScaleCapHitRate = EffectiveScales.Num() > 0
        ? static_cast<float>(ScaleCapHitCount) / static_cast<float>(EffectiveScales.Num())
        : 0.0f;

    const TArray<FOffgridAIStreamingSpeechIsland>& ScoreSpeechIslands = LipsyncRuntimeSession ? LipsyncRuntimeSession->GetSpeechIslands() : EmptySpeechIslands;
    float DetectedSpeechOnlyDurationSec = 0.0f;
    float DetectedPauseDurationSec = 0.0f;
    int32 DetectedPauseCount = 0;
    float PreviousIslandEndSec = -1.0f;
    for (const FOffgridAIStreamingSpeechIsland& Island : ScoreSpeechIslands)
    {
        if (!Island.bStarted)
        {
            continue;
        }
        const float IslandEndSec = Island.bEnded && Island.AudioBufferEndSec > Island.AudioBufferStartSec
            ? Island.AudioBufferEndSec
            : FMath::Max(Island.AudioBufferLastSpeechSec, Island.AudioBufferStartSec);
        DetectedSpeechOnlyDurationSec += FMath::Max(IslandEndSec - Island.AudioBufferStartSec, 0.0f);
        if (PreviousIslandEndSec >= 0.0f)
        {
            const float PauseSec = FMath::Max(Island.AudioBufferStartSec - PreviousIslandEndSec, 0.0f);
            if (PauseSec >= 0.120f)
            {
                ++DetectedPauseCount;
                DetectedPauseDurationSec += PauseSec;
            }
        }
        PreviousIslandEndSec = FMath::Max(PreviousIslandEndSec, IslandEndSec);
    }
    const float SpeechWithoutCommittedAnimationTailSec = FMath::Max(0.0f, ObservedBufferDurationSec - LastCommittedCenterSec);
    const float SpeechWithoutSubmittedAnimationTailSec = FMath::Max(0.0f, ObservedBufferDurationSec - LastSubmittedPeakPlaybackSec);
    const float AnimationBeforeSpeechLeadSec = FMath::Max(0.0f, LastSubmittedPeakPlaybackSec - ObservedBufferDurationSec);
    const float SpeechVsPauseDurationRatio = DetectedPauseDurationSec > 0.001f
        ? DetectedSpeechOnlyDurationSec / DetectedPauseDurationSec
        : (DetectedSpeechOnlyDurationSec > 0.0f ? 999.0f : 0.0f);

    FString ScoreCSV = TEXT("Subsystem,Metric,Value,Unit,NPCID,LineID,Notes\n");
    auto AddMetric = [&ScoreCSV, &NPCString, &LineString](const TCHAR* Subsystem, const TCHAR* Metric, float Value, const TCHAR* Unit, const TCHAR* Notes)
    {
        TArray<FString> Columns;
        Columns.Add(FString(Subsystem));
        Columns.Add(FString(Metric));
        Columns.Add(FString::Printf(TEXT("%.6f"), Value));
        Columns.Add(FString(Unit));
        Columns.Add(NPCString);
        Columns.Add(LineString);
        Columns.Add(EscapeDebugCSVString(FString(Notes)));
        ScoreCSV += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    };

    AddMetric(TEXT("VisemePlanner"), TEXT("PlanEventCount"), static_cast<float>(PlannedCount), TEXT("events"), TEXT("Objective count only; qualitative scoring is in viseme_plan_review.md"));
    AddMetric(TEXT("VisemePlanner"), TEXT("PlanOrderingViolations"), 0.0f, TEXT("count"), TEXT("Planner output order is implicit in EventIndex; nonzero only if future validation detects disorder"));
    AddMetric(TEXT("Diagnostics"), TEXT("OracleEventCount"), static_cast<float>(BudgeterErrorsSec.Num()), TEXT("events"), TEXT("Number of events with valid audio_plan_alignment.csv oracle rows. Timing MAE fields are -1 when this is zero."));
    AddMetric(TEXT("ArticulationBudgeter"), TEXT("TextTimingMAE"), BudgeterMAE, TEXT("ms"), TEXT("Weak text/audio-warp expected center versus full-WAV oracle BestAudioSec; -1 means no oracle file/rows were available"));
    AddMetric(TEXT("ArticulationBudgeter"), TEXT("TextTimingBias"), BudgeterBias, TEXT("ms"), TEXT("Positive means text prior is late versus oracle; negative means early"));
    AddMetric(TEXT("ArticulationBudgeter"), TEXT("TextTimingP90Abs"), BudgeterP90, TEXT("ms"), TEXT("P90 absolute text-prior timing error"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("CommittedEventCount"), static_cast<float>(CommittedCount), TEXT("events"), TEXT("Events present in ActiveAlignedVisemeTrack"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("MissingCommittedEventCount"), static_cast<float>(MissingCommittedCount), TEXT("events"), TEXT("Should be zero under the liveness invariant"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("MonotonicityViolations"), static_cast<float>(MonotonicViolations), TEXT("count"), TEXT("Should be zero; event centers must increase with EventIndex"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("StreamingTimingMAE"), AlignerMAE, TEXT("ms"), TEXT("Committed playback center versus full-WAV oracle BestAudioSec; -1 means no oracle file/rows were available"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("StreamingLeadBias"), AlignerBias, TEXT("ms"), TEXT("Positive means committed centers are late versus oracle; negative means early"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("StreamingTimingP90Abs"), AlignerP90, TEXT("ms"), TEXT("P90 absolute committed timing error"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("CommitLeadMean"), CommitLeadMeanMs, TEXT("ms"), TEXT("Mean committed center minus playback time at commit; lower means events freeze closer to audible playback"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("CommitLeadP90Abs"), CommitLeadP90Ms, TEXT("ms"), TEXT("P90 absolute commit lead; useful for diagnosing over-eager freezing"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("CommitReason_WithinHorizonCount"), static_cast<float>(CommitReasonWithinHorizonCount), TEXT("events"), TEXT("Events whose final center was within the playback/preroll commit horizon at update time"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("CommitReason_AheadOfHorizonCount"), static_cast<float>(CommitReasonAheadOfHorizonCount), TEXT("events"), TEXT("Events rebuilt ahead of the requested commit horizon; high counts indicate whole-track freezing/rebuild behavior"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("CommitReason_EndOfStreamCount"), static_cast<float>(CommitReasonEndOfStreamCount), TEXT("events"), TEXT("Events annotated during final end-of-stream update"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("CommitReason_FallbackCount"), static_cast<float>(CommitReasonFallbackCount), TEXT("events"), TEXT("Events committed without a strong matched audio island"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("CommitReason_UnknownCount"), static_cast<float>(CommitReasonUnknownCount), TEXT("events"), TEXT("Events with unclassified commit reason"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("EffectiveCommitHorizonMean"), CommitHorizonMeanMs, TEXT("ms"), TEXT("Mean commit horizon used by streaming aligner"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("EffectiveSegmentScaleMean"), ScaleMean, TEXT("ratio"), TEXT("Mean speech-only segment scale applied to committed events"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("EffectiveSegmentScaleMax"), ScaleMax, TEXT("ratio"), TEXT("Max speech-only segment scale applied to committed events"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("ScaleCapHitCount"), static_cast<float>(ScaleCapHitCount), TEXT("events"), TEXT("Events whose effective speech-only segment scale hit the clamp ceiling"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("ScaleCapHitRate"), ScaleCapHitRate, TEXT("ratio"), TEXT("Share of committed events whose effective scale hit the clamp ceiling"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("DetectedSpeechOnlyDuration"), DetectedSpeechOnlyDurationSec * 1000.0f, TEXT("ms"), TEXT("Sum of detected speech-island durations; excludes detected pauses/silence"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("DetectedPauseCount"), static_cast<float>(DetectedPauseCount), TEXT("count"), TEXT("Detected gaps >=120ms between speech islands"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("DetectedPauseDuration"), DetectedPauseDurationSec * 1000.0f, TEXT("ms"), TEXT("Total detected gap duration between speech islands; should affect segment offsets, not articulation scale"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("SpeechVsPauseDurationRatio"), SpeechVsPauseDurationRatio, TEXT("ratio"), TEXT("Speech-only duration divided by detected pause duration; 999 means no detected pauses"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("ObservedAudioBufferDuration"), ObservedBufferDurationSec * 1000.0f, TEXT("ms"), TEXT("Full streamed PCM duration including pre-speech, speech, pauses, and tail"));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("LastCommittedVisemeCenter"), LastCommittedCenterSec * 1000.0f, TEXT("ms"), TEXT("Last committed FinalRenderCenterSeconds. If far before observed audio end, the line exhausts animation early."));
    AddMetric(TEXT("AudioAnchorAligner"), TEXT("SpeechWithoutCommittedAnimationTail"), SpeechWithoutCommittedAnimationTailSec * 1000.0f, TEXT("ms"), TEXT("Observed audio remaining after last committed viseme center. >250ms is an obvious early-exhaustion failure."));
    AddMetric(TEXT("VisemePerformer"), TEXT("LastSubmittedFacePeak"), LastSubmittedPeakPlaybackSec * 1000.0f, TEXT("ms"), TEXT("Last event peak observed in submitted_poses.csv / FaceDriver sampling."));
    AddMetric(TEXT("VisemePerformer"), TEXT("SpeechWithoutSubmittedAnimationTail"), SpeechWithoutSubmittedAnimationTailSec * 1000.0f, TEXT("ms"), TEXT("Observed audio remaining after last submitted FaceDriver peak. >250ms means the face visibly stops before speech ends."));
    AddMetric(TEXT("VisemePerformer"), TEXT("AnimationAfterAudioTail"), AnimationBeforeSpeechLeadSec * 1000.0f, TEXT("ms"), TEXT("Face peaks after observed audio end. Large values indicate late drift rather than early exhaustion."));
    AddMetric(TEXT("VisemePerformer"), TEXT("SubmittedEventPeakCount"), static_cast<float>(SubmittedEventCount), TEXT("events"), TEXT("Unique events with at least one submitted/FaceDriver peak sample"));
    AddMetric(TEXT("VisemePerformer"), TEXT("MissingSubmittedPeakCount"), static_cast<float>(MissingSubmittedCount), TEXT("events"), TEXT("Events with no observed submitted_poses peak sample"));
    AddMetric(TEXT("VisemePerformer"), TEXT("PeakTimingMAE"), PerformerMAE, TEXT("ms"), TEXT("FaceDriver peak playback time versus committed playback center"));
    AddMetric(TEXT("VisemePerformer"), TEXT("PeakLeadBias"), PerformerBias, TEXT("ms"), TEXT("Negative means peak occurs before committed center"));
    AddMetric(TEXT("VisemePerformer"), TEXT("PeakTimingP90Abs"), PerformerP90, TEXT("ms"), TEXT("P90 absolute FaceDriver peak timing error"));
    AddMetric(TEXT("FaceDriver"), TEXT("WeakPeakRate"), WeakPeakRate, TEXT("ratio"), TEXT("Strong planned events whose FaceDriver peak stayed below 0.18"));
    AddMetric(TEXT("FaceDriver"), TEXT("WeakPeakCount"), static_cast<float>(WeakPeakCount), TEXT("events"), TEXT("Proxy for buried/cut-off animations; subjective naturalness still requires video review"));
    FFileHelper::SaveStringToFile(ScoreCSV, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("lipsync_scorecard.csv")));

    FString ReviewMD;
    ReviewMD += TEXT("# Viseme Plan Review\n\n");
    ReviewMD += TEXT("This file is intentionally a manual/AI review scaffold. The planner cannot be scored objectively from audio logs alone; review the transcript word-by-word against planned_events.csv.\n\n");
    ReviewMD += TEXT("## Scores to fill in\n\n");
    ReviewMD += TEXT("- PlanCoverage (1-5): did the plan include the important visible mouth shapes for each word?\n");
    ReviewMD += TEXT("- PlanPlausibility (1-5): do selected visemes make phonetic/perceptual sense?\n");
    ReviewMD += TEXT("- PlanClutter (1-5, higher is cleaner): are there unnecessary micro-visemes or noisy repeats?\n");
    ReviewMD += TEXT("- PlanOrdering (objective): any event-order or word-association errors?\n\n");
    ReviewMD += TEXT("## Planned events\n\n");
    ReviewMD += TEXT("| Event | Word | Pose | TextViseme | Strength | Reviewer notes |\n");
    ReviewMD += TEXT("|---:|---|---|---|---:|---|\n");
    for (int32 EventIndex = 0; EventIndex < PlannedCount; ++EventIndex)
    {
        const FOffgridAITextVisemeEvent& Event = ActiveTextVisemePlan.Events[EventIndex];
        ReviewMD += FString::Printf(TEXT("| %d | %s | %s | %s | %.2f |  |\n"),
            EventIndex,
            *Event.SourceText,
            *Event.PoseID.ToString(),
            *FOffgridAITextVisemePlanner::ToDebugString(Event.Viseme),
            FMath::Clamp(Event.Strength, 0.0f, 1.0f));
    }
    FFileHelper::SaveStringToFile(ReviewMD, *FPaths::Combine(LipsyncDebugLineDirectory, TEXT("viseme_plan_review.md")));
}


void UOffgridAILineCoach::AppendLipsyncDebugSubmittedPosesCSV(float PlaybackSeconds, const TMap<FName, float>& SubmittedWeights, const TMap<FName, float>& FaceDriverWeights) const
{
    if (!bLipsyncDebugFileInitialized || LipsyncDebugLineDirectory.IsEmpty())
    {
        return;
    }

    const FString Path = FPaths::Combine(LipsyncDebugLineDirectory, TEXT("submitted_poses.csv"));
    if (!FPaths::FileExists(Path))
    {
        const FString Header = TEXT("RealTimeSec,NPCID,LineID,PlaybackSec,PoseID,Family,SubmittedWeight,FaceDriverWeight,SurvivedToFaceDriver,SourceEventIndex,SourceWord,CommittedPlaybackStartSec,CommittedPlaybackCenterSec,CommittedPlaybackEndSec,SourceStrength,CommitPlaybackSec,CommitLeadMs,CommitReason,EffectiveSegmentScale,EffectiveCommitHorizonMs,PlannerIslandPredictedDurationSec,PlannerEventPredictedOffsetSec,PlannerProsodyGroupIndex,PlannerProsodyRole,PlannerProsodyGroupAllocatedSec,SourceTimingDomain\n");
        FFileHelper::SaveStringToFile(Header, *Path);
    }

    FString Rows;
    for (const FOffgridAISubmittedVisemeSample& Sample : LastSubmittedVisemeSamples)
    {
        const bool bSubmittedPosePresent = SubmittedWeights.Contains(Sample.PoseID);
        if (!bSubmittedPosePresent)
        {
            continue;
        }

        // Event-instance invariant: debug provenance comes from the submitted sample itself.
        // Never rediscover timing from PoseID; repeated PoseIDs are common and unsafe.
        TArray<FString> Columns;
        const float FaceWeight = FaceDriverWeights.FindRef(Sample.PoseID);
        Columns.Add(FString::Printf(TEXT("%.6f"), FPlatformTime::Seconds()));
        Columns.Add(NPCID.ToString());
        Columns.Add(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None")));
        Columns.Add(FString::Printf(TEXT("%.6f"), PlaybackSeconds));
        Columns.Add(Sample.PoseID.ToString());
        Columns.Add(DebugFamilyForPose(Sample.PoseID));
        Columns.Add(FString::Printf(TEXT("%.6f"), Clamp01(Sample.SubmittedWeight)));
        Columns.Add(FString::Printf(TEXT("%.6f"), Clamp01(FaceWeight)));
        Columns.Add(FaceWeight > KINDA_SMALL_NUMBER ? TEXT("1") : TEXT("0"));
        Columns.Add(FString::FromInt(Sample.EventIndex));
        Columns.Add(EscapeDebugCSVString(Sample.SourceWord));
        Columns.Add(FString::Printf(TEXT("%.6f"), Sample.CommittedRenderStartSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Sample.CommittedRenderCenterSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Sample.CommittedRenderEndSeconds));
        Columns.Add(FString::Printf(TEXT("%.6f"), Sample.SourceStrength));
        const FOffgridAIAlignedVisemeEvent* SourceEvent = ActiveAlignedVisemeTrack.Events.FindByPredicate([&Sample](const FOffgridAIAlignedVisemeEvent& Event)
        {
            return Event.EventIndex == Sample.EventIndex;
        });
        Columns.Add(SourceEvent ? FString::Printf(TEXT("%.6f"), SourceEvent->CommitPlaybackSeconds) : TEXT(""));
        Columns.Add(SourceEvent ? FString::Printf(TEXT("%.3f"), SourceEvent->CommitLeadSeconds * 1000.0f) : TEXT(""));
        Columns.Add(SourceEvent ? FString::Printf(TEXT("%.6f"), SourceEvent->EffectiveSegmentScale) : TEXT(""));
        Columns.Add(SourceEvent ? FString::Printf(TEXT("%.3f"), SourceEvent->EffectiveCommitHorizonSeconds * 1000.0f) : TEXT(""));
        Columns.Add(SourceEvent ? FString::Printf(TEXT("%.6f"), SourceEvent->PlannerIslandPredictedDurationSeconds) : TEXT(""));
        Columns.Add(SourceEvent ? FString::Printf(TEXT("%.6f"), SourceEvent->PlannerEventPredictedOffsetSeconds) : TEXT(""));
        Columns.Add(SourceEvent ? FString::FromInt(SourceEvent->PlannerProsodyGroupIndex) : TEXT(""));
        Columns.Add(SourceEvent ? SourceEvent->PlannerProsodyRole.ToString() : TEXT(""));
        Columns.Add(SourceEvent ? FString::Printf(TEXT("%.6f"), SourceEvent->PlannerProsodyGroupAllocatedSeconds) : TEXT(""));
        Columns.Add(TEXT("CommittedPlayback"));
        Rows += FString::Join(Columns, TEXT(",")) + TEXT("\n");
    }

    // Empty submissions are intentional before speech start / after final event.
    // Do not synthesize InvalidRejected rows from pose maps; that reintroduces pose-keyed provenance.
    if (!Rows.IsEmpty())
    {
        FFileHelper::SaveStringToFile(Rows, *Path, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
    }
}



void UOffgridAILineCoach::AppendLipsyncDebugCSV(const TCHAR* Stage, float FrameTimeSec, const FOffgridAILipsyncFeatureFrame* Features, const FOffgridAILipsyncPoseRuntimeState& Raw, const FOffgridAILipsyncPoseRuntimeState& Smoothed, const FOffgridAILipsyncPoseRuntimeState& Final) const
{
    if (!bLipsyncDebugFileInitialized)
    {
        return;
    }

    const float RMSNorm = Features ? Features->RMSNorm : 0.0f;
    const float PrevRMS = Features ? Features->PreviousRMSNorm : 0.0f;
    const float DeltaRMS = Features ? (Features->RMSNorm - Features->PreviousRMSNorm) : 0.0f;
    const float Voiced = Features ? Features->Voicedness : 0.0f;
    const float Centroid = Features ? Features->CentroidNorm : 0.0f;
    const float Low = Features ? Features->LowRatio : 0.0f;
    const float Mid = Features ? Features->MidRatio : 0.0f;
    const float High = Features ? Features->HighRatio : 0.0f;
    const int32 Transient = (Features && Features->bIsTransient) ? 1 : 0;

    TMap<FName, float> DriverWeights = LastResolvedDriverVisemeWeights;
    float FaceJawOpen = CurrentFacialFrame.JawOpen;
    if (const UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
    {
        const TMap<FName, float> ResolvedDriverWeights = FaceDriver->GetDebugActivePoseWeights();
        if (ResolvedDriverWeights.Num() > 0)
        {
            DriverWeights = ResolvedDriverWeights;
        }
        const FOffgridAIMetaHumanFacePose LatestFacePose = FaceDriver->GetLatestFacePose();
        FaceJawOpen = LatestFacePose.CTRL_C_jaw.Y;
    }

    const float DriverClosedFamily = GetDebugPoseFamilyWeight(DriverWeights, TEXT("Closed"));
    const float DriverOpenFamily = GetDebugPoseFamilyWeight(DriverWeights, TEXT("Open"));
    const float DriverWideFamily = GetDebugPoseFamilyWeight(DriverWeights, TEXT("Wide"));
    const float DriverRoundFamily = GetDebugPoseFamilyWeight(DriverWeights, TEXT("Round"));
    const float DriverFunnelFamily = GetDebugPoseFamilyWeight(DriverWeights, TEXT("Funnel"));
    const float DriverTeethFamily = GetDebugPoseFamilyWeight(DriverWeights, TEXT("Teeth"));
    const float DriverTongueFamily = GetDebugPoseFamilyWeight(DriverWeights, TEXT("Tongue"));
    FName DominantDriverPose = NAME_None;
    float DominantDriverWeight = 0.0f;
    GetDebugDominantPose(DriverWeights, DominantDriverPose, DominantDriverWeight);

    float FaceMouthCornerPull = 0.0f;
    float FaceMouthCornerDepress = 0.0f;
    float FaceMouthStretch = 0.0f;
    float FaceLipPress = 0.0f;
    float FaceLipPurse = 0.0f;
    float FaceLipFunnel = 0.0f;
    float FaceTongue = 0.0f;
    if (const UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
    {
        const FOffgridAIMetaHumanFacePose LatestFacePose = FaceDriver->GetLatestFacePose();
        FaceMouthCornerPull = FMath::Max(LatestFacePose.CTRL_L_mouth_cornerPull, LatestFacePose.CTRL_R_mouth_cornerPull);
        FaceMouthCornerDepress = FMath::Max(LatestFacePose.CTRL_L_mouth_cornerDepress, LatestFacePose.CTRL_R_mouth_cornerDepress);
        FaceMouthStretch = FMath::Max(LatestFacePose.CTRL_L_mouth_stretch, LatestFacePose.CTRL_R_mouth_stretch);
        FaceLipPress = FMath::Max(FMath::Max(LatestFacePose.CTRL_L_mouth_pressU, LatestFacePose.CTRL_R_mouth_pressU), FMath::Max(LatestFacePose.CTRL_L_mouth_pressD, LatestFacePose.CTRL_R_mouth_pressD));
        FaceLipPurse = FMath::Max(FMath::Max(LatestFacePose.CTRL_L_mouth_purseU, LatestFacePose.CTRL_R_mouth_purseU), FMath::Max(LatestFacePose.CTRL_L_mouth_purseD, LatestFacePose.CTRL_R_mouth_purseD));
        FaceLipFunnel = FMath::Max(FMath::Max(LatestFacePose.CTRL_L_mouth_funnelU, LatestFacePose.CTRL_R_mouth_funnelU), FMath::Max(LatestFacePose.CTRL_L_mouth_funnelD, LatestFacePose.CTRL_R_mouth_funnelD));
        FaceTongue = FMath::Max(FMath::Max(FMath::Abs(LatestFacePose.CTRL_C_tongue.X), FMath::Abs(LatestFacePose.CTRL_C_tongue.Y)), FMath::Abs(LatestFacePose.CTRL_C_tongue_press));
    }

    TArray<FString> Columns;
    Columns.Reserve(62);
    Columns.Add(FString::Printf(TEXT("%.6f"), FPlatformTime::Seconds()));
    Columns.Add(NPCID.ToString());
    Columns.Add(bHasActiveLineRequest ? ActiveLineRequest.LineID.ToString() : FString(TEXT("None")));
    Columns.Add(Stage ? FString(Stage) : FString(TEXT("None")));
    const float PCMSpeechActivity = ComputePCMDrivenSpeechActivityAtPlaybackTime(GetCurrentOutputPlaybackSeconds());
    Columns.Add(FString::Printf(TEXT("%.6f"), GetCurrentOutputPlaybackSeconds()));
    Columns.Add(FString::Printf(TEXT("%.6f"), FrameTimeSec));
    Columns.Add(FString::Printf(TEXT("%.6f"), PCMSpeechActivity));
    Columns.Add(FString::Printf(TEXT("%.6f"), bActiveAlignedVisemeTrackBuilt ? ActiveAlignedVisemeTrack.SpeechStartSeconds : -1.0f));
    Columns.Add(FString::Printf(TEXT("%.6f"), bActiveAlignedVisemeTrackBuilt ? ActiveAlignedVisemeTrack.SpeechEndSeconds : -1.0f));
    Columns.Add(FString::Printf(TEXT("%.6f"), RMSNorm));
    Columns.Add(FString::Printf(TEXT("%.6f"), PrevRMS));
    Columns.Add(FString::Printf(TEXT("%.6f"), DeltaRMS));
    Columns.Add(FString::Printf(TEXT("%.6f"), Voiced));
    Columns.Add(FString::Printf(TEXT("%.6f"), Centroid));
    Columns.Add(FString::Printf(TEXT("%.6f"), Low));
    Columns.Add(FString::Printf(TEXT("%.6f"), Mid));
    Columns.Add(FString::Printf(TEXT("%.6f"), High));
    Columns.Add(FString::Printf(TEXT("%d"), Transient));
    Columns.Add(FString::Printf(TEXT("%.6f"), Raw.Closed));
    Columns.Add(FString::Printf(TEXT("%.6f"), Raw.Open));
    Columns.Add(FString::Printf(TEXT("%.6f"), Raw.Wide));
    Columns.Add(FString::Printf(TEXT("%.6f"), Raw.Round));
    Columns.Add(FString::Printf(TEXT("%.6f"), Raw.Funnel));
    Columns.Add(FString::Printf(TEXT("%.6f"), Raw.Teeth));
    Columns.Add(FString::Printf(TEXT("%.6f"), Smoothed.Closed));
    Columns.Add(FString::Printf(TEXT("%.6f"), Smoothed.Open));
    Columns.Add(FString::Printf(TEXT("%.6f"), Smoothed.Wide));
    Columns.Add(FString::Printf(TEXT("%.6f"), Smoothed.Round));
    Columns.Add(FString::Printf(TEXT("%.6f"), Smoothed.Funnel));
    Columns.Add(FString::Printf(TEXT("%.6f"), Smoothed.Teeth));
    Columns.Add(FString::Printf(TEXT("%.6f"), Final.Closed));
    Columns.Add(FString::Printf(TEXT("%.6f"), Final.Open));
    Columns.Add(FString::Printf(TEXT("%.6f"), Final.Wide));
    Columns.Add(FString::Printf(TEXT("%.6f"), Final.Round));
    Columns.Add(FString::Printf(TEXT("%.6f"), Final.Funnel));
    Columns.Add(FString::Printf(TEXT("%.6f"), Final.Teeth));
    while (Columns.Num() < 44)
    {
        Columns.Add(TEXT(""));
    }
    Columns.Add(FString::Printf(TEXT("%.6f"), DriverClosedFamily));
    Columns.Add(FString::Printf(TEXT("%.6f"), DriverOpenFamily));
    Columns.Add(FString::Printf(TEXT("%.6f"), DriverWideFamily));
    Columns.Add(FString::Printf(TEXT("%.6f"), DriverRoundFamily));
    Columns.Add(FString::Printf(TEXT("%.6f"), DriverFunnelFamily));
    Columns.Add(FString::Printf(TEXT("%.6f"), DriverTeethFamily));
    Columns.Add(FString::Printf(TEXT("%.6f"), DriverTongueFamily));
    Columns.Add(DominantDriverPose.ToString());
    Columns.Add(FString::Printf(TEXT("%.6f"), DominantDriverWeight));
    Columns.Add(FString::Printf(TEXT("%.6f"), FaceJawOpen));
    Columns.Add(FString::Printf(TEXT("%.6f"), FaceMouthCornerPull));
    Columns.Add(FString::Printf(TEXT("%.6f"), FaceMouthCornerDepress));
    Columns.Add(FString::Printf(TEXT("%.6f"), FaceMouthStretch));
    Columns.Add(FString::Printf(TEXT("%.6f"), FaceLipPress));
    Columns.Add(FString::Printf(TEXT("%.6f"), FaceLipPurse));
    Columns.Add(FString::Printf(TEXT("%.6f"), FaceLipFunnel));
    Columns.Add(FString::Printf(TEXT("%.6f"), FaceTongue));
    Columns.Add(EscapeDebugCSVString(SerializeDebugPoseWeights(DriverWeights)));

    FString Row = FString::Join(Columns, TEXT(","));
    Row.Append(TEXT("\n"));
    FFileHelper::SaveStringToFile(Row, *GetLipsyncDebugLogPath(), FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
}

void UOffgridAILineCoach::BroadcastFacialFrameIfChanged()
{
    const bool bSameJaw = FMath::IsNearlyEqual(CurrentFacialFrame.JawOpen, LastBroadcastFacialFrame.JawOpen, KINDA_SMALL_NUMBER);
    const bool bSameTeeth = FMath::IsNearlyEqual(CurrentFacialFrame.TeethShow, LastBroadcastFacialFrame.TeethShow, KINDA_SMALL_NUMBER);
    const bool bSameRound = FMath::IsNearlyEqual(CurrentFacialFrame.LipRound, LastBroadcastFacialFrame.LipRound, KINDA_SMALL_NUMBER);
    const bool bSameEmotions = AreEmotionMapsNearlyEqual(CurrentFacialFrame.EmotionWeights, LastBroadcastFacialFrame.EmotionWeights);
    const bool bSameMouthPoses = AreFloatMapsNearlyEqual(CurrentFacialFrame.MouthPoseWeights, LastBroadcastFacialFrame.MouthPoseWeights);
    const bool bSameNPC = CurrentFacialFrame.NPCID == LastBroadcastFacialFrame.NPCID;
    const bool bSameLine = CurrentFacialFrame.LineID == LastBroadcastFacialFrame.LineID;

    if (bSameJaw && bSameTeeth && bSameRound && bSameEmotions && bSameMouthPoses && bSameNPC && bSameLine)
    {
        return;
    }

    LastBroadcastFacialFrame = CurrentFacialFrame;
    OnFacialFrameUpdated.Broadcast(CurrentFacialFrame);
}


void UOffgridAILineCoach::BeginLineFacialState(FName Emotion, float EmotionMagnitude)
{
    InitializeEmotionMapsIfNeeded();

    const FName RequestedEmotion = NormalizeEmotionName(Emotion);
    const float ResolvedMagnitude = FMath::Clamp(EmotionMagnitude, 0.0f, 1.0f);

    if (!bEnableFacialEmotionExpression)
    {
        ActiveLineEmotion = NAME_None;
        ActiveLineEmotionMagnitude = 0.0f;
        CurrentFacialFrame.NPCID = NPCID;
        CurrentFacialFrame.LineID = bHasActiveLineRequest ? ActiveLineRequest.LineID : NAME_None;
        CurrentFacialFrame.EmotionWeights.Reset();
        BroadcastFacialFrameIfChanged();

        if (UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
        {
            FaceDriver->SetLineSpeechMouthSuppressionActive(false);
            FaceDriver->ClearEmotion();
        }

        UE_LOG(LogOffgridAI, Log,
            TEXT("[LineCoach][Emotion] BeginLineFacialState npc=%s disabled_by_linecoach=true requested=%s magnitude=%.3f"),
            *NPCID.ToString(),
            *RequestedEmotion.ToString(),
            ResolvedMagnitude);
        return;
    }

    bool bSupportedLineEmotion = false;
    for (const FName SupportedEmotion : CachedSupportedEmotions)
    {
        if (NormalizeEmotionName(SupportedEmotion).ToString().Equals(RequestedEmotion.ToString(), ESearchCase::IgnoreCase))
        {
            bSupportedLineEmotion = true;
            break;
        }
    }

    const bool bHasExpressiveEmotion =
        bSupportedLineEmotion &&
        RequestedEmotion != NAME_None &&
        RequestedEmotion != TEXT("neutral") &&
        ResolvedMagnitude > 0.001f;

    ActiveLineEmotion = bHasExpressiveEmotion ? RequestedEmotion : NAME_None;
    ActiveLineEmotionMagnitude = bHasExpressiveEmotion ? ResolvedMagnitude : 0.0f;

    CurrentFacialFrame.NPCID = NPCID;
    CurrentFacialFrame.LineID = bHasActiveLineRequest ? ActiveLineRequest.LineID : NAME_None;
    CurrentFacialFrame.EmotionWeights.Reset();
    if (bHasExpressiveEmotion)
    {
        CurrentFacialFrame.EmotionWeights.Add(ActiveLineEmotion, ActiveLineEmotionMagnitude);
    }

    UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver();

    UE_LOG(LogOffgridAI, Log,
        TEXT("[LineCoach][Emotion] BeginLineFacialState npc=%s emotion=%s magnitude=%.3f supported=%s face_driver=%s source=ConversationManager"),
        *NPCID.ToString(),
        *RequestedEmotion.ToString(),
        ResolvedMagnitude,
        bSupportedLineEmotion ? TEXT("true") : TEXT("false"),
        FaceDriver ? *FaceDriver->GetName() : TEXT("<none>"));

    ApplyEmotionMouthAllowanceSettingsToFaceDriver(FaceDriver);
    if (FaceDriver)
    {
        FaceDriver->SetLineSpeechMouthSuppressionActive(true);
    }
}

void UOffgridAILineCoach::SubmitDrivenEmotionExpression(FName Emotion, float EmotionMagnitude)
{
    InitializeEmotionMapsIfNeeded();

    const FName RequestedEmotion = NormalizeEmotionName(Emotion);
    const float ResolvedMagnitude = FMath::Clamp(EmotionMagnitude, 0.0f, 1.0f);

    if (!bEnableFacialEmotionExpression)
    {
        ActiveLineEmotion = NAME_None;
        ActiveLineEmotionMagnitude = 0.0f;
        CurrentFacialFrame.NPCID = NPCID;
        CurrentFacialFrame.LineID = bHasActiveLineRequest ? ActiveLineRequest.LineID : NAME_None;
        CurrentFacialFrame.EmotionWeights.Reset();
        BroadcastFacialFrameIfChanged();

        if (UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
        {
            FaceDriver->SetLineSpeechMouthSuppressionActive(false);
            FaceDriver->ClearEmotion();
        }
        return;
    }

    bool bSupportedEmotion = false;
    for (const FName SupportedEmotion : CachedSupportedEmotions)
    {
        if (NormalizeEmotionName(SupportedEmotion).ToString().Equals(RequestedEmotion.ToString(), ESearchCase::IgnoreCase))
        {
            bSupportedEmotion = true;
            break;
        }
    }

    const bool bHasExpressiveEmotion =
        bSupportedEmotion &&
        RequestedEmotion != NAME_None &&
        RequestedEmotion != TEXT("neutral") &&
        ResolvedMagnitude > 0.001f;

    ActiveLineEmotion = bHasExpressiveEmotion ? RequestedEmotion : NAME_None;
    ActiveLineEmotionMagnitude = bHasExpressiveEmotion ? ResolvedMagnitude : 0.0f;

    CurrentFacialFrame.NPCID = NPCID;
    CurrentFacialFrame.LineID = bHasActiveLineRequest ? ActiveLineRequest.LineID : NAME_None;
    CurrentFacialFrame.EmotionWeights.Reset();
    if (bHasExpressiveEmotion)
    {
        CurrentFacialFrame.EmotionWeights.Add(ActiveLineEmotion, ActiveLineEmotionMagnitude);
    }
    BroadcastFacialFrameIfChanged();

    UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver();
    ApplyEmotionMouthAllowanceSettingsToFaceDriver(FaceDriver);
    if (!FaceDriver)
    {
        return;
    }

    if (bHasExpressiveEmotion)
    {
        FaceDriver->SubmitEmotion(ActiveLineEmotion, ActiveLineEmotionMagnitude, 1.0f);
    }
    else
    {
        FaceDriver->ClearEmotion();
    }
}

void UOffgridAILineCoach::EndLineFacialState()
{
    InitializeEmotionMapsIfNeeded();

    if (!bEnableFacialEmotionExpression)
    {
        bDriverVisemeSubmissionEnabled = false;
        LastResolvedDriverVisemeWeights.Reset();
        LastSubmittedVisemeSamples.Reset();
        LastResolvedDriverVisemePlaybackSeconds = -1.0;
        ActiveLineEmotion = NAME_None;
        ActiveLineEmotionMagnitude = 0.0f;
        CurrentFacialFrame.EmotionWeights.Reset();

        if (UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
        {
            FaceDriver->ClearVisemes();
            FaceDriver->SetLineSpeechMouthSuppressionActive(false);
            FaceDriver->ClearEmotion();
        }

        UE_LOG(LogOffgridAI, Log,
            TEXT("[LineCoach][Emotion] EndLineFacialState npc=%s disabled_by_linecoach=true clearing_visemes=true"),
            *NPCID.ToString());
        return;
    }

    // After audible line completion, LineCoach must stop submitting any direct
    // viseme targets. The FaceDriver owns the release envelope from this point;
    // continuing to send stale LastResolvedDriverVisemeWeights can override
    // ClearVisemes() and leave jaw/funnel controls stuck.
    bDriverVisemeSubmissionEnabled = false;
    LastResolvedDriverVisemeWeights.Reset();
    LastSubmittedVisemeSamples.Reset();
    LastResolvedDriverVisemePlaybackSeconds = -1.0;

    if (UOffgridAIMetaHumanFaceDriverComponent* FaceDriver = GetFaceDriver())
    {
        UE_LOG(LogOffgridAI, Log,
            TEXT("[LineCoach][Emotion] EndLineFacialState npc=%s clearing_visemes=true active_emotion=%s active_magnitude=%.3f state_owner=ConversationManager"),
            *NPCID.ToString(),
            ActiveLineEmotion != NAME_None ? *ActiveLineEmotion.ToString() : TEXT("<none>"),
            ActiveLineEmotionMagnitude);

        ApplyEmotionMouthAllowanceSettingsToFaceDriver(FaceDriver);
        FaceDriver->SetLineSpeechMouthSuppressionActive(false);

        // Stop submitting viseme targets, but leave the current expression target
        // alone. ConversationManager supplies the next emotional magnitude on the
        // next line; LineCoach does not maintain or restore persistent emotion.
        FaceDriver->ClearVisemes();
    }
}
void UOffgridAILineCoach::ApplyEmotionMouthAllowanceSettingsToFaceDriver(UOffgridAIMetaHumanFaceDriverComponent* FaceDriver) const
{
    if (!FaceDriver)
    {
        return;
    }

    bool bShouldOverride = bOverrideFaceDriverEmotionMouthAllowance;
    float SpeechControlScale = EmotionSpeechControlScaleDuringSpeech;
    float MouthCornerScale = EmotionMouthCornerScaleDuringSpeech;
    float MouthCornerBilabialScale = EmotionMouthCornerScaleDuringBilabial;
    float SharedMouthScale = EmotionSharedMouthScaleDuringSpeech;
    float SpeechHold = EmotionSpeechHoldSeconds;
    float MouthFadeIn = EmotionMouthFadeInSeconds;
    float MouthFadeOut = EmotionMouthFadeOutSeconds;
    float FullMouthAfterSilence = EmotionFullMouthAfterSilenceSeconds;

    if (EmotionSettingsAsset && EmotionSettingsAsset->bOverrideFaceDriverEmotionMouthAllowance)
    {
        bShouldOverride = true;
        SpeechControlScale = EmotionSettingsAsset->EmotionSpeechControlScaleDuringSpeech;
        MouthCornerScale = EmotionSettingsAsset->EmotionMouthCornerScaleDuringSpeech;
        MouthCornerBilabialScale = EmotionSettingsAsset->EmotionMouthCornerScaleDuringBilabial;
        SharedMouthScale = EmotionSettingsAsset->EmotionSharedMouthScaleDuringSpeech;
        SpeechHold = EmotionSettingsAsset->EmotionSpeechHoldSeconds;
        MouthFadeIn = EmotionSettingsAsset->EmotionMouthFadeInSeconds;
        MouthFadeOut = EmotionSettingsAsset->EmotionMouthFadeOutSeconds;
        FullMouthAfterSilence = EmotionSettingsAsset->EmotionFullMouthAfterSilenceSeconds;
    }

    // Experiment C: force the intended emotion/lipsync coexistence profile for
    // this branch even if older LineCoachAudioSettings assets still contain the
    // lipsync-isolation defaults. Mouth emotion is allowed during speech, but only
    // as a light contribution; lipsync remains dominant.
    bShouldOverride = true;
    // More expressive coexistence profile: lipsync still owns hard closures and
    // phoneme timing, but emotion keeps enough lower-face influence during speech
    // that high-intensity expressions do not look sedated. Post-speech recovery is
    // intentionally short and smoothed to avoid a visible mouth snap.
    SpeechControlScale = 0.12f;
    MouthCornerScale = 0.45f;
    MouthCornerBilabialScale = 0.10f;
    SharedMouthScale = 0.28f;
    SpeechHold = 0.10f;
    MouthFadeIn = 0.45f;
    MouthFadeOut = 0.08f;
    FullMouthAfterSilence = 0.55f;

    if (!bShouldOverride)
    {
        return;
    }

    FaceDriver->ConfigureEmotionMouthAllowance(
        SpeechControlScale,
        MouthCornerScale,
        MouthCornerBilabialScale,
        SharedMouthScale,
        SpeechHold,
        MouthFadeIn,
        MouthFadeOut,
        FullMouthAfterSilence);

    if (EmotionSettingsAsset)
    {
        FaceDriver->EmotionBlendInSeconds = FMath::Clamp(EmotionSettingsAsset->EmotionBlendInSeconds, 0.001f, 3.0f);
        FaceDriver->EmotionBlendOutSeconds = FMath::Clamp(EmotionSettingsAsset->EmotionBlendOutSeconds, 0.001f, 3.0f);
        FaceDriver->EmotionFamilyTransitionBlendSeconds = FMath::Clamp(EmotionSettingsAsset->EmotionFamilyTransitionBlendSeconds, 0.001f, 10.0f);
    }

    // Experiment C3: use the same mouth-masked emotion/lipsync behavior as
    // Experiment C, but make emotion acquisition/family changes visibly settle
    // in roughly one second. StepBlend uses an exponential time constant, so
    // 0.35s reaches ~94% of target after ~1s without a hard pose pop.
    FaceDriver->EmotionBlendInSeconds = 0.35f;
    FaceDriver->EmotionBlendOutSeconds = 0.35f;
    FaceDriver->EmotionFamilyTransitionBlendSeconds = 0.35f;
}

UOffgridAIMetaHumanFaceDriverComponent* UOffgridAILineCoach::GetFaceDriver() const
{
    if (CachedFaceDriver)
    {
        return CachedFaceDriver;
    }

    return GetOwner() ? GetOwner()->FindComponentByClass<UOffgridAIMetaHumanFaceDriverComponent>() : nullptr;
}

float UOffgridAILineCoach::BlendScalar(float CurrentValue, float TargetValue, float DeltaTime, float AttackSpeed, float ReleaseSpeed) const
{
    const float Speed = (TargetValue > CurrentValue) ? AttackSpeed : ReleaseSpeed;
    return FMath::FInterpTo(CurrentValue, TargetValue, DeltaTime, FMath::Max(Speed, 0.001f));
}

UOffgridAIOrchestrator* UOffgridAILineCoach::GetOrchestrator() const
{
    if (const UWorld* World = GetWorld())
    {
        if (UGameInstance* GI = World->GetGameInstance())
        {
            return GI->GetSubsystem<UOffgridAIOrchestrator>();
        }
    }

    return nullptr;
}

void UOffgridAILineCoach::InitializeEmotionMapsIfNeeded()
{
    if (bEmotionMapsInitialized)
    {
        return;
    }

    CachedSupportedEmotions = GetSupportedEmotionNames();
    bEmotionMapsInitialized = true;
}

FOffgridAIPADSState UOffgridAILineCoach::GetStartingPADSState() const
{
    FOffgridAIPADSState State;
    State.Pleasure = FMath::Clamp(StartingPleasure, -1.0f, 1.0f);
    State.Activation = FMath::Clamp(StartingActivation, -1.0f, 1.0f);
    State.Dominance = FMath::Clamp(StartingDominance, -1.0f, 1.0f);
    State.Stability = FMath::Clamp(StartingStability, 0.0f, 1.0f);
    return State;
}

TArray<FName> UOffgridAILineCoach::GetConfiguredSupportedEmotionNames() const
{
    return GetSupportedEmotionNames();
}

const TArray<FName>& UOffgridAILineCoach::GetSupportedEmotionNames() const
{
    if (CachedSupportedEmotions.Num() > 0)
    {
        return CachedSupportedEmotions;
    }

    const UOffgridAIEmotionSettingsDataAsset* ResolvedEmotionSettingsAsset = EmotionSettingsAsset.Get();
    if (ResolvedEmotionSettingsAsset && ResolvedEmotionSettingsAsset->SupportedEmotions.Num() > 0)
    {
        return ResolvedEmotionSettingsAsset->SupportedEmotions;
    }

    static const TArray<FName> DefaultEmotions =
    {
        TEXT("happy"),
        TEXT("sad"),
        TEXT("fearful"),
        TEXT("angry"),
        TEXT("surprised")
    };

    return DefaultEmotions;
}

void UOffgridAILineCoach::EnsurePlaybackObjects(int32 SampleRate, int32 NumChannels)
{
    const bool bNeedsRebuild =
        !ProceduralSoundWave ||
        !OutputAudioComponent ||
        ActiveSampleRate != SampleRate ||
        ActiveNumChannels != NumChannels;

    if (!bNeedsRebuild)
    {
        if (OutputAudioComponent)
        {
            OutputAudioComponent->SetRelativeLocation(GetConfiguredAudioSourceOffset());
            OutputAudioComponent->bAllowSpatialization = IsSpatializationEnabled();
            OutputAudioComponent->SetVolumeMultiplier(GetPlaybackVolumeMultiplier());
        }

        return;
    }

    TeardownPlaybackObjects();

    ActiveSampleRate = SampleRate;
    ActiveNumChannels = NumChannels;

    ProceduralSoundWave = NewObject<USoundWaveProcedural>(this);
    ProceduralSoundWave->SetSampleRate(SampleRate);
    ProceduralSoundWave->NumChannels = NumChannels;
    ProceduralSoundWave->Duration = INDEFINITELY_LOOPING_DURATION;
    ProceduralSoundWave->SoundGroup = SOUNDGROUP_Voice;
    ProceduralSoundWave->bLooping = false;
    ProceduralSoundWave->bCanProcessAsync = true;

    AActor* OwnerActor = GetOwner();
    if (!OwnerActor)
    {
        return;
    }

    OutputAudioComponent = NewObject<UAudioComponent>(OwnerActor);
    OutputAudioComponent->bAutoActivate = false;
    OutputAudioComponent->bIsUISound = false;
    OutputAudioComponent->bAllowSpatialization = IsSpatializationEnabled();
    OutputAudioComponent->RegisterComponent();

    if (USceneComponent* RootComponent = OwnerActor->GetRootComponent())
    {
        OutputAudioComponent->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    }

    OutputAudioComponent->SetRelativeLocation(GetConfiguredAudioSourceOffset());
    OutputAudioComponent->SetVolumeMultiplier(GetPlaybackVolumeMultiplier());
    OutputAudioComponent->SetSound(ProceduralSoundWave);
}

void UOffgridAILineCoach::TeardownPlaybackObjects()
{
    if (OutputAudioComponent)
    {
        OutputAudioComponent->Stop();
        OutputAudioComponent->DestroyComponent();
        OutputAudioComponent = nullptr;
    }

    ProceduralSoundWave = nullptr;
    ActiveSampleRate = 0;
    ActiveNumChannels = 0;
    QueuedOutputBytes = 0;
    SubmittedOutputBytes = 0;
    ReceivedOutputChunkCount = 0;
    SubmittedOutputChunkCount = 0;
    PlaybackStartedAfterWindowIndex = 0;
    LastSubmittedChunkStartSample = 0;
    LastSubmittedChunkEndSample = 0;
    OutputPlaybackStartTimeSeconds = 0.0;
    OutputDrainZeroSinceSeconds = 0.0;
    bOutputStreamOpen = false;
}

void UOffgridAILineCoach::SchedulePlaybackDrainCheck(float DelaySeconds)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().SetTimer(
            PlaybackCompletionTimerHandle,
            this,
            &UOffgridAILineCoach::HandlePlaybackFinished,
            FMath::Max(DelaySeconds, 0.01f),
            false);
    }
}

bool UOffgridAILineCoach::HasPendingOrBufferedOutputAudio() const
{
    return GetEstimatedBufferedPlaybackBytes() > 0;
}

void UOffgridAILineCoach::HandlePlaybackFinished()
{
    FlushPendingOutputPCM(true);

    const int64 BufferedBytes = GetEstimatedBufferedPlaybackBytes();
    const int32 BytesPerSecond = GetBytesPerSecond();
    const double SubmittedDurationSeconds = BytesPerSecond > 0
        ? static_cast<double>(QueuedOutputBytes) / static_cast<double>(BytesPerSecond)
        : 0.0;
    const double PlaybackSeconds = GetCurrentOutputPlaybackSeconds();
    // The sample-consumption playback clock is intentionally capped at the line
    // duration once the procedural queue drains. Do not add PlaybackCompletionTailSeconds
    // to this comparison or completion can wait forever at exactly SubmittedDurationSeconds.
    // The tail is only a facial decay affordance; audible completion is reached when
    // all submitted PCM has drained from the procedural wave and the consumed-sample
    // clock has reached the submitted duration.
    const double RequiredAudibleDrainSeconds = SubmittedDurationSeconds;

    if (BufferedBytes > 0 || PlaybackSeconds + 0.005 < RequiredAudibleDrainSeconds)
    {
        OutputDrainZeroSinceSeconds = 0.0;
        UE_LOG(LogOffgridAI, Verbose, TEXT("LineCoach drain pending npc=%s line=%s buffered_bytes=%lld playback=%.3f required=%.3f queued_bytes=%lld tail=%.3f"),
            *NPCID.ToString(),
            *ActiveLineRequest.LineID.ToString(),
            BufferedBytes,
            PlaybackSeconds,
            RequiredAudibleDrainSeconds,
            QueuedOutputBytes,
            PlaybackCompletionTailSeconds);
        if (BufferedBytes > 0)
        {
            StartOutputPlaybackIfReady(true);
        }

        const float NextCheckSeconds = (BufferedBytes > 0)
            ? 0.03f
            : FMath::Clamp(static_cast<float>(RequiredAudibleDrainSeconds - PlaybackSeconds), 0.03f, 0.10f);
        SchedulePlaybackDrainCheck(NextCheckSeconds);
        return;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    if (OutputDrainZeroSinceSeconds <= 0.0)
    {
        OutputDrainZeroSinceSeconds = NowSeconds;
    }

    const float PostDrainHoldSeconds = GetPlaybackPostDrainHoldSeconds();
    if (PostDrainHoldSeconds > 0.0f && NowSeconds - OutputDrainZeroSinceSeconds < static_cast<double>(PostDrainHoldSeconds))
    {
        SchedulePlaybackDrainCheck(FMath::Clamp(PostDrainHoldSeconds - static_cast<float>(NowSeconds - OutputDrainZeroSinceSeconds), 0.02f, 0.08f));
        return;
    }

    // At this point all PCM that LineCoach received has been submitted to the
    // procedural wave, the procedural source queue has drained, the sample-consumption
    // playback clock has reached the submitted duration, and the post-drain grace
    // period has elapsed. Do NOT call Stop() or ResetAudio() here. The Unreal audio
    // mixer may still have the last frames in its render/device buffers even after
    // USoundWaveProcedural reports zero available source bytes. Stopping/resetting
    // here can audibly chop the tail. Let the component underrun/stop naturally; the
    // next BeginOutputAudioStream() already stops and resets the component before
    // queuing a new line.

    PendingOutputPCM.Reset();
    PendingOutputPCMReadOffset = 0;
    SubmittedOutputBytes = 0;
    QueuedOutputBytes = 0;
    bOutputPlaybackStarted = false;
    bOutputPlaybackPausedForUnderrun = false;
    OutputPlaybackStartTimeSeconds = 0.0;
    OutputPlaybackResumeTimeSeconds = 0.0;
    ConsumedPlaybackTimeSeconds = 0.0;
    OutputDrainZeroSinceSeconds = 0.0;

    WriteLipsyncDebugScorecards();

    EndLineFacialState();

    // Audible completion must not leave the mouth twitching between lines. Snap
    // the target to a tiny neutral closure and let the per-pose output dynamics
    // release over a very short window on the next ticks.
    LipsyncPoseState = FOffgridAILipsyncPoseRuntimeState();
    TargetDisplayedLipsyncPoseState = FOffgridAILipsyncPoseRuntimeState();
    TargetDisplayedLipsyncPoseState.Closed = GetLipsyncSettingFloat(&UOffgridAILipsyncSettingsDataAsset::TextPlanRestClosedWeight, 0.035f);
    CurrentDisplayedLipsyncPoseState.Open = FMath::Min(CurrentDisplayedLipsyncPoseState.Open, 0.08f);
    CurrentDisplayedLipsyncPoseState.Wide = FMath::Min(CurrentDisplayedLipsyncPoseState.Wide, 0.08f);
    CurrentDisplayedLipsyncPoseState.Round = FMath::Min(CurrentDisplayedLipsyncPoseState.Round, 0.08f);
    CurrentDisplayedLipsyncPoseState.Funnel = FMath::Min(CurrentDisplayedLipsyncPoseState.Funnel, 0.06f);
    CurrentDisplayedLipsyncPoseState.Teeth = FMath::Min(CurrentDisplayedLipsyncPoseState.Teeth, 0.06f);
    bHasDisplayedLipsyncTarget = true;
    CurrentFacialFrame.NPCID = NPCID;
    CurrentFacialFrame.LineID = NAME_None;

    if (bHasActiveLineRequest)
    {
        const FOffgridAILinePerformanceRequest CompletedLineRequest = ActiveLineRequest;
        bHasActiveLineRequest = false;
        ActiveLineRequest = FOffgridAILinePerformanceRequest();

        UE_LOG(LogOffgridAI, Log, TEXT("LineCoach audible line complete npc=%s line=%s"),
            *NPCID.ToString(),
            *CompletedLineRequest.LineID.ToString());

        if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
        {
            Orchestrator->NotifyLinePerformanceCompleted(CompletedLineRequest.ConversationID, CompletedLineRequest.LineID);
        }
    }
}

FVector UOffgridAILineCoach::GetConfiguredAudioSourceOffset() const
{
    return LineCoachAudioSettingsAsset ? LineCoachAudioSettingsAsset->AudioSourceOffset : DefaultAudioSourceOffset;
}

bool UOffgridAILineCoach::IsSpatializationEnabled() const
{
    return LineCoachAudioSettingsAsset ? LineCoachAudioSettingsAsset->bEnableSpatialization : DefaultEnableSpatialization;
}

float UOffgridAILineCoach::GetPlaybackVolumeMultiplier() const
{
    return LineCoachAudioSettingsAsset ? LineCoachAudioSettingsAsset->PlaybackVolumeMultiplier : DefaultPlaybackVolumeMultiplier;
}


