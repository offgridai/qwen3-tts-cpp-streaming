#include "Lipsync/OffgridAIArticulationBudgeter.h"

namespace
{
    static bool IsStrongReadablePose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        return S == TEXT("12_Ww-Oo-") || S == TEXT("16_Ww-Ew-") || S == TEXT("22_MBP") ||
            S == TEXT("20_FV") || S == TEXT("19_FV-Or-") || S == TEXT("21_FV-Ee-") ||
            S == TEXT("11_Oo") || S == TEXT("03_Ee") || S == TEXT("05_Ay") ||
            S == TEXT("06_Eh") || S == TEXT("07_Aa") || S == TEXT("08_Ah") || S == TEXT("09_Oh");
    }

    static float MinimumReadableStrength(FName PoseID)
    {
        const FString S = PoseID.ToString();
        if (S == TEXT("12_Ww-Oo-") || S == TEXT("16_Ww-Ew-")) { return 0.98f; }
        if (S == TEXT("22_MBP")) { return 0.99f; }
        if (S == TEXT("20_FV") || S == TEXT("19_FV-Or-") || S == TEXT("21_FV-Ee-")) { return 0.97f; }
        if (S == TEXT("11_Oo") || S == TEXT("10_Or")) { return 0.94f; }
        if (S == TEXT("03_Ee")) { return 0.86f; }
        if (S == TEXT("05_Ay")) { return 0.84f; }
        if (S == TEXT("06_Eh")) { return 0.74f; }
        if (S == TEXT("07_Aa") || S == TEXT("08_Ah")) { return 0.86f; }
        if (S == TEXT("09_Oh")) { return 0.82f; }
        if (S == TEXT("18_Uh")) { return 0.62f; }
        return 0.0f;
    }


    static float SalientOwnershipStrength(FName PoseID)
    {
        const FString S = PoseID.ToString();
        // J5: selected salient events should be allowed to fully own the mouth.
        // This is not downstream rescue; it is an explicit budget decision before
        // timing/performance. Small/transient phones remain below full ownership.
        if (S == TEXT("22_MBP")) { return 1.00f; }
        if (S == TEXT("12_Ww-Oo-") || S == TEXT("16_Ww-Ew-")) { return 1.00f; }
        if (S == TEXT("20_FV") || S == TEXT("19_FV-Or-") || S == TEXT("21_FV-Ee-")) { return 1.00f; }
        if (S == TEXT("11_Oo") || S == TEXT("10_Or")) { return 1.00f; }
        if (S == TEXT("03_Ee")) { return 0.94f; }
        if (S == TEXT("05_Ay")) { return 0.92f; }
        if (S == TEXT("07_Aa") || S == TEXT("08_Ah")) { return 0.94f; }
        if (S == TEXT("09_Oh")) { return 0.90f; }
        if (S == TEXT("06_Eh")) { return 0.82f; }
        if (S == TEXT("18_Uh")) { return 0.68f; }
        return MinimumReadableStrength(PoseID);
    }
}

FOffgridAITextVisemePlan FOffgridAIArticulationBudgeter::Budget(const FOffgridAITextVisemePlan& InPlan)
{
    FOffgridAITextVisemePlan Out = InPlan;

    // Strength-only pass. Duration is owned by the text timing estimator and
    // phrase/island launch is owned by the streaming audio aligner.

    for (FOffgridAITextVisemeEvent& Event : Out.Events)
    {
        Event.Strength = FMath::Clamp(Event.Strength, 0.0f, 1.0f);

        // Budgeter's only job is perceptual strength. Strong mouth landmarks must
        // be authored strongly enough for the FaceDriver to render directly. This
        // replaces the old LineCoach-side hidden calibration budget.
        if (Event.bIsLandmark || Event.bIsDominant)
        {
            Event.Strength = FMath::Max(Event.Strength, SalientOwnershipStrength(Event.PoseID));
        }
        else if (IsStrongReadablePose(Event.PoseID) && Event.Strength >= 0.55f)
        {
            Event.Strength = FMath::Max(Event.Strength, MinimumReadableStrength(Event.PoseID));
        }

        // Version J2 perceptual cleanup: if an event survived planning/budgeting
        // and is already moderately strong, push it to a more readable floor.
        // Only landmark/dominant events receive near/full ownership above.
        if (IsStrongReadablePose(Event.PoseID) && Event.Strength >= 0.55f)
        {
            Event.Strength = FMath::Max(Event.Strength, MinimumReadableStrength(Event.PoseID));
        }

        // True TH/T-front is useful but visually noisy. Keep it modest; L-liquid
        // generation has been removed in the planner, so this only affects true T/TH.
        if (Event.PoseID == TEXT("24_Tongue_Th"))
        {
            Event.Strength = FMath::Clamp(Event.Strength, 0.0f, 0.42f);
        }
    }

    Out.Layer1Diagnostics.StageCounts.CandidateCount = InPlan.Events.Num();
    Out.Layer1Diagnostics.StageCounts.TimedCandidateCount = InPlan.Events.Num();
    Out.Layer1Diagnostics.StageCounts.FinalEventCount = Out.Events.Num();
    Out.Layer1Diagnostics.CompressionRatio = InPlan.Events.Num() > 0
        ? static_cast<float>(Out.Events.Num()) / static_cast<float>(InPlan.Events.Num())
        : 1.0f;
    return Out;
}
