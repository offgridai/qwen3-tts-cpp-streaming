#include "Lipsync/OffgridAILipsyncRuntimeAdapter.h"
#include "Lipsync/OffgridAIArticulationBudgeter.h"

namespace
{
    struct FRuntimeCommitStamp
    {
        float CommitPlaybackSeconds = 0.0f;
        float CommitLeadSeconds = 0.0f;
        float EffectiveCommitHorizonSeconds = 0.0f;
        FName CommitReason = FName(TEXT("unknown"));
    };

    static bool IsPreservableCommitReason(FName Reason)
    {
        return Reason == FName(TEXT("within_horizon"))
            || Reason == FName(TEXT("late_publication"))
            || Reason == FName(TEXT("end_of_stream_flush"));
    }

    static FName ClassifyRuntimeCommitReason(const FOffgridAILipsyncRuntimeUpdateInput& Input, const FOffgridAIAlignedVisemeEvent& Event)
    {
        if (Input.bInputStreamClosed)
        {
            return FName(TEXT("end_of_stream_flush"));
        }
        if (Event.AudioIslandIndex == INDEX_NONE || Event.AlignmentConfidence < 0.5f)
        {
            return FName(TEXT("fallback_or_missing_audio"));
        }
        if (Event.FinalRenderCenterSeconds < Input.CurrentPlaybackSec - 0.001f)
        {
            return FName(TEXT("late_publication"));
        }
        const float HorizonSec = Input.CurrentPlaybackSec + FMath::Max(0.0f, Input.PrerollSec);
        if (Event.FinalRenderCenterSeconds <= HorizonSec + 0.001f)
        {
            return FName(TEXT("within_horizon"));
        }
        return FName(TEXT("released_beyond_horizon"));
    }

    static void PreserveOrStampRuntimeCommitForensics(
        const FOffgridAILipsyncRuntimeUpdateInput& Input,
        const TMap<int32, FRuntimeCommitStamp>& ExistingStamps,
        FOffgridAIAlignedVisemeTrack& Track)
    {
        for (FOffgridAIAlignedVisemeEvent& Event : Track.Events)
        {
            if (const FRuntimeCommitStamp* Existing = ExistingStamps.Find(Event.EventIndex))
            {
                Event.CommitPlaybackSeconds = Existing->CommitPlaybackSeconds;
                Event.CommitLeadSeconds = Existing->CommitLeadSeconds;
                Event.EffectiveCommitHorizonSeconds = Existing->EffectiveCommitHorizonSeconds;
                Event.CommitReason = Existing->CommitReason;
                continue;
            }

            Event.EffectiveCommitHorizonSeconds = Input.PrerollSec;
            Event.CommitReason = ClassifyRuntimeCommitReason(Input, Event);
            if (Event.CommitReason == FName(TEXT("released_beyond_horizon")))
            {
                Event.CommitPlaybackSeconds = -1.0f;
                Event.CommitLeadSeconds = -1.0f;
            }
            else
            {
                Event.CommitPlaybackSeconds = Input.CurrentPlaybackSec;
                Event.CommitLeadSeconds = Event.FinalRenderCenterSeconds - Input.CurrentPlaybackSec;
            }
        }
    }
}

void FOffgridAILipsyncRuntimeAdapter::UpdateCommittedTrack(
    const FOffgridAILipsyncRuntimeUpdateInput& Input,
    FOffgridAIAlignedVisemeTrack& InOutTrack,
    bool& bInOutTrackBuilt)
{
    if (!Input.TextPlan || !Input.SpeechIslands || Input.TextPlan->Events.Num() <= 0)
    {
        return;
    }

    if (InOutTrack.LineID.IsNone())
    {
        InOutTrack.LineID = Input.LineID;
        InOutTrack.NPCID = Input.NPCID;
    }

    TMap<int32, FRuntimeCommitStamp> ExistingStamps;
    for (const FOffgridAIAlignedVisemeEvent& ExistingEvent : InOutTrack.Events)
    {
        if (!IsPreservableCommitReason(ExistingEvent.CommitReason))
        {
            continue;
        }

        FRuntimeCommitStamp Stamp;
        Stamp.CommitPlaybackSeconds = ExistingEvent.CommitPlaybackSeconds;
        Stamp.CommitLeadSeconds = ExistingEvent.CommitLeadSeconds;
        Stamp.EffectiveCommitHorizonSeconds = ExistingEvent.EffectiveCommitHorizonSeconds;
        Stamp.CommitReason = ExistingEvent.CommitReason;
        ExistingStamps.Add(ExistingEvent.EventIndex, Stamp);
    }

    FOffgridAIAudioOccupancySchedulerState LocalEphemeralState;
    FOffgridAIAudioOccupancySchedulerState& SchedulerState = Input.AudioOccupancyState ? *Input.AudioOccupancyState : LocalEphemeralState;

    FOffgridAIAudioOccupancyScheduler::UpdateCommittedTrack(
        *Input.TextPlan,
        *Input.SpeechIslands,
        Input.AudioFeatureFrames,
        Input.CurrentPlaybackSec,
        Input.PrerollSec,
        Input.IslandDurationScale,
        Input.bInputStreamClosed,
        SchedulerState,
        InOutTrack,
        bInOutTrackBuilt);

    PreserveOrStampRuntimeCommitForensics(Input, ExistingStamps, InOutTrack);
}

void FOffgridAILipsyncRuntimeSession::Reset()
{
    NPCID = NAME_None;
    LineID = NAME_None;
    DialogueText = FString();
    PrerollSec = 0.150f;
    IslandDurationScale = 1.0f;
    PlaybackSec = 0.0f;
    bBegun = false;
    bPlaybackStarted = false;
    bCommittedTrackBuilt = false;
    TextPlan = FOffgridAITextVisemePlan();
    Detector.Reset();
    CommittedTrack = FOffgridAIAlignedVisemeTrack();
    AudioOccupancyState.Reset();
    AudioOccupancyDiagnosticRows.Reset();
    AudioOccupancyDiagnosticUpdateOrdinal = 0;
    StreamTailDiagnosticRow = FOffgridAIStreamTailDiagnosticRow();
    PCMChunkCount = 0;
    PCMBytesReceived = 0;
    PCMSamplesReceived = 0;
    LastPCMChunkSampleRate = 0;
    LastPCMChunkChannels = 0;
    LastPCMChunkStartSample = -1;
    LastPCMChunkEndSample = -1;
}

void FOffgridAILipsyncRuntimeSession::BeginLine(const FOffgridAILipsyncRuntimeBeginInput& Input)
{
    Reset();
    bBegun = true;
    NPCID = Input.NPCID;
    LineID = Input.LineID;
    DialogueText = Input.DialogueText;
    PrerollSec = FMath::Max(0.0f, Input.PrerollSec);
    IslandDurationScale = FMath::Clamp(Input.IslandDurationScale, 0.75f, 1.35f);

    TextPlan = FOffgridAITextVisemePlanner::BuildPlan(FText::FromString(DialogueText));
    TextPlan = FOffgridAIArticulationBudgeter::Budget(TextPlan);

    CommittedTrack.NPCID = NPCID;
    CommittedTrack.LineID = LineID;
}

void FOffgridAILipsyncRuntimeSession::UpdatePlaybackGate(float ObservedEndSec)
{
    if (bPlaybackStarted || !Detector.HasObservedFirstSpeechStart())
    {
        return;
    }

    if (ObservedEndSec >= Detector.GetFirstSpeechAudioBufferStartSec() + PrerollSec)
    {
        bPlaybackStarted = true;
        PlaybackSec = 0.0f;
    }
}

void FOffgridAILipsyncRuntimeSession::PushAudioPCM16(
    const TArray<uint8>& PCMChunk,
    int32 BytesToUse,
    int32 SampleRate,
    int32 NumChannels,
    int64 ChunkStartSample)
{
    if (!bBegun)
    {
        return;
    }

    PCMChunkCount += 1;
    PCMBytesReceived += FMath::Max(0, BytesToUse);
    LastPCMChunkSampleRate = SampleRate;
    LastPCMChunkChannels = NumChannels;
    const int32 BytesPerSampleFrame = FMath::Max(1, NumChannels) * static_cast<int32>(sizeof(int16));
    const int64 ChunkFrames = BytesPerSampleFrame > 0 ? static_cast<int64>(FMath::Max(0, BytesToUse) / BytesPerSampleFrame) : 0;
    PCMSamplesReceived += ChunkFrames;
    LastPCMChunkStartSample = ChunkStartSample;
    LastPCMChunkEndSample = ChunkStartSample >= 0 ? (ChunkStartSample + ChunkFrames) : -1;

    Detector.AppendPCM16(PCMChunk, BytesToUse, SampleRate, NumChannels, ChunkStartSample);
    UpdatePlaybackGate(Detector.GetObservedAudioBufferEndSec());
}

void FOffgridAILipsyncRuntimeSession::RecordAudioOccupancyDiagnostics(float CurrentPlaybackSec, bool bFinalReplay)
{
    TArray<FOffgridAIAudioOccupancyDiagnosticRow> Rows;
    FOffgridAIAudioOccupancyScheduler::CollectAudioOccupancyDiagnosticRows(
        TextPlan,
        Detector.GetIslands(),
        &Detector.GetFeatureFrames(),
        CommittedTrack,
        AudioOccupancyState,
        CurrentPlaybackSec,
        PrerollSec,
        bFinalReplay,
        AudioOccupancyDiagnosticUpdateOrdinal,
        Rows);

    AudioOccupancyDiagnosticRows.Append(Rows);
    AudioOccupancyDiagnosticUpdateOrdinal += 1;
}

void FOffgridAILipsyncRuntimeSession::Update(float CurrentPlaybackSec)
{
    if (!bBegun || !bPlaybackStarted)
    {
        return;
    }

    FOffgridAILipsyncRuntimeUpdateInput Input;
    Input.TextPlan = &TextPlan;
    Input.SpeechIslands = &Detector.GetIslands();
    Input.AudioFeatureFrames = &Detector.GetFeatureFrames();
    Input.bInputStreamClosed = false;
    Input.CurrentPlaybackSec = CurrentPlaybackSec;
    Input.PrerollSec = PrerollSec;
    Input.IslandDurationScale = IslandDurationScale;
    Input.NPCID = NPCID;
    Input.LineID = LineID;
    Input.AudioOccupancyState = &AudioOccupancyState;

    FOffgridAILipsyncRuntimeAdapter::UpdateCommittedTrack(Input, CommittedTrack, bCommittedTrackBuilt);
    PlaybackSec = CurrentPlaybackSec;
    RecordAudioOccupancyDiagnostics(CurrentPlaybackSec, false);
}

void FOffgridAILipsyncRuntimeSession::Finalize(float FinalPlaybackSec)
{
    if (!bBegun)
    {
        return;
    }

    Detector.Finalize();

    if (!bPlaybackStarted && Detector.HasObservedFirstSpeechStart())
    {
        bPlaybackStarted = true;
        PlaybackSec = 0.0f;
    }

    FOffgridAILipsyncRuntimeUpdateInput Input;
    Input.TextPlan = &TextPlan;
    Input.SpeechIslands = &Detector.GetIslands();
    Input.AudioFeatureFrames = &Detector.GetFeatureFrames();
    Input.bInputStreamClosed = true;
    Input.CurrentPlaybackSec = FinalPlaybackSec;
    Input.PrerollSec = PrerollSec;
    Input.IslandDurationScale = IslandDurationScale;
    Input.NPCID = NPCID;
    Input.LineID = LineID;
    Input.AudioOccupancyState = &AudioOccupancyState;

    FOffgridAILipsyncRuntimeAdapter::UpdateCommittedTrack(Input, CommittedTrack, bCommittedTrackBuilt);
    RecordAudioOccupancyDiagnostics(FinalPlaybackSec, true);

    StreamTailDiagnosticRow.LineID = LineID;
    StreamTailDiagnosticRow.PCMChunkCount = PCMChunkCount;
    StreamTailDiagnosticRow.PCMBytesReceived = PCMBytesReceived;
    StreamTailDiagnosticRow.PCMSamplesReceived = PCMSamplesReceived;
    StreamTailDiagnosticRow.LastSampleRate = LastPCMChunkSampleRate;
    StreamTailDiagnosticRow.LastNumChannels = LastPCMChunkChannels;
    StreamTailDiagnosticRow.LastChunkStartSample = LastPCMChunkStartSample;
    StreamTailDiagnosticRow.LastChunkEndSample = LastPCMChunkEndSample;
    StreamTailDiagnosticRow.ObservedAudioBufferEndSec = Detector.GetObservedAudioBufferEndSec();
    StreamTailDiagnosticRow.FirstSpeechAudioBufferStartSec = Detector.HasObservedFirstSpeechStart() ? Detector.GetFirstSpeechAudioBufferStartSec() : 0.0f;
    StreamTailDiagnosticRow.SpeechIslandCount = Detector.GetIslands().Num();
    StreamTailDiagnosticRow.bInputStreamClosed = true;
    StreamTailDiagnosticRow.DiagnosticKind = FName(TEXT("M09_stream_tail_summary"));

    PlaybackSec = FinalPlaybackSec;
}
