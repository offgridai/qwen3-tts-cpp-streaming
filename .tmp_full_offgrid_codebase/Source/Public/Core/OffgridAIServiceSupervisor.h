#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

#include "Core/OffgridAITypes.h"

/*
One runtime owner per service kind.

This object does not perform ASR / LLM / TTS work itself. Instead it owns the
operational state for one service channel:
- startup / ready handshake
- heartbeat tracking
- request timeout tracking
- restart policy
- status / event publication

The selected concrete implementation is represented today by the
EOffgridAIServiceImplementation enum. For now only the stub path is implemented.
Later the runtime can host a real backing without changing the orchestrator or
service adapter contract.
*/
class OFFGRIDAI_API FOffgridAIServiceSupervisor
{
public:
    using FEventSink = TFunction<void(const FOffgridAIServiceEvent&)>;
    using FStatusSink = TFunction<void(const FOffgridAIServiceStatus&)>;

    FOffgridAIServiceSupervisor(
        EOffgridAIServiceKind InServiceKind,
        EOffgridAIServiceImplementation InImplementation,
        const FOffgridAIServiceSupervisorSettings& InSettings,
        FEventSink InEventSink,
        FStatusSink InStatusSink);

    void Start(double NowSeconds);
    void Stop(double NowSeconds);
    void Tick(double NowSeconds);

    bool CanAcceptRequest() const;
    void ConfirmStartupReady(double NowSeconds, const FString& Message = TEXT("Ready handshake completed."));
    void FailStartup(const FString& Reason, double NowSeconds);
    bool BeginRequest(const FString& RequestID, const FGuid& ConversationID, double NowSeconds);
    void CompleteRequest(double NowSeconds);
    void FailRequest(const FString& Reason, double NowSeconds);

    void NoteHeartbeat(double NowSeconds);
    void ForceFatal(const FString& Reason, double NowSeconds);

    const FOffgridAIServiceStatus& GetStatus() const { return Status; }
    const FString& GetActiveRequestID() const { return ActiveRequestID; }
    const FGuid& GetActiveConversationID() const { return ActiveConversationID; }

private:
    void EnterState(EOffgridAIServiceState NewState, double NowSeconds, const FString& Message, EOffgridAIServiceEventSeverity Severity);
    void EmitEvent(EOffgridAIServiceEventSeverity Severity, const FString& Category, const FString& Message) const;
    void PublishStatus() const;
    bool CanRestart(double NowSeconds) const;
    void ScheduleRestart(double NowSeconds, const FString& Reason);

    EOffgridAIServiceKind ServiceKind;
    EOffgridAIServiceImplementation Implementation;
    FOffgridAIServiceSupervisorSettings Settings;
    FEventSink EventSink;
    FStatusSink StatusSink;

    FOffgridAIServiceStatus Status;
    FString ActiveRequestID;
    FGuid ActiveConversationID;

    double StartRequestedAtSeconds = 0.0;
    double StartupReadyAtSeconds = 0.0;
    double RestartReadyAtSeconds = 0.0;
    double LastHeartbeatAtSeconds = 0.0;
    double ActiveRequestStartedAtSeconds = 0.0;
    double LastSupervisorTickAtSeconds = 0.0;

    TArray<double> RestartHistorySeconds;
};
