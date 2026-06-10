#include "Lipsync/OffgridAIAudioOccupancyScheduler.h"
#include <cfloat>

namespace
{
    static float Clamp01(float Value)
    {
        return FMath::Clamp(Value, 0.0f, 1.0f);
    }

    static float EventTextCenterSec(const FOffgridAITextVisemePlan& Plan, const FOffgridAITextVisemeEvent& Event)
    {
        const float CenterNorm = (Clamp01(Event.StartNorm) + Clamp01(Event.EndNorm)) * 0.5f;
        return CenterNorm * FMath::Max(Plan.EstimatedDurationSeconds, 0.10f);
    }

    static bool PoseContains(FName PoseID, const TCHAR* Token)
    {
        return PoseID.ToString().Contains(Token);
    }

    static bool IsVowelLikePose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        return S.Contains(TEXT("Aa")) || S.Contains(TEXT("Ah")) || S.Contains(TEXT("Oh")) ||
            S.Contains(TEXT("Oo")) || S.Contains(TEXT("Or")) || S.Contains(TEXT("Uh")) ||
            S.Contains(TEXT("Ee")) || S.Contains(TEXT("Ih")) || S.Contains(TEXT("Ay")) ||
            S.Contains(TEXT("Eh")) || S.Contains(TEXT("Ww"));
    }

    static bool IsLandmarkLikePose(FName PoseID)
    {
        const FString S = PoseID.ToString();
        return S.Contains(TEXT("MBP")) || S.Contains(TEXT("FV")) || S.Contains(TEXT("Tongue")) ||
            S.Contains(TEXT("Ch")) || S.Contains(TEXT("Jj")) || S.Contains(TEXT("Sh")) || S.Contains(TEXT("Ww"));
    }

    static float EventWidthSeconds(const FOffgridAITextVisemeEvent& Event)
    {
        if (Event.Viseme == EOffgridAITextViseme::MBP || PoseContains(Event.PoseID, TEXT("MBP")))
        {
            return 0.090f;
        }
        if (Event.bIsLandmark || IsLandmarkLikePose(Event.PoseID))
        {
            return 0.095f;
        }
        if (IsVowelLikePose(Event.PoseID))
        {
            return 0.125f;
        }
        return 0.075f;
    }

    static float AudioIslandStartSec(const FOffgridAIStreamingSpeechIsland& Island)
    {
        return Island.AudioBufferStartSec;
    }

    static float AudioIslandEndSec(const FOffgridAIStreamingSpeechIsland& Island)
    {
        return FMath::Max(Island.AudioBufferEndSec, Island.AudioBufferLastSpeechSec);
    }

    static bool HasUsableAudioIsland(const FOffgridAIStreamingSpeechIsland& Island)
    {
        return Island.bStarted && AudioIslandEndSec(Island) > AudioIslandStartSec(Island) + 0.001f;
    }

    static void EnforceTrackMonotonicity(FOffgridAIAlignedVisemeTrack& Track)
    {
        float PreviousCenterSec = -FLT_MAX;
        for (FOffgridAIAlignedVisemeEvent& Event : Track.Events)
        {
            const float RequiredCenterSec = PreviousCenterSec > -FLT_MAX * 0.5f ? PreviousCenterSec + 0.006f : Event.FinalRenderCenterSeconds;
            if (Event.FinalRenderCenterSeconds < RequiredCenterSec)
            {
                const float DeltaSec = RequiredCenterSec - Event.FinalRenderCenterSeconds;
                Event.FinalRenderCenterSeconds += DeltaSec;
                Event.RenderStartSeconds += DeltaSec;
                Event.RenderEndSeconds += DeltaSec;
                Event.bCenterOrderRepaired = true;
                Event.CenterOrderRepairSeconds += DeltaSec;
            }
            PreviousCenterSec = Event.FinalRenderCenterSeconds;
        }
    }

    static float CurrentCommittedSpeechEnd(const FOffgridAIAlignedVisemeTrack& Track)
    {
        float EndSec = 0.0f;
        for (const FOffgridAIAlignedVisemeEvent& Event : Track.Events)
        {
            EndSec = FMath::Max(EndSec, Event.RenderEndSeconds);
            EndSec = FMath::Max(EndSec, Event.FinalRenderCenterSeconds);
        }
        return EndSec;
    }

    static void GatherUsableAudioIslands(
        const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
        TArray<int32>& OutUsableAudioIndices)
    {
        OutUsableAudioIndices.Reset();
        for (int32 AudioI = 0; AudioI < AudioIslands.Num(); ++AudioI)
        {
            if (HasUsableAudioIsland(AudioIslands[AudioI]))
            {
                OutUsableAudioIndices.Add(AudioI);
            }
        }
    }

    static float TotalUsableSpeechActiveSec(
        const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
        const TArray<int32>& UsableAudioIndices)
    {
        float TotalSec = 0.0f;
        for (const int32 AudioI : UsableAudioIndices)
        {
            const FOffgridAIStreamingSpeechIsland& Island = AudioIslands[AudioI];
            TotalSec += FMath::Max(0.0f, AudioIslandEndSec(Island) - AudioIslandStartSec(Island));
        }
        return TotalSec;
    }

    static float LastUsableAudioEnd(
        const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
        const TArray<int32>& UsableAudioIndices)
    {
        float EndSec = 0.0f;
        for (const int32 AudioI : UsableAudioIndices)
        {
            EndSec = FMath::Max(EndSec, AudioIslandEndSec(AudioIslands[AudioI]));
        }
        return EndSec;
    }



    struct FTextIslandScheduleInfo
    {
        int32 TextIslandIndex = INDEX_NONE;
        int32 FirstEventIndex = INDEX_NONE;
        int32 LastEventIndex = INDEX_NONE;
        float TextStartSec = 0.0f;
        float TextEndSec = 0.0f;
        float TextSpanSec = 0.0f;
        int32 FirstUsableAudioOrdinal = INDEX_NONE;
        int32 LastUsableAudioOrdinal = INDEX_NONE;
        float AudioActiveStartSec = 0.0f;
        float AudioActiveSpanSec = 0.0f;
    };

    static int32 FindTextIslandInfoIndex(const TArray<FTextIslandScheduleInfo>& Infos, int32 TextIslandIndex)
    {
        for (int32 I = 0; I < Infos.Num(); ++I)
        {
            if (Infos[I].TextIslandIndex == TextIslandIndex)
            {
                return I;
            }
        }
        return INDEX_NONE;
    }

    static void BuildTextIslandScheduleInfos(
        const FOffgridAITextVisemePlan& TextPlan,
        const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
        const TArray<int32>& UsableAudioIndices,
        TArray<FTextIslandScheduleInfo>& OutInfos)
    {
        OutInfos.Reset();
        if (TextPlan.Events.Num() <= 0)
        {
            return;
        }

        for (int32 EventI = 0; EventI < TextPlan.Events.Num(); ++EventI)
        {
            const FOffgridAITextVisemeEvent& Event = TextPlan.Events[EventI];
            const int32 IslandIndex = FMath::Max(0, Event.SentenceIslandIndex);
            int32 InfoIndex = FindTextIslandInfoIndex(OutInfos, IslandIndex);
            if (InfoIndex == INDEX_NONE)
            {
                FTextIslandScheduleInfo Info;
                Info.TextIslandIndex = IslandIndex;
                Info.FirstEventIndex = EventI;
                Info.LastEventIndex = EventI;
                Info.TextStartSec = EventTextCenterSec(TextPlan, Event);
                Info.TextEndSec = Info.TextStartSec;
                OutInfos.Add(Info);
            }
            else
            {
                FTextIslandScheduleInfo& Info = OutInfos[InfoIndex];
                Info.FirstEventIndex = FMath::Min(Info.FirstEventIndex, EventI);
                Info.LastEventIndex = FMath::Max(Info.LastEventIndex, EventI);
                const float CenterSec = EventTextCenterSec(TextPlan, Event);
                Info.TextStartSec = FMath::Min(Info.TextStartSec, CenterSec);
                Info.TextEndSec = FMath::Max(Info.TextEndSec, CenterSec);
            }
        }

        OutInfos.Sort([](const FTextIslandScheduleInfo& A, const FTextIslandScheduleInfo& B)
        {
            return A.TextIslandIndex < B.TextIslandIndex;
        });

        float TotalTextSpanSec = 0.0f;
        for (FTextIslandScheduleInfo& Info : OutInfos)
        {
            Info.TextSpanSec = FMath::Max(0.060f, Info.TextEndSec - Info.TextStartSec);
            TotalTextSpanSec += Info.TextSpanSec;
        }

        if (UsableAudioIndices.Num() <= 0 || OutInfos.Num() <= 0)
        {
            return;
        }

        // Pair text sentence islands to contiguous detected speech islands.
        // This preserves hard punctuation ordering without trying to infer phonemes.
        // Every remaining text island gets at least one audio island when possible.
        int32 NextAudioOrdinal = 0;
        float ConsumedTextSpanSec = 0.0f;
        for (int32 InfoI = 0; InfoI < OutInfos.Num(); ++InfoI)
        {
            FTextIslandScheduleInfo& Info = OutInfos[InfoI];
            const int32 RemainingTextIslands = OutInfos.Num() - InfoI;
            const int32 RemainingAudioIslands = UsableAudioIndices.Num() - NextAudioOrdinal;
            if (RemainingAudioIslands <= 0)
            {
                Info.FirstUsableAudioOrdinal = INDEX_NONE;
                Info.LastUsableAudioOrdinal = INDEX_NONE;
                continue;
            }

            Info.FirstUsableAudioOrdinal = NextAudioOrdinal;

            if (RemainingTextIslands <= 1)
            {
                Info.LastUsableAudioOrdinal = UsableAudioIndices.Num() - 1;
                NextAudioOrdinal = UsableAudioIndices.Num();
            }
            else if (RemainingAudioIslands <= RemainingTextIslands)
            {
                Info.LastUsableAudioOrdinal = NextAudioOrdinal;
                ++NextAudioOrdinal;
            }
            else
            {
                ConsumedTextSpanSec += Info.TextSpanSec;
                const float TargetTextFraction = TotalTextSpanSec > 0.001f ? ConsumedTextSpanSec / TotalTextSpanSec : float(InfoI + 1) / float(OutInfos.Num());
                int32 TargetNextAudioOrdinal = FMath::RoundToInt(TargetTextFraction * float(UsableAudioIndices.Num()));
                const int32 MinNextAudioOrdinal = NextAudioOrdinal + 1;
                const int32 MaxNextAudioOrdinal = UsableAudioIndices.Num() - (RemainingTextIslands - 1);
                TargetNextAudioOrdinal = FMath::Clamp(TargetNextAudioOrdinal, MinNextAudioOrdinal, MaxNextAudioOrdinal);
                Info.LastUsableAudioOrdinal = TargetNextAudioOrdinal - 1;
                NextAudioOrdinal = TargetNextAudioOrdinal;
            }

            Info.AudioActiveStartSec = 0.0f;
            for (int32 Ordinal = 0; Ordinal < Info.FirstUsableAudioOrdinal; ++Ordinal)
            {
                const FOffgridAIStreamingSpeechIsland& Island = AudioIslands[UsableAudioIndices[Ordinal]];
                Info.AudioActiveStartSec += FMath::Max(0.0f, AudioIslandEndSec(Island) - AudioIslandStartSec(Island));
            }

            Info.AudioActiveSpanSec = 0.0f;
            for (int32 Ordinal = Info.FirstUsableAudioOrdinal; Ordinal <= Info.LastUsableAudioOrdinal; ++Ordinal)
            {
                const FOffgridAIStreamingSpeechIsland& Island = AudioIslands[UsableAudioIndices[Ordinal]];
                Info.AudioActiveSpanSec += FMath::Max(0.0f, AudioIslandEndSec(Island) - AudioIslandStartSec(Island));
            }
        }
    }


    static float EffectiveO01RegionActiveSpanSec(const FTextIslandScheduleInfo& Info)
    {
        const float PlannedSpanSec = FMath::Max(0.060f, Info.TextSpanSec);
        const float ObservedSpanSec = FMath::Max(0.0f, Info.AudioActiveSpanSec);
        if (ObservedSpanSec < 0.120f)
        {
            return PlannedSpanSec;
        }

        const float RawScale = ObservedSpanSec / PlannedSpanSec;
        // O01 is the first runtime attempt at region-duration pacing. Trust normal
        // observed region scales, but fall back to planner timing for obvious
        // detector/pairing failures rather than letting one bad island collapse.
        if (RawScale < 0.50f || RawScale > 2.25f)
        {
            return PlannedSpanSec;
        }
        return ObservedSpanSec;
    }

    static bool FindAudioTimeForIslandLocalActiveElapsed(
        const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
        const TArray<int32>& UsableAudioIndices,
        const FTextIslandScheduleInfo& Info,
        float TargetIslandLocalActiveSec,
        float& OutAudioSec,
        int32& OutAudioIndex,
        float& OutGlobalAudioActiveSec)
    {
        if (Info.FirstUsableAudioOrdinal == INDEX_NONE || Info.LastUsableAudioOrdinal == INDEX_NONE)
        {
            return false;
        }

        float AccumulatedLocalSec = 0.0f;
        for (int32 Ordinal = Info.FirstUsableAudioOrdinal; Ordinal <= Info.LastUsableAudioOrdinal; ++Ordinal)
        {
            const int32 AudioI = UsableAudioIndices[Ordinal];
            const FOffgridAIStreamingSpeechIsland& Island = AudioIslands[AudioI];
            const float StartSec = AudioIslandStartSec(Island);
            const float EndSec = AudioIslandEndSec(Island);
            const float SpanSec = FMath::Max(0.0f, EndSec - StartSec);
            if (TargetIslandLocalActiveSec <= AccumulatedLocalSec + SpanSec + 0.0005f)
            {
                const float LocalActiveSec = FMath::Clamp(TargetIslandLocalActiveSec - AccumulatedLocalSec, 0.0f, SpanSec);
                OutAudioSec = StartSec + LocalActiveSec;
                OutAudioIndex = AudioI;
                OutGlobalAudioActiveSec = Info.AudioActiveStartSec + AccumulatedLocalSec + LocalActiveSec;
                return true;
            }
            AccumulatedLocalSec += SpanSec;
        }
        return false;
    }
    static bool FindAudioTimeForActiveElapsed(
        const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
        const TArray<int32>& UsableAudioIndices,
        float TargetActiveElapsedSec,
        float& OutAudioSec,
        int32& OutAudioIndex,
        float& OutAudioActiveSec)
    {
        float AccumulatedSpeechSec = 0.0f;
        for (const int32 AudioI : UsableAudioIndices)
        {
            const FOffgridAIStreamingSpeechIsland& Island = AudioIslands[AudioI];
            const float StartSec = AudioIslandStartSec(Island);
            const float EndSec = AudioIslandEndSec(Island);
            const float SpanSec = FMath::Max(0.0f, EndSec - StartSec);
            if (TargetActiveElapsedSec <= AccumulatedSpeechSec + SpanSec + 0.0005f)
            {
                const float LocalActiveSec = FMath::Clamp(TargetActiveElapsedSec - AccumulatedSpeechSec, 0.0f, SpanSec);
                OutAudioSec = StartSec + LocalActiveSec;
                OutAudioIndex = AudioI;
                OutAudioActiveSec = AccumulatedSpeechSec + LocalActiveSec;
                return true;
            }
            AccumulatedSpeechSec += SpanSec;
        }
        return false;
    }

    static FName PlaybackModeForCommitReason(FName CommitReason)
    {
        if (CommitReason == FName(TEXT("M09_audio_occupancy")) || CommitReason == FName(TEXT("M08_audio_occupancy")))
        {
            return FName(TEXT("speech_active"));
        }
        if (CommitReason == FName(TEXT("M09_live_tail_drain_after_audio")) || CommitReason == FName(TEXT("M08_live_tail_drain_after_audio")))
        {
            return FName(TEXT("tail_drain"));
        }
        if (CommitReason == FName(TEXT("end_of_stream_flush")))
        {
            return FName(TEXT("final_flush"));
        }
        return FName(TEXT("fallback"));
    }
}

void FOffgridAIAudioOccupancyScheduler::UpdateCommittedTrack(
    const FOffgridAITextVisemePlan& TextPlan,
    const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
    const TArray<FOffgridAIStreamingAudioFeatureFrame>* AudioFeatureFrames,
    float CurrentPlaybackSec,
    float PrerollSec,
    float IslandDurationScale,
    bool bInputStreamClosed,
    FOffgridAIAudioOccupancySchedulerState& SchedulerState,
    FOffgridAIAlignedVisemeTrack& InOutTrack,
    bool& bOutHasTrack)
{
    (void)AudioFeatureFrames;
    (void)IslandDurationScale;
    (void)SchedulerState;

    if (TextPlan.Events.Num() <= 0)
    {
        bOutHasTrack = true;
        return;
    }

    if (InOutTrack.NPCID == NAME_None)
    {
        InOutTrack.NPCID = FName(TEXT("Alpha"));
    }
    if (InOutTrack.LineID == NAME_None)
    {
        InOutTrack.LineID = FName(TEXT("AlphaLine"));
    }

    InOutTrack.Events.Reset();

    TArray<int32> UsableAudioIndices;
    GatherUsableAudioIslands(AudioIslands, UsableAudioIndices);
    if (UsableAudioIndices.Num() <= 0)
    {
        InOutTrack.SpeechStartSeconds = 0.0f;
        InOutTrack.SpeechEndSeconds = 0.0f;
        bOutHasTrack = true;
        return;
    }

    TArray<FTextIslandScheduleInfo> TextIslandInfos;
    BuildTextIslandScheduleInfos(TextPlan, AudioIslands, UsableAudioIndices, TextIslandInfos);

    const float FirstTextCenterSec = EventTextCenterSec(TextPlan, TextPlan.Events[0]);
    const float TotalSpeechActiveSec = TotalUsableSpeechActiveSec(AudioIslands, UsableAudioIndices);
    const float FinalAudioEndSec = LastUsableAudioEnd(AudioIslands, UsableAudioIndices);

    float LastPublishedCenterSec = -FLT_MAX;
    int32 LastPublishedAudioIndex = INDEX_NONE;

    for (int32 EventI = 0; EventI < TextPlan.Events.Num(); ++EventI)
    {
        const FOffgridAITextVisemeEvent& TextEvent = TextPlan.Events[EventI];
        const float TextCenterSec = EventTextCenterSec(TextPlan, TextEvent);
        const int32 TextIslandIndex = FMath::Max(0, TextEvent.SentenceIslandIndex);
        const int32 TextIslandInfoIndex = FindTextIslandInfoIndex(TextIslandInfos, TextIslandIndex);
        const FTextIslandScheduleInfo* TextIslandInfo = TextIslandInfos.IsValidIndex(TextIslandInfoIndex) ? &TextIslandInfos[TextIslandInfoIndex] : nullptr;
        const float IslandTextStartSec = TextIslandInfo ? TextIslandInfo->TextStartSec : FirstTextCenterSec;
        const float IslandTextEndSec = TextIslandInfo ? TextIslandInfo->TextEndSec : TextPlan.EstimatedDurationSeconds;
        const float IslandTextSpanSec = FMath::Max(0.001f, IslandTextEndSec - IslandTextStartSec);
        const float IslandLocalTextSec = FMath::Clamp(TextCenterSec - IslandTextStartSec, 0.0f, IslandTextSpanSec);
        const float IslandLocalNorm = FMath::Clamp(IslandLocalTextSec / IslandTextSpanSec, 0.0f, 1.0f);
        const float EffectiveIslandActiveSpanSec = TextIslandInfo
            ? EffectiveO01RegionActiveSpanSec(*TextIslandInfo)
            : IslandTextSpanSec;
        const float TargetActiveElapsedSec = TextIslandInfo
            ? TextIslandInfo->AudioActiveStartSec + IslandLocalNorm * EffectiveIslandActiveSpanSec
            : FMath::Max(0.0f, TextCenterSec - FirstTextCenterSec);
        const float TargetIslandLocalActiveSec = TextIslandInfo
            ? IslandLocalNorm * EffectiveIslandActiveSpanSec
            : TargetActiveElapsedSec;

        float CenterSec = 0.0f;
        int32 AudioIslandIndex = INDEX_NONE;
        float AudioActiveSec = 0.0f;
        const bool bMappedToObservedSpeech = TextIslandInfo
            ? FindAudioTimeForIslandLocalActiveElapsed(
                AudioIslands,
                UsableAudioIndices,
                *TextIslandInfo,
                TargetIslandLocalActiveSec,
                CenterSec,
                AudioIslandIndex,
                AudioActiveSec)
            : FindAudioTimeForActiveElapsed(
                AudioIslands,
                UsableAudioIndices,
                TargetActiveElapsedSec,
                CenterSec,
                AudioIslandIndex,
                AudioActiveSec);

        if (!bMappedToObservedSpeech)
        {
            const float TailDrainStartSec = FinalAudioEndSec + 0.030f;
            const float TailDrainReadySec = FinalAudioEndSec + 0.020f;
            if (!bInputStreamClosed && CurrentPlaybackSec + PrerollSec < TailDrainReadySec)
            {
                break;
            }

            const float MissingSpeechActiveSec = FMath::Max(0.0f, TargetActiveElapsedSec - TotalSpeechActiveSec);
            const float TailDrainScale = 0.70f;
            CenterSec = FMath::Max(TailDrainStartSec + MissingSpeechActiveSec * TailDrainScale, LastPublishedCenterSec + 0.006f);
            AudioActiveSec = TotalSpeechActiveSec;
        }

        const float WidthSec = EventWidthSeconds(TextEvent);
        FOffgridAIAlignedVisemeEvent Added;
        Added.EventIndex = EventI;
        Added.PoseID = TextEvent.PoseID;
        Added.Strength = Clamp01(TextEvent.Strength);
        Added.SourceWord = TextEvent.SourceText;
        Added.WordIndex = TextEvent.WordIndex;
        Added.PhraseIndex = TextEvent.PhraseIndex;
        Added.bIsLandmark = TextEvent.bIsLandmark;
        Added.TextCenterNorm = (Clamp01(TextEvent.StartNorm) + Clamp01(TextEvent.EndNorm)) * 0.5f;
        Added.TextDiagnosticCenterSeconds = TextCenterSec;
        Added.FinalRenderCenterSeconds = CenterSec;
        Added.RenderStartSeconds = FMath::Max(0.0f, CenterSec - WidthSec * 0.5f);
        Added.RenderEndSeconds = CenterSec + WidthSec * 0.5f;
        Added.AlignmentConfidence = bMappedToObservedSpeech ? 1.0f : 0.20f;
        Added.CommitPlaybackSeconds = -1.0f;
        Added.CommitLeadSeconds = -1.0f;
        Added.CommitReason = bMappedToObservedSpeech ? FName(TEXT("O01_region_duration_pacing")) : FName(TEXT("O01_region_duration_tail_drain"));
        Added.EffectiveCommitHorizonSeconds = PrerollSec;
        Added.EffectiveSegmentScale = IslandTextSpanSec > 0.001f ? EffectiveIslandActiveSpanSec / IslandTextSpanSec : 1.0f;

        Added.RequiredActiveElapsedSeconds = TargetActiveElapsedSec;
        Added.ObservedActiveElapsedSeconds = AudioActiveSec;
        Added.ActiveProgressDeficitSeconds = FMath::Max(0.0f, TargetActiveElapsedSec - AudioActiveSec);
        Added.RequiredProgressNorm = FMath::Clamp(TargetActiveElapsedSec / FMath::Max(TextPlan.EstimatedDurationSeconds, 0.001f), 0.0f, 1.0f);
        Added.ObservedProgressNorm = FMath::Clamp(AudioActiveSec / FMath::Max(TextPlan.EstimatedDurationSeconds, 0.001f), 0.0f, 1.0f);
        Added.ActiveProgressRatio = TargetActiveElapsedSec > 0.001f ? AudioActiveSec / TargetActiveElapsedSec : 1.0f;
        Added.bMappedToObservedSpeech = bMappedToObservedSpeech;

        Added.TextIslandIndex = TextIslandIndex;
        Added.AudioIslandIndex = AudioIslandIndex;
        Added.IslandTextStartSeconds = IslandTextStartSec;
        Added.IslandTextEndSeconds = IslandTextEndSec;
        Added.IslandAudioStartSeconds = TextIslandInfo && TextIslandInfo->FirstUsableAudioOrdinal != INDEX_NONE
            ? AudioIslandStartSec(AudioIslands[UsableAudioIndices[TextIslandInfo->FirstUsableAudioOrdinal]])
            : (AudioIslandIndex != INDEX_NONE ? AudioIslandStartSec(AudioIslands[AudioIslandIndex]) : CenterSec);
        Added.IslandAudioEndSeconds = TextIslandInfo && TextIslandInfo->LastUsableAudioOrdinal != INDEX_NONE
            ? AudioIslandEndSec(AudioIslands[UsableAudioIndices[TextIslandInfo->LastUsableAudioOrdinal]])
            : (AudioIslandIndex != INDEX_NONE ? AudioIslandEndSec(AudioIslands[AudioIslandIndex]) : CenterSec);
        Added.IslandAudioSpanSeconds = Added.IslandAudioEndSeconds - Added.IslandAudioStartSeconds;
        Added.IslandLocalNorm = IslandLocalNorm;
        Added.bReusedIslandClock = TextIslandInfo && TextIslandInfo->FirstUsableAudioOrdinal != TextIslandInfo->LastUsableAudioOrdinal;

        Added.PlannerIslandPredictedDurationSeconds = IslandTextSpanSec;
        Added.PlannerIslandSpeechMaterialSeconds = IslandTextSpanSec;
        Added.PlannerIslandPunctuationSeconds = 0.0f;
        Added.PlannerEventPredictedOffsetSeconds = IslandLocalTextSec;
        Added.PlannerEventNormCenter = Added.IslandLocalNorm;
        Added.PlannerProsodyGroupIndex = TextEvent.PhraseIndex;
        Added.PlannerProsodyRole = TextEvent.bIsDominant ? FName(TEXT("dominant")) : (TextEvent.bIsFunctionWord ? FName(TEXT("function")) : FName(TEXT("content")));
        Added.PlannerProsodyGroupWeight = 1.0f;
        Added.PlannerProsodyGroupAllocatedSeconds = IslandTextSpanSec;
        Added.PlannerProsodyGroupEventCount = TextPlan.Events.Num();
        Added.PlannerProsodyGroupEventCountModelSeconds = IslandTextSpanSec;
        Added.PlannerProsodyIslandSpeechBudgetSeconds = IslandTextSpanSec;
        Added.PlannerProsodyAllocationScale = 1.0f;
        Added.PlannerDurationBucket = FName(TEXT("O01_region_duration_pacing"));
        Added.PlannerDurationScaleApplied = IslandTextSpanSec > 0.001f ? EffectiveIslandActiveSpanSec / IslandTextSpanSec : 1.0f;
        Added.PlannerIslandUnscaledDurationSeconds = IslandTextSpanSec;

        InOutTrack.Events.Add(Added);
        LastPublishedCenterSec = CenterSec;
        LastPublishedAudioIndex = AudioIslandIndex;
    }

    EnforceTrackMonotonicity(InOutTrack);
    if (InOutTrack.Events.Num() > 0)
    {
        InOutTrack.SpeechStartSeconds = InOutTrack.Events[0].FinalRenderCenterSeconds;
        InOutTrack.SpeechEndSeconds = CurrentCommittedSpeechEnd(InOutTrack);
    }
    else
    {
        InOutTrack.SpeechStartSeconds = 0.0f;
        InOutTrack.SpeechEndSeconds = 0.0f;
    }
    bOutHasTrack = true;
}

void FOffgridAIAudioOccupancyScheduler::CollectAudioOccupancyDiagnosticRows(
    const FOffgridAITextVisemePlan& TextPlan,
    const TArray<FOffgridAIStreamingSpeechIsland>& AudioIslands,
    const TArray<FOffgridAIStreamingAudioFeatureFrame>* AudioFeatureFrames,
    const FOffgridAIAlignedVisemeTrack& Track,
    const FOffgridAIAudioOccupancySchedulerState& SchedulerState,
    float CurrentPlaybackSec,
    float PrerollSec,
    bool bFinalReplay,
    int32 UpdateOrdinal,
    TArray<FOffgridAIAudioOccupancyDiagnosticRow>& OutRows)
{
    (void)TextPlan;
    (void)AudioFeatureFrames;
    (void)SchedulerState;

    OutRows.Reset();
    for (const FOffgridAIAlignedVisemeEvent& Event : Track.Events)
    {
        FOffgridAIAudioOccupancyDiagnosticRow Row;
        Row.LineID = Track.LineID;
        Row.UpdateOrdinal = UpdateOrdinal;
        Row.bFinalReplay = bFinalReplay;
        Row.CurrentPlaybackSec = CurrentPlaybackSec;
        Row.PrerollSec = PrerollSec;
        Row.SourceEventIndex = Event.EventIndex;
        Row.Word = Event.SourceWord;
        Row.PoseID = Event.PoseID;
        Row.PlannedCenterSec = Event.TextDiagnosticCenterSeconds;
        Row.CommittedCenterSec = Event.FinalRenderCenterSeconds;
        Row.RenderStartSec = Event.RenderStartSeconds;
        Row.RenderEndSec = Event.RenderEndSeconds;
        Row.CommitReason = Event.CommitReason;
        Row.PlaybackMode = PlaybackModeForCommitReason(Event.CommitReason);
        Row.SpeechIslandIndex = Event.AudioIslandIndex;
        if (AudioIslands.IsValidIndex(Event.AudioIslandIndex))
        {
            Row.SpeechIslandStartSec = AudioIslandStartSec(AudioIslands[Event.AudioIslandIndex]);
            Row.SpeechIslandEndSec = AudioIslandEndSec(AudioIslands[Event.AudioIslandIndex]);
        }
        Row.AudioActiveSec = Event.ObservedActiveElapsedSeconds;
        Row.TextPlayheadSec = Event.TextDiagnosticCenterSeconds;
        Row.RequiredActiveElapsedSec = Event.RequiredActiveElapsedSeconds;
        Row.ObservedActiveElapsedSec = Event.ObservedActiveElapsedSeconds;
        Row.ActiveProgressDeficitSec = Event.ActiveProgressDeficitSeconds;
        Row.RequiredProgressNorm = Event.RequiredProgressNorm;
        Row.ObservedProgressNorm = Event.ObservedProgressNorm;
        Row.ActiveProgressRatio = Event.ActiveProgressRatio;
        Row.bMappedToObservedSpeech = Event.bMappedToObservedSpeech;
        Row.bTailDrain = Row.PlaybackMode == FName(TEXT("tail_drain"));
        Row.DiagnosticKind = bFinalReplay ? FName(TEXT("M09_audio_occupancy_final")) : FName(TEXT("M09_audio_occupancy_live"));
        OutRows.Add(Row);
    }
}
