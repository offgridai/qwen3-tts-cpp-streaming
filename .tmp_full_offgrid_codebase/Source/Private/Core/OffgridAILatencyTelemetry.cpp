#include "Core/OffgridAILatencyTelemetry.h"

#include "OffgridAI.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    double NowSeconds()
    {
        return FPlatformTime::Seconds();
    }
}

FOffgridAIMetricAccumulator::FOffgridAIMetricAccumulator(int32 InMaxSamplesPerMetric)
{
    SetMaxSamplesPerMetric(InMaxSamplesPerMetric);
}

void FOffgridAIMetricAccumulator::Reset()
{
    SamplesByMetric.Reset();
}

void FOffgridAIMetricAccumulator::SetMaxSamplesPerMetric(int32 InMaxSamplesPerMetric)
{
    MaxSamplesPerMetric = FMath::Clamp(InMaxSamplesPerMetric, 10, 1000);
}

void FOffgridAIMetricAccumulator::AddSample(const FString& MetricName, double ValueMs)
{
    if (MetricName.IsEmpty())
    {
        return;
    }

    TArray<double>& Samples = SamplesByMetric.FindOrAdd(MetricName);
    Samples.Add(FMath::Max(ValueMs, 0.0));

    while (Samples.Num() > MaxSamplesPerMetric)
    {
        Samples.RemoveAt(0, 1, EAllowShrinking::No);
    }
}

bool FOffgridAIMetricAccumulator::GetWindow(const FString& MetricName, FOffgridAIMetricWindow& OutWindow) const
{
    const TArray<double>* Samples = SamplesByMetric.Find(MetricName);
    if (!Samples || Samples->IsEmpty())
    {
        OutWindow = FOffgridAIMetricWindow();
        return false;
    }

    OutWindow.SampleCount = Samples->Num();
    OutWindow.LastMs = (*Samples)[Samples->Num() - 1];
    OutWindow.MedianMs = Percentile(*Samples, 0.50);
    OutWindow.P90Ms = Percentile(*Samples, 0.90);
    return true;
}

FString FOffgridAIMetricAccumulator::BuildLogSummary() const
{
    static const TCHAR* MetricOrder[] =
    {
        TEXT("RoundTripLatency"),
        TEXT("PerceivedLatency"),
        TEXT("ASRLatency"),
        TEXT("LLMTotalLatency"),
        TEXT("TTSFirstAudioLatency"),
        TEXT("PlaybackDelay"),
        TEXT("TurnTotalLatency")
    };

    TArray<FString> Parts;
    for (const TCHAR* MetricName : MetricOrder)
    {
        FOffgridAIMetricWindow Window;
        if (GetWindow(MetricName, Window))
        {
            Parts.Add(FString::Printf(TEXT("%s last=%.1fms median=%.1fms p90=%.1fms n=%d"),
                MetricName, Window.LastMs, Window.MedianMs, Window.P90Ms, Window.SampleCount));
        }
    }

    return FString::Join(Parts, TEXT(" | "));
}

double FOffgridAIMetricAccumulator::Percentile(TArray<double> Values, double Percentile01)
{
    if (Values.IsEmpty())
    {
        return 0.0;
    }

    Values.Sort();
    if (Values.Num() == 1)
    {
        return Values[0];
    }

    const double Clamped = FMath::Clamp(Percentile01, 0.0, 1.0);
    const double Position = Clamped * static_cast<double>(Values.Num() - 1);
    const int32 LowerIndex = FMath::FloorToInt(Position);
    const int32 UpperIndex = FMath::CeilToInt(Position);
    const double Alpha = Position - static_cast<double>(LowerIndex);
    return FMath::Lerp(Values[LowerIndex], Values[UpperIndex], Alpha);
}

void FOffgridAITurnLatencyTrace::Reset()
{
    bActive = false;
    TurnIndex = 0;
    ConversationID = FGuid();
    PlayerID = NAME_None;
    CurrentNPCID = NAME_None;
    CurrentLineID = NAME_None;
    StartedAtSeconds = 0.0;
    Marks.Reset();
}

void FOffgridAITurnLatencyTrace::BeginTurn(int32 InTurnIndex, const FGuid& InConversationID, FName InPlayerID)
{
    Reset();
    bActive = true;
    TurnIndex = InTurnIndex;
    ConversationID = InConversationID;
    PlayerID = InPlayerID;
    StartedAtSeconds = NowSeconds();
}

bool FOffgridAITurnLatencyTrace::HasFirstMark(const FString& Name) const
{
    return FindFirstMark(Name) != nullptr;
}

void FOffgridAITurnLatencyTrace::Mark(const FString& Name, const FString& Detail)
{
    if (!bActive || Name.IsEmpty())
    {
        return;
    }

    FOffgridAILatencyMark& MarkRef = Marks.AddDefaulted_GetRef();
    MarkRef.Name = Name;
    MarkRef.TimeSeconds = NowSeconds();
    MarkRef.Detail = Detail;
}

void FOffgridAITurnLatencyTrace::SetCurrentLine(FName InNPCID, FName InLineID)
{
    CurrentNPCID = InNPCID;
    CurrentLineID = InLineID;
}

void FOffgridAITurnLatencyTrace::EmitAndReset(const FString& Outcome)
{
    if (!bActive)
    {
        return;
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[Latency][TurnSummary] %s"), *BuildSummary(Outcome));
    UE_LOG(LogOffgridAI, Log, TEXT("[Latency][TurnTimeline] %s"), *BuildTimelineJson(Outcome));

    Reset();
}

const FOffgridAILatencyMark* FOffgridAITurnLatencyTrace::FindFirstMark(const FString& Name) const
{
    for (const FOffgridAILatencyMark& MarkRef : Marks)
    {
        if (MarkRef.Name == Name)
        {
            return &MarkRef;
        }
    }

    return nullptr;
}

bool FOffgridAITurnLatencyTrace::TryGetDeltaMs(const FString& StartMark, const FString& EndMark, double& OutMs) const
{
    const FOffgridAILatencyMark* Start = FindFirstMark(StartMark);
    const FOffgridAILatencyMark* End = FindFirstMark(EndMark);
    if (!Start || !End)
    {
        return false;
    }

    OutMs = FMath::Max((End->TimeSeconds - Start->TimeSeconds) * 1000.0, 0.0);
    return true;
}

void FOffgridAITurnLatencyTrace::GetCanonicalMetricSamples(TArray<FOffgridAIMetricSample>& OutSamples) const
{
    auto AddDelta = [this, &OutSamples](const TCHAR* MetricName, const TCHAR* StartMark, const TCHAR* EndMark)
    {
        double ValueMs = 0.0;
        if (TryGetDeltaMs(StartMark, EndMark, ValueMs))
        {
            FOffgridAIMetricSample& Sample = OutSamples.AddDefaulted_GetRef();
            Sample.Name = MetricName;
            Sample.ValueMs = ValueMs;
        }
    };

    AddDelta(TEXT("RoundTripLatency"), TEXT("ASRFinalizeSent"), TEXT("FirstAudioSample"));
    AddDelta(TEXT("PerceivedLatency"), TEXT("ASRFinalizeSent"), TEXT("FirstAudioSample"));
    AddDelta(TEXT("ASRLatency"), TEXT("ASRFinalizeSent"), TEXT("ASRFinalizeReturned"));
    AddDelta(TEXT("LLMFirstTokenLatency"), TEXT("LLMRequestSent"), TEXT("LLMFirstTokenReceived"));
    AddDelta(TEXT("LLMTotalLatency"), TEXT("LLMRequestSent"), TEXT("LLMResponseReceived"));
    AddDelta(TEXT("TTSFirstAudioLatency"), TEXT("TTSRequestSent"), TEXT("FirstAudioChunkReceived"));
    AddDelta(TEXT("PlaybackDelay"), TEXT("FirstAudioSample"), TEXT("PlaybackStarted"));
    AddDelta(TEXT("TurnTotalLatency"), TEXT("PTTPressed"), TEXT("ReturnedToAwaitingInput"));
}

FString FOffgridAITurnLatencyTrace::FormatMetric(const FString& Name, double ValueMs)
{
    return FString::Printf(TEXT("%s=%.1fms"), *Name, ValueMs);
}

FString FOffgridAITurnLatencyTrace::BuildSummary(const FString& Outcome) const
{
    TArray<FString> Parts;
    Parts.Reserve(20);

    Parts.Add(FString::Printf(TEXT("turn=%d"), TurnIndex));
    Parts.Add(FString::Printf(TEXT("conversation=%s"), *ConversationID.ToString(EGuidFormats::DigitsWithHyphens)));
    Parts.Add(FString::Printf(TEXT("player=%s"), *PlayerID.ToString()));
    Parts.Add(FString::Printf(TEXT("outcome=%s"), Outcome.IsEmpty() ? TEXT("unknown") : *Outcome));

    if (CurrentNPCID != NAME_None)
    {
        Parts.Add(FString::Printf(TEXT("npc=%s"), *CurrentNPCID.ToString()));
    }
    if (CurrentLineID != NAME_None)
    {
        Parts.Add(FString::Printf(TEXT("line=%s"), *CurrentLineID.ToString()));
    }

    double ValueMs = 0.0;
    if (TryGetDeltaMs(TEXT("PTTPressed"), TEXT("PTTReleased"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("capture"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("PTTReleased"), TEXT("BoomOperatorFinalizeRequested"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("capture_tail"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("BoomOperatorFinalizeRequested"), TEXT("ASRFinalizeSent"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("boom_to_asr_finalize"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("ASRFinalizeSent"), TEXT("ASRFinalizeReturned"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("asr_finalize_call"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("ASRFinalizeSent"), TEXT("ASRFinalReceived"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("asr"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("PTTReleased"), TEXT("PlayerTranscriptBroadcast"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("to_player_text_visible"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("LLMRequestSent"), TEXT("LLMRequestReturned"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("llm_submit_call"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("LLMRequestSent"), TEXT("LLMResponseReceived"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("llm"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("PTTReleased"), TEXT("NPCTranscriptBroadcast"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("to_npc_text_visible"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("TTSRequestSent"), TEXT("TTSRequestReturned"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("tts_begin_call"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("TTSRequestSent"), TEXT("TTSStreamStarted"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("tts_start"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("PTTReleased"), TEXT("FirstAudioSample"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("to_first_audio_sample"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("FirstAudioSample"), TEXT("PlaybackStarted"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("playback_lead"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("PTTReleased"), TEXT("PlaybackStarted"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("to_playback_start"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("ASRFinalizeSent"), TEXT("FirstAudioSample"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("round_trip"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("ASRFinalizeSent"), TEXT("PlaybackStarted"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("perceived_latency"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("NPCLineDispatched"), TEXT("LineCompleted"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("line_exec"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("PTTReleased"), TEXT("ReturnedToAwaitingInput"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("post_release_total"), ValueMs));
    }
    if (TryGetDeltaMs(TEXT("PTTPressed"), TEXT("ReturnedToAwaitingInput"), ValueMs))
    {
        Parts.Add(FormatMetric(TEXT("turn_total"), ValueMs));
    }

    return FString::Join(Parts, TEXT(" "));
}

FString FOffgridAITurnLatencyTrace::BuildTimelineJson(const FString& Outcome) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("turn_index"), TurnIndex);
    Root->SetStringField(TEXT("conversation_id"), ConversationID.ToString(EGuidFormats::DigitsWithHyphens));
    Root->SetStringField(TEXT("player_id"), PlayerID.ToString());
    Root->SetStringField(TEXT("npc_id"), CurrentNPCID.ToString());
    Root->SetStringField(TEXT("line_id"), CurrentLineID.ToString());
    Root->SetStringField(TEXT("outcome"), Outcome);
    Root->SetNumberField(TEXT("started_at_seconds"), StartedAtSeconds);

    TArray<TSharedPtr<FJsonValue>> MarkValues;
    MarkValues.Reserve(Marks.Num());

    for (const FOffgridAILatencyMark& MarkRef : Marks)
    {
        TSharedRef<FJsonObject> MarkObject = MakeShared<FJsonObject>();
        MarkObject->SetStringField(TEXT("name"), MarkRef.Name);
        MarkObject->SetNumberField(TEXT("time_seconds"), MarkRef.TimeSeconds);
        MarkObject->SetNumberField(TEXT("relative_ms"), FMath::Max((MarkRef.TimeSeconds - StartedAtSeconds) * 1000.0, 0.0));
        MarkObject->SetStringField(TEXT("detail"), MarkRef.Detail);
        MarkValues.Add(MakeShared<FJsonValueObject>(MarkObject));
    }

    Root->SetArrayField(TEXT("marks"), MarkValues);

    FString OutJson;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
    FJsonSerializer::Serialize(Root, Writer);
    return OutJson;
}
