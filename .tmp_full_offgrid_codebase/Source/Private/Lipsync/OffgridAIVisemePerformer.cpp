#include "Lipsync/OffgridAIVisemePerformer.h"

namespace
{
    static float SmoothStep01(float X)
    {
        const float T = FMath::Clamp(X, 0.0f, 1.0f);
        return T * T * (3.0f - 2.0f * T);
    }

    static float SmootherStep01(float X)
    {
        const float T = FMath::Clamp(X, 0.0f, 1.0f);
        return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f);
    }

    static bool IsPose(FName PoseID, const TCHAR* Literal)
    {
        return PoseID == FName(Literal);
    }

    static bool IsDiphthongGlideLeadIn(const FOffgridAIAlignedVisemeTrack& Track, int32 EventArrayIndex)
    {
        if (!Track.Events.IsValidIndex(EventArrayIndex) || !Track.Events.IsValidIndex(EventArrayIndex + 1)) { return false; }
        const FOffgridAIAlignedVisemeEvent& E = Track.Events[EventArrayIndex];
        const FOffgridAIAlignedVisemeEvent& Next = Track.Events[EventArrayIndex + 1];
        const FString W = E.SourceWord.ToLower();
        if (!(W == TEXT("how") || W == TEXT("now"))) { return false; }
        if (E.SourceWord != Next.SourceWord) { return false; }
        return IsPose(E.PoseID, TEXT("07_Aa")) && IsPose(Next.PoseID, TEXT("11_Oo"));
    }

    static bool IsClosurePose(FName PoseID)
    {
        return IsPose(PoseID, TEXT("22_MBP"));
    }

    static bool IsFricativeOrTransientPose(FName PoseID)
    {
        return IsPose(PoseID, TEXT("20_FV")) || IsPose(PoseID, TEXT("19_FV-Or-")) || IsPose(PoseID, TEXT("21_FV-Ee-"))
            || IsPose(PoseID, TEXT("14_ChJjSh")) || IsPose(PoseID, TEXT("24_Tongue_Th"));
    }

    static bool IsGlidePose(FName PoseID)
    {
        return IsPose(PoseID, TEXT("12_Ww-Oo-")) || IsPose(PoseID, TEXT("16_Ww-Ew-"));
    }

    static bool IsRoundOrWideVowelPose(FName PoseID)
    {
        return IsGlidePose(PoseID) || IsPose(PoseID, TEXT("11_Oo"))
            || IsPose(PoseID, TEXT("10_Or")) || IsPose(PoseID, TEXT("03_Ee")) || IsPose(PoseID, TEXT("05_Ay"))
            || IsPose(PoseID, TEXT("06_Eh")) || IsPose(PoseID, TEXT("07_Aa")) || IsPose(PoseID, TEXT("08_Ah"))
            || IsPose(PoseID, TEXT("09_Oh")) || IsPose(PoseID, TEXT("18_Uh"));
    }

    static bool IsVowelLikePose(FName PoseID)
    {
        return IsRoundOrWideVowelPose(PoseID) || IsPose(PoseID, TEXT("04_Ih")) || IsPose(PoseID, TEXT("18_Uh"));
    }

    static bool IsSentenceBoundaryGap(float GapSeconds)
    {
        return GapSeconds > 0.160f;
    }

    static bool IsTerminalHoldVowelPose(FName PoseID)
    {
        return IsPose(PoseID, TEXT("07_Aa"))
            || IsPose(PoseID, TEXT("08_Ah"))
            || IsPose(PoseID, TEXT("05_Ay"))
            || IsPose(PoseID, TEXT("11_Oo"))
            || IsPose(PoseID, TEXT("12_Ww-Oo-"))
            || IsPose(PoseID, TEXT("09_Oh"))
            || IsPose(PoseID, TEXT("10_Or"))
            || IsPose(PoseID, TEXT("03_Ee"));
    }

    static bool IsIncompatibleWithMBP(FName PoseID)
    {
        return IsPose(PoseID, TEXT("20_FV"))
            || IsPose(PoseID, TEXT("19_FV-Or-"))
            || IsPose(PoseID, TEXT("21_FV-Ee-"))
            || IsPose(PoseID, TEXT("14_ChJjSh"))
            || IsPose(PoseID, TEXT("24_Tongue_Th"));
    }

    static bool IsExplosivePose(FName PoseID)
    {
        // J9: must-read contact/explosive shapes. These need a real
        // visible contact window, not a one-sample mathematical peak.
        return IsClosurePose(PoseID)
            || IsPose(PoseID, TEXT("20_FV"))
            || IsPose(PoseID, TEXT("19_FV-Or-"))
            || IsPose(PoseID, TEXT("21_FV-Ee-"))
            || IsPose(PoseID, TEXT("14_ChJjSh"));
    }

    static bool IsProtectedClosurePose(FName PoseID)
    {
        return IsExplosivePose(PoseID);
    }

    static void DesiredShouldersForPose(FName PoseID, float& OutAttackSeconds, float& OutReleaseSeconds)
    {
        // Version J9: keep timing centers fixed, but give explosive/contact
        // poses a visible contact window.  This is not a timing shift; it only
        // widens the shoulder/hold around the already-authored center.
        if (IsClosurePose(PoseID))
        {
            OutAttackSeconds = 0.026f;
            OutReleaseSeconds = 0.066f;
            return;
        }
        if (IsPose(PoseID, TEXT("20_FV")) || IsPose(PoseID, TEXT("19_FV-Or-")) || IsPose(PoseID, TEXT("21_FV-Ee-")))
        {
            OutAttackSeconds = 0.036f;
            OutReleaseSeconds = 0.074f;
            return;
        }
        if (IsPose(PoseID, TEXT("14_ChJjSh")))
        {
            OutAttackSeconds = 0.034f;
            OutReleaseSeconds = 0.064f;
            return;
        }
        if (IsFricativeOrTransientPose(PoseID))
        {
            OutAttackSeconds = 0.040f;
            OutReleaseSeconds = 0.055f;
            return;
        }
        if (IsPose(PoseID, TEXT("04_Ih")) || IsPose(PoseID, TEXT("18_Uh")))
        {
            OutAttackSeconds = 0.058f;
            OutReleaseSeconds = 0.078f;
            return;
        }
        if (IsGlidePose(PoseID))
        {
            OutAttackSeconds = 0.090f;
            OutReleaseSeconds = 0.112f;
            return;
        }
        if (IsPose(PoseID, TEXT("11_Oo")) || IsPose(PoseID, TEXT("10_Or")))
        {
            OutAttackSeconds = 0.110f;
            OutReleaseSeconds = 0.135f;
            return;
        }
        if (IsRoundOrWideVowelPose(PoseID))
        {
            OutAttackSeconds = 0.090f;
            OutReleaseSeconds = 0.112f;
            return;
        }
        OutAttackSeconds = 0.070f;
        OutReleaseSeconds = 0.085f;
    }

    static void DesiredPeakWindowOffsetsForPose(FName PoseID, float& OutLeadSeconds, float& OutLagSeconds)
    {
        OutLeadSeconds = 0.012f;
        OutLagSeconds = 0.012f;
        if (IsClosurePose(PoseID))
        {
            OutLeadSeconds = 0.044f;
            OutLagSeconds = 0.014f;
            return;
        }
        if (IsPose(PoseID, TEXT("20_FV")) || IsPose(PoseID, TEXT("19_FV-Or-")) || IsPose(PoseID, TEXT("21_FV-Ee-")))
        {
            OutLeadSeconds = 0.034f;
            OutLagSeconds = 0.014f;
            return;
        }
        if (IsPose(PoseID, TEXT("14_ChJjSh")))
        {
            OutLeadSeconds = 0.028f;
            OutLagSeconds = 0.014f;
            return;
        }
        if (IsGlidePose(PoseID))
        {
            OutLeadSeconds = 0.020f;
            OutLagSeconds = 0.020f;
            return;
        }
        if (IsPose(PoseID, TEXT("11_Oo")) || IsPose(PoseID, TEXT("10_Or")))
        {
            OutLeadSeconds = 0.020f;
            OutLagSeconds = 0.020f;
            return;
        }
        if (IsRoundOrWideVowelPose(PoseID))
        {
            OutLeadSeconds = 0.014f;
            OutLagSeconds = 0.014f;
            return;
        }
    }

    static float PeakHoldHalfSecondsForPose(FName PoseID)
    {
        // Hold the authored peak briefly so a 30/60 Hz runtime sample can see the
        // intended magnitude without moving the event center. This fixes the case
        // where MBP/FV peaks were mathematically correct but fell between frames
        // and therefore read as weak.
        if (IsClosurePose(PoseID)) { return 0.034f; }       // ~68 ms full contact window
        if (IsPose(PoseID, TEXT("20_FV")) || IsPose(PoseID, TEXT("19_FV-Or-")) || IsPose(PoseID, TEXT("21_FV-Ee-"))) { return 0.028f; } // ~56 ms
        if (IsPose(PoseID, TEXT("14_ChJjSh"))) { return 0.024f; } // ~48 ms
        if (IsPose(PoseID, TEXT("11_Oo")) || IsPose(PoseID, TEXT("12_Ww-Oo-")) || IsPose(PoseID, TEXT("16_Ww-Ew-")) || IsPose(PoseID, TEXT("10_Or"))) { return 0.020f; }
        if (IsRoundOrWideVowelPose(PoseID)) { return 0.014f; }
        if (IsFricativeOrTransientPose(PoseID)) { return 0.010f; }
        return 0.012f;
    }

    static float PeakForPose(FName PoseID, float SourceStrength)
    {
        const FString S = PoseID.ToString();
        float Peak = FMath::Clamp(SourceStrength, 0.0f, 1.0f);
        // J2: keep the renderer faithful to budgeted ownership. If the
        // Budgeter chooses a strong/salient event, do not attenuate it below the
        // perceptual floor here. A SourceStrength of 1.0 can now actually render
        // as a full articulation at the event center.
        if (S == TEXT("12_Ww-Oo-") || S == TEXT("16_Ww-Ew-")) { if (SourceStrength >= 0.74f) { Peak = FMath::Max(Peak, 0.90f); } }
        else if (S == TEXT("22_MBP")) { Peak = FMath::Max(Peak, 0.99f); }
        else if (S == TEXT("20_FV") || S == TEXT("19_FV-Or-") || S == TEXT("21_FV-Ee-")) { Peak = FMath::Max(Peak, 0.98f); }
        else if (S == TEXT("14_ChJjSh")) { Peak = SourceStrength >= 0.74f ? FMath::Max(Peak, 0.94f) : FMath::Min(Peak, 0.62f); }
        else if (S == TEXT("11_Oo") || S == TEXT("10_Or")) { if (SourceStrength >= 0.72f) { Peak = FMath::Max(Peak, 0.98f); } }
        else if (S == TEXT("03_Ee")) { if (SourceStrength >= 0.72f) { Peak = FMath::Max(Peak, 0.86f); } }
        else if (S == TEXT("05_Ay")) { if (SourceStrength >= 0.72f) { Peak = FMath::Max(Peak, 0.88f); } }
        else if (S == TEXT("06_Eh")) { if (SourceStrength >= 0.68f) { Peak = FMath::Max(Peak, 0.74f); } }
        else if (S == TEXT("07_Aa") || S == TEXT("08_Ah")) { if (SourceStrength >= 0.72f) { Peak = FMath::Max(Peak, 0.90f); } }
        else if (S == TEXT("09_Oh")) { if (SourceStrength >= 0.72f) { Peak = FMath::Max(Peak, 0.90f); } }
        else if (S == TEXT("18_Uh")) { if (SourceStrength >= 0.58f) { Peak = FMath::Max(Peak, 0.62f); } }
        else if (S == TEXT("24_Tongue_Th")) { Peak = FMath::Clamp(Peak, 0.0f, 0.42f); }
        return FMath::Clamp(Peak, 0.0f, 1.0f);
    }

    static float EnvelopeAt(const FOffgridAIAlignedVisemeTrack& Track, int32 EventArrayIndex, float PlaybackSeconds)
    {
        if (!Track.Events.IsValidIndex(EventArrayIndex)) { return 0.0f; }
        const FOffgridAIAlignedVisemeEvent& E = Track.Events[EventArrayIndex];
        const float Center = E.FinalRenderCenterSeconds;

        float DesiredAttack = 0.070f;
        float DesiredRelease = 0.085f;
        DesiredShouldersForPose(E.PoseID, DesiredAttack, DesiredRelease);
        const bool bDiphthongGlideLeadIn = IsDiphthongGlideLeadIn(Track, EventArrayIndex);
        if (bDiphthongGlideLeadIn)
        {
            // J6: the first half of HOW/NOW is a tiny visible onset, not a
            // standalone Aa. Short shoulders keep it from starting before the
            // audible H and keep it glued to the following Oo.
            DesiredAttack = 0.028f;
            DesiredRelease = 0.045f;
        }
        const float HoldHalf = bDiphthongGlideLeadIn ? 0.006f : PeakHoldHalfSecondsForPose(E.PoseID);
        float PeakLeadHalf = HoldHalf;
        float PeakLagHalf = HoldHalf;
        DesiredPeakWindowOffsetsForPose(E.PoseID, PeakLeadHalf, PeakLagHalf);

        float PrevGap = 0.250f;
        float NextGap = 0.250f;
        bool bPrevIsProtectedClosure = false;
        bool bPrevPhraseBoundary = false;
        bool bNextPhraseBoundary = false;
        if (Track.Events.IsValidIndex(EventArrayIndex - 1))
        {
            const FOffgridAIAlignedVisemeEvent& Prev = Track.Events[EventArrayIndex - 1];
            PrevGap = FMath::Max(0.001f, Center - Prev.FinalRenderCenterSeconds);
            bPrevIsProtectedClosure = IsProtectedClosurePose(Prev.PoseID);
            bPrevPhraseBoundary = Prev.PhraseIndex != E.PhraseIndex;
        }
        if (Track.Events.IsValidIndex(EventArrayIndex + 1))
        {
            const FOffgridAIAlignedVisemeEvent& Next = Track.Events[EventArrayIndex + 1];
            NextGap = FMath::Max(0.001f, Next.FinalRenderCenterSeconds - Center);
            bNextPhraseBoundary = Next.PhraseIndex != E.PhraseIndex;
        }

        // J12: treat large center gaps as local speech-region boundaries even
        // when punctuation/phrase indexing is imperfect. This generalizes the
        // line-start anti-pop rule to resumed speech such as "... Bodega. How"
        // or short interjection starts such as "Yum".
        const bool bPrevSpeechRegionBoundary = bPrevPhraseBoundary || IsSentenceBoundaryGap(PrevGap);
        const bool bNextSpeechRegionBoundary = bNextPhraseBoundary || IsSentenceBoundaryGap(NextGap);

        float AttackSeconds = FMath::Min(DesiredAttack, FMath::Max(0.024f, PrevGap * 0.68f));
        float ReleaseSeconds = FMath::Min(DesiredRelease, FMath::Max(0.030f, NextGap * 0.68f));
        if (bDiphthongGlideLeadIn)
        {
            AttackSeconds = FMath::Min(AttackSeconds, 0.028f);
            ReleaseSeconds = FMath::Min(FMath::Max(0.032f, NextGap * 0.48f), 0.055f);
            PeakLeadHalf = 0.006f;
            PeakLagHalf = 0.004f;
        }

        // Let MBP/FV briefly own the topology before the following vowel starts.
        // The vowel peak is unchanged; only its early shoulder is delayed.
        if (bPrevIsProtectedClosure && IsVowelLikePose(E.PoseID) && PrevGap < 0.070f)
        {
            AttackSeconds = FMath::Min(AttackSeconds, FMath::Max(0.012f, PrevGap * 0.32f));
        }

        // J8: apply the line-start anti-pop idea to every resumed-speech
        // phrase.  Phrase boundaries are the buffered-mode proxy for local
        // SpeechRegion starts: the previous phrase may hold softly through the
        // silence, but the first event of the new phrase may not leak a long
        // anticipatory shoulder backward across that silence.  This fixes the
        // repeated-greeting case where the mouth began shaping HOW during the
        // punctuation pause and visually swallowed the terminal A of Bodega.
        if (bPrevSpeechRegionBoundary && PrevGap > 0.145f)
        {
            // Phrase/resumed-speech starts must not visibly pre-attack across
            // silence. Keep only a tiny anticipation shoulder.
            AttackSeconds = FMath::Min(AttackSeconds, 0.018f);
        }

        // If the new phrase begins with a compact glide pair (e.g. HOW = Aa ->
        // Oo), do not let the target vowel's long round-vowel shoulder start
        // before the phrase lead-in has actually appeared.  The center/peak
        // stays fixed; only the pre-peak shoulder is clipped.
        if (Track.Events.IsValidIndex(EventArrayIndex - 1))
        {
            const FOffgridAIAlignedVisemeEvent& Prev = Track.Events[EventArrayIndex - 1];
            const bool bPreviousWasPhraseLeadIn = Prev.PhraseIndex == E.PhraseIndex
                && EventArrayIndex >= 2
                && Track.Events.IsValidIndex(EventArrayIndex - 2)
                && Track.Events[EventArrayIndex - 2].PhraseIndex != E.PhraseIndex
                && Prev.SourceWord == E.SourceWord
                && IsPose(Prev.PoseID, TEXT("07_Aa"))
                && IsPose(E.PoseID, TEXT("11_Oo"));
            if (bPreviousWasPhraseLeadIn)
            {
                AttackSeconds = FMath::Min(AttackSeconds, FMath::Max(0.012f, PrevGap * 0.36f));
            }
        }

        // Avoid the J4 "neutral vacuum" in short sentence pauses. If a salient
        // vowel ends a phrase and the next phrase starts soon, keep the prior
        // articulation alive into the pause instead of decaying to rest and then
        // ramping back up as a visible extra mouth movement.
        const bool bTerminalHoldCandidate = bNextSpeechRegionBoundary
            && IsTerminalHoldVowelPose(E.PoseID)
            && NextGap > 0.145f
            && NextGap < 0.560f;
        if (bTerminalHoldCandidate)
        {
            // J12: phrase-terminal completion. Let the last vowel of the phrase
            // visibly land and remain softly held through silence; do not let the
            // following phrase's attack steal the ending.
            ReleaseSeconds = FMath::Max(ReleaseSeconds, FMath::Min(0.340f, NextGap * 0.86f));
        }

        const float PeakStart = Center - PeakLeadHalf;
        const float PeakEnd = Center + PeakLagHalf;
        float AttackStart = PeakStart - AttackSeconds;
        float ReleaseEnd = PeakEnd + ReleaseSeconds;

        // Preserve the authored anti-pop boundary. This trims pre-speech shoulder
        // energy only; the event center and peak hold remain authored.  Do not
        // let the gate erase the peak window entirely: J10 exposed cases where
        // a first speech-region event existed in the aligned track but produced
        // no submitted peak because AttackStart was clamped past PeakStart.
        AttackStart = FMath::Max(AttackStart, Track.SpeechStartSeconds - 0.035f);
        AttackStart = FMath::Min(AttackStart, PeakStart);

        if (PlaybackSeconds < AttackStart || PlaybackSeconds > ReleaseEnd) { return 0.0f; }

        float Envelope = 0.0f;
        if (PlaybackSeconds >= PeakStart && PlaybackSeconds <= PeakEnd)
        {
            Envelope = 1.0f;
        }
        else if (PlaybackSeconds < PeakStart)
        {
            Envelope = SmootherStep01((PlaybackSeconds - AttackStart) / FMath::Max(0.001f, PeakStart - AttackStart));
        }
        else
        {
            Envelope = 1.0f - SmootherStep01((PlaybackSeconds - PeakEnd) / FMath::Max(0.001f, ReleaseEnd - PeakEnd));
        }

        if (bTerminalHoldCandidate && PlaybackSeconds > PeakEnd)
        {
            // Low-strength continuity only. The peak remains at Center, but the
            // terminal vowel cannot vanish immediately into a neutral mouth
            // before the next local speech region begins.
            const float HoldEnd = FMath::Min(ReleaseEnd, Center + FMath::Min(0.320f, NextGap * 0.72f));
            if (PlaybackSeconds <= HoldEnd)
            {
                Envelope = FMath::Max(Envelope, 0.42f);
            }
        }

        return Envelope;
    }
}

TArray<FOffgridAISubmittedVisemeSample> FOffgridAIVisemePerformer::Sample(const FOffgridAIAlignedVisemeTrack& Track, float PlaybackSeconds, bool bGateBeforeSpeechStart)
{
    TArray<FOffgridAISubmittedVisemeSample> Out;
    if (Track.Events.Num() <= 0) { return Out; }
    if (bGateBeforeSpeechStart && PlaybackSeconds < Track.SpeechStartSeconds - 0.035f) { return Out; }

    float ProtectedMBP = 0.0f;
    float ProtectedFV = 0.0f;
    float ProtectedCh = 0.0f;

    for (int32 EventArrayIndex = 0; EventArrayIndex < Track.Events.Num(); ++EventArrayIndex)
    {
        const FOffgridAIAlignedVisemeEvent& E = Track.Events[EventArrayIndex];
        if (E.PoseID == NAME_None || E.EventIndex == INDEX_NONE) { continue; }
        if (!FMath::IsFinite(E.RenderStartSeconds) || !FMath::IsFinite(E.FinalRenderCenterSeconds) || !FMath::IsFinite(E.RenderEndSeconds) || E.RenderEndSeconds <= E.RenderStartSeconds) { continue; }
        float Weight = FMath::Clamp(EnvelopeAt(Track, EventArrayIndex, PlaybackSeconds) * PeakForPose(E.PoseID, E.Strength), 0.0f, 1.0f);
        if (IsDiphthongGlideLeadIn(Track, EventArrayIndex))
        {
            // Let the Oo own the diphthong. The Aa is a brief visible cue only.
            Weight = FMath::Min(Weight, 0.68f);
        }
        if (Weight <= 0.0005f) { continue; }

        const float Dt = FMath::Abs(PlaybackSeconds - E.FinalRenderCenterSeconds);
        if (IsClosurePose(E.PoseID) && Dt <= 0.044f)
        {
            Weight = FMath::Max(Weight, 0.985f);
            ProtectedMBP = FMath::Max(ProtectedMBP, Weight);
        }
        else if ((IsPose(E.PoseID, TEXT("20_FV")) || IsPose(E.PoseID, TEXT("19_FV-Or-")) || IsPose(E.PoseID, TEXT("21_FV-Ee-"))) && Dt <= 0.038f)
        {
            Weight = FMath::Max(Weight, 0.965f);
            ProtectedFV = FMath::Max(ProtectedFV, Weight);
        }
        else if (IsPose(E.PoseID, TEXT("14_ChJjSh")) && Dt <= 0.034f)
        {
            Weight = FMath::Max(Weight, 0.920f);
            ProtectedCh = FMath::Max(ProtectedCh, Weight);
        }

        FOffgridAISubmittedVisemeSample S;
        S.EventIndex = E.EventIndex;
        S.PoseID = E.PoseID;
        S.SourceWord = E.SourceWord;
        S.PlaybackSeconds = PlaybackSeconds;
        S.CommittedRenderStartSeconds = E.RenderStartSeconds;
        S.CommittedRenderCenterSeconds = E.FinalRenderCenterSeconds;
        S.CommittedRenderEndSeconds = E.RenderEndSeconds;
        S.SubmittedWeight = Weight;
        S.SourceStrength = E.Strength;
        Out.Add(S);
    }

    if (ProtectedMBP > 0.0f || ProtectedFV > 0.0f || ProtectedCh > 0.0f)
    {
        for (FOffgridAISubmittedVisemeSample& S : Out)
        {
            // J12: a protected MBP closure needs a clean closed-lip topology.
            // Lingering FV/Ch/Th contacts from the previous word must not remain
            // visible on top of the B/P/M closure (the Alfie's -> Bodega case).
            if (ProtectedMBP > 0.0f && IsIncompatibleWithMBP(S.PoseID))
            {
                const float Scale = 1.0f - ProtectedMBP * 0.92f;
                S.SubmittedWeight = FMath::Min(S.SubmittedWeight * FMath::Clamp(Scale, 0.0f, 1.0f), 0.12f);
                continue;
            }

            if (IsProtectedClosurePose(S.PoseID)) { continue; }
            if (!IsVowelLikePose(S.PoseID)) { continue; }
            float Scale = 1.0f;
            if (ProtectedMBP > 0.0f) { Scale *= (1.0f - ProtectedMBP * 0.58f); }
            if (ProtectedFV > 0.0f) { Scale *= (1.0f - ProtectedFV * 0.34f); }
            if (ProtectedCh > 0.0f) { Scale *= (1.0f - ProtectedCh * 0.30f); }
            S.SubmittedWeight = FMath::Clamp(S.SubmittedWeight * Scale, 0.0f, 1.0f);
        }
    }

    Out.RemoveAll([](const FOffgridAISubmittedVisemeSample& S) { return S.SubmittedWeight <= 0.0005f; });
    return Out;
}


TMap<FName, float> FOffgridAIVisemePerformer::CollapseByPoseID(const TArray<FOffgridAISubmittedVisemeSample>& Samples)
{
    TMap<FName, float> Out;
    for (const FOffgridAISubmittedVisemeSample& S : Samples)
    {
        if (S.PoseID == NAME_None || S.SubmittedWeight <= 0.0005f) { continue; }
        float& Existing = Out.FindOrAdd(S.PoseID);
        Existing = FMath::Max(Existing, FMath::Clamp(S.SubmittedWeight, 0.0f, 1.0f));
    }
    return Out;
}

TArray<FOffgridAIPerformedVisemeFrame> FOffgridAIVisemePerformer::BuildFrames(const FOffgridAIAlignedVisemeTrack& Track, float FPS)
{
    TArray<FOffgridAIPerformedVisemeFrame> Frames;
    if (Track.Events.Num() <= 0 || FPS <= 0.0f)
    {
        return Frames;
    }

    const float Step = 1.0f / FPS;
    const float End = Track.SpeechEndSeconds + 0.250f;
    for (float T = 0.0f; T <= End + 0.0001f; T += Step)
    {
        FOffgridAIPerformedVisemeFrame Frame;
        Frame.PlaybackSeconds = T;
        Frame.NPCID = Track.NPCID;
        Frame.LineID = Track.LineID;
        Frame.AbstractVisemeWeights = CollapseByPoseID(Sample(Track, T, true));
        Frames.Add(Frame);
    }
    return Frames;
}
