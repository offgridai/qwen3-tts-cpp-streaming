#include "Core/OffgridAIServiceSupervisor.h"

#include "OffgridAI.h"

FOffgridAIServiceSupervisor::FOffgridAIServiceSupervisor(
    EOffgridAIServiceKind InServiceKind,
    EOffgridAIServiceImplementation InImplementation,
    const FOffgridAIServiceSupervisorSettings& InSettings,
    FEventSink InEventSink,
    FStatusSink InStatusSink)
    : ServiceKind(InServiceKind)
    , Implementation(InImplementation)
    , Settings(InSettings)
    , EventSink(MoveTemp(InEventSink))
    , StatusSink(MoveTemp(InStatusSink))
{
    Status.ServiceKind = ServiceKind;
    Status.State = EOffgridAIServiceState::Stopped;
    Status.Implementation = Implementation;
    Status.bIsRequired = Settings.bRequired;
}

void FOffgridAIServiceSupervisor::Start(double NowSeconds)
{
    StartRequestedAtSeconds = NowSeconds;
    LastSupervisorTickAtSeconds = NowSeconds;
    LastHeartbeatAtSeconds = NowSeconds;
    ActiveRequestID.Reset();
    ActiveConversationID.Invalidate();
    Status.bHasPendingRequest = false;
    Status.SecondsSinceLastHeartbeat = 0.0f;
    Status.LastError.Reset();

    if (Implementation == EOffgridAIServiceImplementation::Real)
    {
        // Real is allowed to proceed through the normal runtime path.
    }

    StartupReadyAtSeconds = NowSeconds + static_cast<double>(FMath::Max(0.0f, Settings.StartupDelaySeconds));
    EnterState(EOffgridAIServiceState::Starting, NowSeconds, TEXT("Service start requested."), EOffgridAIServiceEventSeverity::Log);
}

void FOffgridAIServiceSupervisor::Stop(double NowSeconds)
{
    ActiveRequestID.Reset();
    ActiveConversationID.Invalidate();
    Status.bHasPendingRequest = false;
    EnterState(EOffgridAIServiceState::Stopped, NowSeconds, TEXT("Service stopped."), EOffgridAIServiceEventSeverity::Log);
}

void FOffgridAIServiceSupervisor::Tick(double NowSeconds)
{
    const double PreviousTickSeconds = LastSupervisorTickAtSeconds;
    LastSupervisorTickAtSeconds = NowSeconds;

    const double SecondsSinceHeartbeat = FMath::Max(0.0, NowSeconds - LastHeartbeatAtSeconds);
    Status.SecondsSinceLastHeartbeat = static_cast<float>(SecondsSinceHeartbeat);

    switch (Status.State)
    {
    case EOffgridAIServiceState::Starting:
        if ((NowSeconds - StartRequestedAtSeconds) > static_cast<double>(Settings.StartupTimeoutSeconds))
        {
            ScheduleRestart(NowSeconds, TEXT("Startup timed out before Ready."));
            return;
        }

        if (Implementation == EOffgridAIServiceImplementation::Stub && NowSeconds >= StartupReadyAtSeconds)
        {
            NoteHeartbeat(NowSeconds);
            EnterState(EOffgridAIServiceState::Ready, NowSeconds, TEXT("Ready handshake completed."), EOffgridAIServiceEventSeverity::Log);
            return;
        }
        break;

    case EOffgridAIServiceState::Busy:
        // Busy requests are governed by the request timeout, not by heartbeat timeout.
        // Streaming TTS can legitimately occupy the service for multiple seconds while
        // still producing audio.  If it really stalls, RequestTimeoutSeconds is the
        // correct failure boundary.
        if ((NowSeconds - ActiveRequestStartedAtSeconds) > static_cast<double>(Settings.RequestTimeoutSeconds))
        {
            ScheduleRestart(NowSeconds, FString::Printf(TEXT("Active request %s timed out."), *ActiveRequestID));
            return;
        }
        break;

    case EOffgridAIServiceState::Restarting:
        if (NowSeconds >= RestartReadyAtSeconds)
        {
            Start(NowSeconds);
            return;
        }
        break;

    case EOffgridAIServiceState::Ready:
        break;

    default:
        break;
    }

    if (Status.State == EOffgridAIServiceState::Ready || Status.State == EOffgridAIServiceState::Busy)
    {
        // IMPORTANT:
        // The old supervisor treated a missed synthetic heartbeat as a hard service
        // failure.  That is wrong for these local process services because heartbeat
        // ticking is driven by the UE game-thread ticker.  A long LLM call, startup
        // hitch, shader/audio initialization hitch, or debugger pause could make the
        // next tick arrive after HeartbeatTimeoutSeconds, causing a healthy TTS service
        // to be restarted immediately before the first NPC line.  That produced the
        // observed hot-start failure: TTS was Ready, then got restarted during
        // ProcessingLLM, then DispatchNPCLine had to wait ~6s for TTS to reload.
        //
        // Keep publishing heartbeat/status freshness, but do not restart purely from
        // the synthetic heartbeat clock. Real failure paths remain:
        // - startup timeout while Starting
        // - request timeout while Busy
        // - explicit service rejection / stream error via FailRequest
        // - fatal restart-window exhaustion
        if (SecondsSinceHeartbeat >= static_cast<double>(Settings.HeartbeatIntervalSeconds))
        {
            NoteHeartbeat(NowSeconds);
        }
    }
}


void FOffgridAIServiceSupervisor::ConfirmStartupReady(double NowSeconds, const FString& Message)
{
    if (Status.State != EOffgridAIServiceState::Starting)
    {
        return;
    }

    NoteHeartbeat(NowSeconds);
    EnterState(EOffgridAIServiceState::Ready, NowSeconds, Message, EOffgridAIServiceEventSeverity::Log);
}

void FOffgridAIServiceSupervisor::FailStartup(const FString& Reason, double NowSeconds)
{
    if (Status.State != EOffgridAIServiceState::Starting)
    {
        return;
    }

    ScheduleRestart(NowSeconds, Reason);
}

bool FOffgridAIServiceSupervisor::CanAcceptRequest() const
{
    return Status.State == EOffgridAIServiceState::Ready;
}

bool FOffgridAIServiceSupervisor::BeginRequest(const FString& RequestID, const FGuid& ConversationID, double NowSeconds)
{
    if (!CanAcceptRequest())
    {
        return false;
    }

    ActiveRequestID = RequestID;
    ActiveConversationID = ConversationID;
    ActiveRequestStartedAtSeconds = NowSeconds;
    Status.bHasPendingRequest = true;
    NoteHeartbeat(NowSeconds);
    EnterState(EOffgridAIServiceState::Busy, NowSeconds, FString::Printf(TEXT("Request started: %s"), *RequestID), EOffgridAIServiceEventSeverity::Verbose);
    return true;
}

void FOffgridAIServiceSupervisor::CompleteRequest(double NowSeconds)
{
    if (!Status.bHasPendingRequest)
    {
        return;
    }

    const FString CompletedRequestID = ActiveRequestID;
    ActiveRequestID.Reset();
    ActiveConversationID.Invalidate();
    Status.bHasPendingRequest = false;
    NoteHeartbeat(NowSeconds);
    EnterState(EOffgridAIServiceState::Ready, NowSeconds, FString::Printf(TEXT("Request completed: %s"), *CompletedRequestID), EOffgridAIServiceEventSeverity::Verbose);
}

void FOffgridAIServiceSupervisor::FailRequest(const FString& Reason, double NowSeconds)
{
    ScheduleRestart(NowSeconds, Reason);
}

void FOffgridAIServiceSupervisor::NoteHeartbeat(double NowSeconds)
{
    LastHeartbeatAtSeconds = NowSeconds;
    Status.SecondsSinceLastHeartbeat = 0.0f;
    PublishStatus();
}

void FOffgridAIServiceSupervisor::ForceFatal(const FString& Reason, double NowSeconds)
{
    ActiveRequestID.Reset();
    ActiveConversationID.Invalidate();
    Status.bHasPendingRequest = false;
    Status.LastError = Reason;
    EnterState(EOffgridAIServiceState::Fatal, NowSeconds, Reason, EOffgridAIServiceEventSeverity::Error);
}

void FOffgridAIServiceSupervisor::EnterState(
    EOffgridAIServiceState NewState,
    double NowSeconds,
    const FString& Message,
    EOffgridAIServiceEventSeverity Severity)
{
    Status.State = NewState;
    if (Severity == EOffgridAIServiceEventSeverity::Error)
    {
        Status.LastError = Message;
    }

    PublishStatus();
    EmitEvent(Severity, TEXT("State"), Message);
}

void FOffgridAIServiceSupervisor::EmitEvent(EOffgridAIServiceEventSeverity Severity, const FString& Category, const FString& Message) const
{
    if (!EventSink)
    {
        return;
    }

    FOffgridAIServiceEvent Event;
    Event.ServiceKind = ServiceKind;
    Event.Severity = Severity;
    Event.Category = Category;
    Event.Message = Message;
    Event.RequestID = ActiveRequestID;
    Event.ConversationID = ActiveConversationID;
    Event.RestartCount = Status.RestartCount;
    EventSink(Event);
}

void FOffgridAIServiceSupervisor::PublishStatus() const
{
    if (StatusSink)
    {
        StatusSink(Status);
    }
}

bool FOffgridAIServiceSupervisor::CanRestart(double NowSeconds) const
{
    int32 RecentRestartCount = 0;
    for (const double RestartTime : RestartHistorySeconds)
    {
        if ((NowSeconds - RestartTime) <= static_cast<double>(Settings.RestartWindowSeconds))
        {
            ++RecentRestartCount;
        }
    }

    return RecentRestartCount < Settings.MaxRestartsInWindow;
}

void FOffgridAIServiceSupervisor::ScheduleRestart(double NowSeconds, const FString& Reason)
{
    if (!CanRestart(NowSeconds))
    {
        ForceFatal(Reason, NowSeconds);
        return;
    }

    RestartHistorySeconds.RemoveAll([this, NowSeconds](const double RestartTime)
    {
        return (NowSeconds - RestartTime) > static_cast<double>(Settings.RestartWindowSeconds);
    });
    RestartHistorySeconds.Add(NowSeconds);

    ++Status.RestartCount;
    Status.LastError = Reason;
    ActiveRequestID.Reset();
    ActiveConversationID.Invalidate();
    Status.bHasPendingRequest = false;
    RestartReadyAtSeconds = NowSeconds + static_cast<double>(FMath::Max(0.0f, Settings.RestartCooldownSeconds));

    EnterState(EOffgridAIServiceState::Restarting, NowSeconds, Reason, EOffgridAIServiceEventSeverity::Warning);
}
