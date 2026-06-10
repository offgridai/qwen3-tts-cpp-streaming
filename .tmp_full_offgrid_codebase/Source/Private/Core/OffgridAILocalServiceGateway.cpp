#include "Core/OffgridAILocalServiceGateway.h"

#include "OffgridAI.h"
#include "ASR/IOffgridAIASRService.h"
#include "LLM/OffgridAILLMService.h"
#include "LLM/OffgridAILLMNamedPipeService.h"
#include "Core/OffgridAIOrchestrator.h"
#include "Core/OffgridAIServiceSupervisor.h"
#include "TTS/OffgridAITTSService.h"
#include "TTS/OffgridAITTSNamedPipeService.h"
#include "ASR/OffgridAIASRNamedPipeService.h"
#include "Data/OffgridAIASRServiceSettingsDataAsset.h"
#include "Data/OffgridAILLMServiceSettingsDataAsset.h"
#include "Data/OffgridAITTSServiceSettingsDataAsset.h"

namespace
{
    FString ServiceKindToString(const EOffgridAIServiceKind ServiceKind)
    {
        switch (ServiceKind)
        {
        case EOffgridAIServiceKind::ASR:
            return TEXT("ASR");
        case EOffgridAIServiceKind::LLM:
            return TEXT("LLM");
        case EOffgridAIServiceKind::TTS:
            return TEXT("TTS");
        default:
            return TEXT("Unknown");
        }
    }
}

FOffgridAILocalServiceGateway::FOffgridAILocalServiceGateway(
    const FOffgridAIServiceSelection& InSelection,
    const FOffgridAIServiceSupervisorSettings& InDefaultSettings,
    const UOffgridAIASRServiceSettingsDataAsset* InASRServiceSettings,
    const UOffgridAILLMServiceSettingsDataAsset* InLLMServiceSettings,
    const UOffgridAITTSServiceSettingsDataAsset* InTTSServiceSettings)
    : ServiceSelection(InSelection)
    , DefaultRuntimeSettings(InDefaultSettings)
    , ASRServiceSettings(InASRServiceSettings)
    , LLMServiceSettings(InLLMServiceSettings)
    , TTSServiceSettings(InTTSServiceSettings)
{
}

FOffgridAILocalServiceGateway::~FOffgridAILocalServiceGateway()
{
    Shutdown();
}

void FOffgridAILocalServiceGateway::Initialize(UOffgridAIOrchestrator* InOrchestrator)
{
    Shutdown();

    Orchestrator = InOrchestrator;

    const EOffgridAIServiceImplementation EffectiveASRImplementation = ResolveASRImplementation();
    const EOffgridAIServiceImplementation EffectiveLLMImplementation = ResolveLLMImplementation();
    const EOffgridAIServiceImplementation EffectiveTTSImplementation = ResolveTTSImplementation();

    auto MakeRuntime = [this](const EOffgridAIServiceKind Kind, const EOffgridAIServiceImplementation Implementation)
        {
            FOffgridAIServiceSupervisorSettings Settings = DefaultRuntimeSettings;
            Settings.bRequired = true;

            return MakeUnique<FOffgridAIServiceSupervisor>(
                Kind,
                Implementation,
                Settings,
                [this](const FOffgridAIServiceEvent& Event) { HandleServiceEvent(Event); },
                [this](const FOffgridAIServiceStatus& Status) { HandleServiceStatusChanged(Status); });
        };

    ASRRuntime = MakeRuntime(EOffgridAIServiceKind::ASR, EffectiveASRImplementation);
    LLMRuntime = MakeRuntime(EOffgridAIServiceKind::LLM, EffectiveLLMImplementation);
    TTSRuntime = MakeRuntime(EOffgridAIServiceKind::TTS, EffectiveTTSImplementation);

    ASRService = CreateASRService(EffectiveASRImplementation);
    LLMService = CreateLLMService(EffectiveLLMImplementation);
    TTSService = CreateTTSService(EffectiveTTSImplementation);

    TickHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FOffgridAILocalServiceGateway::HandleTicker));
}

void FOffgridAILocalServiceGateway::Shutdown()
{
    if (TickHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
    }

    const double NowSeconds = FPlatformTime::Seconds();

    if (ASRRuntime)
    {
        ASRRuntime->Stop(NowSeconds);
    }
    if (LLMRuntime)
    {
        LLMRuntime->Stop(NowSeconds);
    }
    if (TTSRuntime)
    {
        TTSRuntime->Stop(NowSeconds);
    }

    if (ASRService)
    {
        ASRService->Reset();
    }
    if (LLMService)
    {
        LLMService->Reset();
    }
    if (TTSService)
    {
        TTSService->Reset();
    }

    ASRService.Reset();
    LLMService.Reset();
    TTSService.Reset();

    ASRRuntime.Reset();
    LLMRuntime.Reset();
    TTSRuntime.Reset();

    CachedStatuses.Empty();
    ActiveASRInput = FOffgridAIASRActiveInput();
    Orchestrator.Reset();
}

void FOffgridAILocalServiceGateway::StartupServices()
{
    const double NowSeconds = FPlatformTime::Seconds();

    if (ASRRuntime)
    {
        ASRRuntime->Start(NowSeconds);
    }
    if (LLMRuntime)
    {
        LLMRuntime->Start(NowSeconds);
    }
    if (TTSRuntime)
    {
        TTSRuntime->Start(NowSeconds);
    }

    TickServiceStartup(NowSeconds);
}

void FOffgridAILocalServiceGateway::Tick(float DeltaTimeSeconds)
{
    const double NowSeconds = FPlatformTime::Seconds();

    if (ASRRuntime)
    {
        ASRRuntime->Tick(NowSeconds);
    }
    if (LLMRuntime)
    {
        LLMRuntime->Tick(NowSeconds);
    }
    if (TTSRuntime)
    {
        TTSRuntime->Tick(NowSeconds);
    }

    if (ASRService)
    {
        ASRService->Tick(NowSeconds);
    }
    if (LLMService)
    {
        LLMService->Tick(NowSeconds);
    }
    if (TTSService)
    {
        TTSService->Tick(NowSeconds);
    }

    TickServiceStartup(NowSeconds);
    TickASR(NowSeconds);
    TickLLM(NowSeconds);
    TickTTS(NowSeconds);
}

bool FOffgridAILocalServiceGateway::AreRequiredServicesReady() const
{
    for (const TPair<EOffgridAIServiceKind, FOffgridAIServiceStatus>& Pair : CachedStatuses)
    {
        if (Pair.Value.bIsRequired && Pair.Value.State != EOffgridAIServiceState::Ready)
        {
            return false;
        }
    }

    return CachedStatuses.Num() >= 3;
}

FOffgridAIServiceStatus FOffgridAILocalServiceGateway::GetServiceStatus(EOffgridAIServiceKind ServiceKind) const
{
    if (const FOffgridAIServiceStatus* FoundStatus = CachedStatuses.Find(ServiceKind))
    {
        return *FoundStatus;
    }

    FOffgridAIServiceStatus DefaultStatus;
    DefaultStatus.ServiceKind = ServiceKind;
    return DefaultStatus;
}

bool FOffgridAILocalServiceGateway::BeginPlayerAudioInput(const FGuid& ConversationID, FName PlayerID)
{
    if (!ASRRuntime || !ASRService || !ASRRuntime->CanAcceptRequest() || ActiveASRInput.bActive)
    {
        return false;
    }

    const FString RequestID = MakeRequestID(EOffgridAIServiceKind::ASR);
    const double NowSeconds = FPlatformTime::Seconds();
    if (!ASRRuntime->BeginRequest(RequestID, ConversationID, NowSeconds))
    {
        return false;
    }

    if (!ASRService->BeginPlayerAudioInput(ConversationID, PlayerID))
    {
        ASRRuntime->FailRequest(TEXT("ASR service rejected BeginPlayerAudioInput."), NowSeconds);
        return false;
    }

    ActiveASRInput.bActive = true;
    ActiveASRInput.RequestID = RequestID;
    ActiveASRInput.ConversationID = ConversationID;
    ActiveASRInput.PlayerID = PlayerID;
    return true;
}

void FOffgridAILocalServiceGateway::SubmitPlayerAudioChunk(const FGuid& ConversationID, FName PlayerID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels)
{
    if (!ASRService || !ActiveASRInput.bActive)
    {
        return;
    }

    if (ActiveASRInput.ConversationID != ConversationID || ActiveASRInput.PlayerID != PlayerID)
    {
        return;
    }

    ASRService->SubmitPlayerAudioChunk(ConversationID, PlayerID, PCMChunk, SampleRate, NumChannels);
}

void FOffgridAILocalServiceGateway::EndPlayerAudioInput(const FGuid& ConversationID, FName PlayerID)
{
    if (!ASRRuntime || !ASRService || !ActiveASRInput.bActive)
    {
        return;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    if (ActiveASRInput.ConversationID != ConversationID || ActiveASRInput.PlayerID != PlayerID)
    {
        ASRRuntime->FailRequest(TEXT("ASR input session mismatch on EndPlayerAudioInput."), NowSeconds);
        ActiveASRInput = FOffgridAIASRActiveInput();
        return;
    }

    if (!ASRService->EndPlayerAudioInput(ActiveASRInput.RequestID, ConversationID, PlayerID))
    {
        ASRRuntime->FailRequest(TEXT("ASR service rejected EndPlayerAudioInput."), NowSeconds);
        ActiveASRInput = FOffgridAIASRActiveInput();
        return;
    }

    ActiveASRInput = FOffgridAIASRActiveInput();
}

bool FOffgridAILocalServiceGateway::InitializeLLMSession(
    const FGuid& ConversationID,
    const TArray<FName>& NPCIDs,
    const UOffgridAIConversationPromptDataAsset* PromptAsset,
    const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord)
{
    return LLMService && LLMService->InitializeSession(ConversationID, NPCIDs, PromptAsset, CanonicalRecord);
}

bool FOffgridAILocalServiceGateway::SubmitLLMRequest(
    const FGuid& ConversationID,
    const TArray<FName>& NPCIDs,
    const FText& PlayerText,
    EOffgridAILLMRequestKind RequestKind,
    const UOffgridAIConversationPromptDataAsset* PromptAsset,
    const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
    const FString& PersistentEmotionState,
    const TArray<FName>& SupportedEmotionNames)
{
    if (!LLMRuntime || !LLMService || NPCIDs.IsEmpty())
    {
        return false;
    }

    const FString RequestID = MakeRequestID(EOffgridAIServiceKind::LLM);
    const double NowSeconds = FPlatformTime::Seconds();
    if (!LLMRuntime->BeginRequest(RequestID, ConversationID, NowSeconds))
    {
        return false;
    }

    if (!LLMService->SubmitRequest(RequestID, ConversationID, NPCIDs, PlayerText, RequestKind, PromptAsset, CanonicalRecord, PersistentEmotionState, SupportedEmotionNames))
    {
        LLMRuntime->FailRequest(TEXT("LLM service rejected request."), NowSeconds);
        return false;
    }

    return true;
}


void FOffgridAILocalServiceGateway::CancelLLMRequest(const FGuid& ConversationID)
{
    if (LLMService)
    {
        LLMService->CancelActiveRequest(ConversationID);
    }

    if (LLMRuntime)
    {
        LLMRuntime->CompleteRequest(FPlatformTime::Seconds());
    }
}

void FOffgridAILocalServiceGateway::ClearLLMSession(const FGuid& ConversationID)
{
    if (LLMService)
    {
        LLMService->ClearSession(ConversationID);
    }
}

bool FOffgridAILocalServiceGateway::BeginTTSRequest(const FOffgridAILinePerformanceRequest& LineRequest)
{
    if (!TTSRuntime || !TTSService)
    {
        return false;
    }

    const FString RequestID = MakeRequestID(EOffgridAIServiceKind::TTS);
    const double NowSeconds = FPlatformTime::Seconds();
    if (!TTSRuntime->BeginRequest(RequestID, LineRequest.ConversationID, NowSeconds))
    {
        return false;
    }

    TArray<uint8> LoopbackPCM;
    int32 LoopbackSampleRate = 0;
    int32 LoopbackNumChannels = 0;

    if (ASRService)
    {
        ASRService->GetMostRecentCapture(LoopbackPCM, LoopbackSampleRate, LoopbackNumChannels);
    }

    if (!TTSService->BeginSynthesis(RequestID, LineRequest, LoopbackPCM, LoopbackSampleRate, LoopbackNumChannels))
    {
        TTSRuntime->FailRequest(TEXT("TTS service rejected request."), NowSeconds);
        return false;
    }

    return true;
}

void FOffgridAILocalServiceGateway::CancelTTS(const FGuid& ConversationID, FName NPCID, FName LineID)
{
    if (TTSService)
    {
        TTSService->Cancel(ConversationID, NPCID, LineID);
    }

    if (TTSRuntime)
    {
        TTSRuntime->CompleteRequest(FPlatformTime::Seconds());
    }
}

bool FOffgridAILocalServiceGateway::HandleTicker(float DeltaTimeSeconds)
{
    Tick(DeltaTimeSeconds);
    return true;
}

FString FOffgridAILocalServiceGateway::MakeRequestID(EOffgridAIServiceKind ServiceKind)
{
    const FString Prefix = ServiceKindToString(ServiceKind);
    const FGuid Guid = FGuid::NewGuid();
    return FString::Printf(TEXT("%s_%s"), *Prefix, *Guid.ToString(EGuidFormats::Digits));
}

FOffgridAIServiceSupervisor* FOffgridAILocalServiceGateway::FindRuntime(EOffgridAIServiceKind ServiceKind) const
{
    switch (ServiceKind)
    {
    case EOffgridAIServiceKind::ASR:
        return ASRRuntime.Get();
    case EOffgridAIServiceKind::LLM:
        return LLMRuntime.Get();
    case EOffgridAIServiceKind::TTS:
        return TTSRuntime.Get();
    default:
        return nullptr;
    }
}

void FOffgridAILocalServiceGateway::HandleServiceEvent(const FOffgridAIServiceEvent& ServiceEvent)
{
    const FString ServiceLabel = ServiceKindToString(ServiceEvent.ServiceKind);

    switch (ServiceEvent.Severity)
    {
    case EOffgridAIServiceEventSeverity::Verbose:
        UE_LOG(LogOffgridAI, Verbose, TEXT("[%s][Verbose][%s] %s"), *ServiceLabel, *ServiceEvent.Category, *ServiceEvent.Message);
        break;
    case EOffgridAIServiceEventSeverity::Warning:
        UE_LOG(LogOffgridAI, Warning, TEXT("[%s][Warning][%s] %s"), *ServiceLabel, *ServiceEvent.Category, *ServiceEvent.Message);
        break;
    case EOffgridAIServiceEventSeverity::Error:
        UE_LOG(LogOffgridAI, Error, TEXT("[%s][Error][%s] %s"), *ServiceLabel, *ServiceEvent.Category, *ServiceEvent.Message);
        break;
    default:
        UE_LOG(LogOffgridAI, Log, TEXT("[%s][Log][%s] %s"), *ServiceLabel, *ServiceEvent.Category, *ServiceEvent.Message);
        break;
    }
}

void FOffgridAILocalServiceGateway::HandleServiceStatusChanged(const FOffgridAIServiceStatus& ServiceStatus)
{
    CachedStatuses.FindOrAdd(ServiceStatus.ServiceKind) = ServiceStatus;

    if (Orchestrator.IsValid())
    {
        Orchestrator->HandleServiceStatusChanged(ServiceStatus);
    }

    if (ServiceStatus.State == EOffgridAIServiceState::Restarting || ServiceStatus.State == EOffgridAIServiceState::Fatal)
    {
        ResetServiceState(ServiceStatus.ServiceKind);
    }

    if (ServiceStatus.State == EOffgridAIServiceState::Fatal && ServiceStatus.bIsRequired && Orchestrator.IsValid())
    {
        Orchestrator->HandleServiceError(FGuid(), FString::Printf(TEXT("Required service became fatal: %s"), *ServiceKindToString(ServiceStatus.ServiceKind)));
    }
}

void FOffgridAILocalServiceGateway::ResetServiceState(EOffgridAIServiceKind ServiceKind)
{
    switch (ServiceKind)
    {
    case EOffgridAIServiceKind::ASR:
        ActiveASRInput = FOffgridAIASRActiveInput();
        if (ASRService)
        {
            ASRService->Reset();
        }
        break;

    case EOffgridAIServiceKind::LLM:
        if (LLMService)
        {
            LLMService->Reset();
        }
        break;

    case EOffgridAIServiceKind::TTS:
        if (TTSService)
        {
            TTSService->Reset();
        }
        break;

    default:
        break;
    }
}

EOffgridAIServiceImplementation FOffgridAILocalServiceGateway::ResolveASRImplementation() const
{
    if (ASRServiceSettings)
    {
        return ASRServiceSettings->BackendKind == EOffgridAIASRBackendKind::SherpaOnnx
            ? EOffgridAIServiceImplementation::Real
            : EOffgridAIServiceImplementation::Stub;
    }

    return ServiceSelection.ASR;
}

EOffgridAIServiceImplementation FOffgridAILocalServiceGateway::ResolveLLMImplementation() const
{
    if (LLMServiceSettings)
    {
        return LLMServiceSettings->BackendKind == EOffgridAILLMBackendKind::ExternalProcess
            ? EOffgridAIServiceImplementation::Real
            : EOffgridAIServiceImplementation::Stub;
    }

    return ServiceSelection.LLM;
}

EOffgridAIServiceImplementation FOffgridAILocalServiceGateway::ResolveTTSImplementation() const
{
    if (TTSServiceSettings)
    {
        return TTSServiceSettings->BackendKind == EOffgridAITTSBackendKind::Qwen3
            ? EOffgridAIServiceImplementation::Real
            : EOffgridAIServiceImplementation::Stub;
    }

    return ServiceSelection.TTS;
}

TUniquePtr<IOffgridAIASRService> FOffgridAILocalServiceGateway::CreateASRService(EOffgridAIServiceImplementation Implementation) const
{
    switch (Implementation)
    {
    case EOffgridAIServiceImplementation::Stub:
        UE_LOG(LogOffgridAI, Log, TEXT("CreateASRService: using Stub ASR service"));
        return MakeUnique<FOffgridAIASRStubService>();

    case EOffgridAIServiceImplementation::Real:
        UE_LOG(LogOffgridAI, Log, TEXT("CreateASRService: using Real ASR service"));
        return MakeUnique<FOffgridAIASRNamedPipeService>(ASRServiceSettings);

    default:
        return nullptr;
    }
}

TUniquePtr<IOffgridAILLMService> FOffgridAILocalServiceGateway::CreateLLMService(EOffgridAIServiceImplementation Implementation) const
{
    switch (Implementation)
    {
    case EOffgridAIServiceImplementation::Stub:
        UE_LOG(LogOffgridAI, Log, TEXT("CreateLLMService: using Stub LLM service"));
        return MakeUnique<FOffgridAILLMStub>(LLMServiceSettings);

    case EOffgridAIServiceImplementation::Real:
        UE_LOG(LogOffgridAI, Log, TEXT("CreateLLMService: using Real LLM service"));
        return MakeUnique<FOffgridAILLMNamedPipeService>(LLMServiceSettings);

    default:
        return nullptr;
    }
}

TUniquePtr<IOffgridAITTSService> FOffgridAILocalServiceGateway::CreateTTSService(EOffgridAIServiceImplementation Implementation) const
{
    switch (Implementation)
    {
    case EOffgridAIServiceImplementation::Stub:
        UE_LOG(LogOffgridAI, Log, TEXT("CreateTTSService: using Stub TTS service"));
        return MakeUnique<FOffgridAITTSStub>(TTSServiceSettings);

    case EOffgridAIServiceImplementation::Real:
        UE_LOG(LogOffgridAI, Log, TEXT("CreateTTSService: using Real TTS service"));
        return MakeUnique<FOffgridAITTSNamedPipeService>(TTSServiceSettings);

    default:
        return nullptr;
    }
}

void FOffgridAILocalServiceGateway::TickServiceStartup(double NowSeconds)
{
    if (ASRRuntime && ASRService && ASRRuntime->GetStatus().State == EOffgridAIServiceState::Starting)
    {
        FString StartupError;
        if (ASRService->EnsureServiceReady(StartupError))
        {
            ASRRuntime->ConfirmStartupReady(NowSeconds, TEXT("Ready handshake completed."));
        }
        else if (!StartupError.IsEmpty())
        {
            ASRRuntime->ForceFatal(StartupError, NowSeconds);
        }
    }

    if (LLMRuntime && LLMService && LLMRuntime->GetStatus().State == EOffgridAIServiceState::Starting)
    {
        FString StartupError;
        if (LLMService->EnsureServiceReady(StartupError))
        {
            LLMRuntime->ConfirmStartupReady(NowSeconds, TEXT("Ready handshake completed."));
        }
        else if (!StartupError.IsEmpty())
        {
            LLMRuntime->ForceFatal(StartupError, NowSeconds);
        }
    }

    if (TTSRuntime && TTSService && TTSRuntime->GetStatus().State == EOffgridAIServiceState::Starting)
    {
        FString StartupError;
        if (TTSService->EnsureServiceReady(StartupError))
        {
            TTSRuntime->ConfirmStartupReady(NowSeconds, TEXT("Ready handshake completed."));
        }
        else if (!StartupError.IsEmpty())
        {
            TTSRuntime->ForceFatal(StartupError, NowSeconds);
        }
    }
}

void FOffgridAILocalServiceGateway::TickASR(double NowSeconds)
{
    if (!ASRRuntime || !ASRService || !Orchestrator.IsValid())
    {
        return;
    }

    FOffgridAIASRCompletedRequest CompletedRequest;
    if (!ASRService->TryPopCompletedRequest(CompletedRequest))
    {
        return;
    }

    ASRRuntime->CompleteRequest(NowSeconds);
    Orchestrator->HandleASRFinalTranscript(CompletedRequest.ConversationID, CompletedRequest.PlayerID, CompletedRequest.Transcript);
}

void FOffgridAILocalServiceGateway::TickLLM(double NowSeconds)
{
    if (!LLMRuntime || !LLMService || !Orchestrator.IsValid())
    {
        return;
    }

    FOffgridAILLMCompletedRequest CompletedRequest;
    if (!LLMService->TryPopCompletedRequest(CompletedRequest))
    {
        return;
    }

    LLMRuntime->CompleteRequest(NowSeconds);
    Orchestrator->HandleLLMResponse(CompletedRequest.ConversationID, CompletedRequest.JSONPayload);
}

void FOffgridAILocalServiceGateway::TickTTS(double NowSeconds)
{
    if (!TTSRuntime || !TTSService || !Orchestrator.IsValid())
    {
        return;
    }

    // Normal steady-state polling remains capped so TTS cannot monopolize the game
    // thread. Before the first audio chunk in a tick, allow a larger drain window so
    // stream_started / bookkeeping events cannot strand the first playable PCM behind
    // an arbitrary one-frame delay. Once audio is flowing, drop back to the steady cap.
    constexpr int32 MaxSteadyStreamEventsPerTick = 8;
    constexpr int32 MaxInitialStreamEventsPerTick = 32;
    bool bSawAudioChunkThisTick = false;
    for (int32 EventIndex = 0; EventIndex < MaxInitialStreamEventsPerTick; ++EventIndex)
    {
        if (bSawAudioChunkThisTick && EventIndex >= MaxSteadyStreamEventsPerTick)
        {
            break;
        }

        FOffgridAITTSStreamEvent StreamEvent;
        if (!TTSService->TryPopStreamEvent(StreamEvent))
        {
            break;
        }

        switch (StreamEvent.Type)
        {
        case EOffgridAITTSStreamEventType::StreamStarted:
            Orchestrator->HandleTTSStreamStarted(
                StreamEvent.ConversationID,
                StreamEvent.NPCID,
                StreamEvent.LineID,
                StreamEvent.SampleRate,
                StreamEvent.NumChannels);
            break;

        case EOffgridAITTSStreamEventType::AudioChunk:
            bSawAudioChunkThisTick = true;
            Orchestrator->HandleTTSStreamChunk(
                StreamEvent.ConversationID,
                StreamEvent.NPCID,
                StreamEvent.LineID,
                StreamEvent.PCMChunk,
                StreamEvent.SampleRate,
                StreamEvent.NumChannels,
                StreamEvent.ChunkStartSample,
                StreamEvent.ChunkSampleCount);
            break;

        case EOffgridAITTSStreamEventType::Completed:
            TTSRuntime->CompleteRequest(NowSeconds);
            Orchestrator->HandleTTSStreamCompleted(
                StreamEvent.ConversationID,
                StreamEvent.NPCID,
                StreamEvent.LineID);
            break;

        case EOffgridAITTSStreamEventType::Error:
            TTSRuntime->FailRequest(StreamEvent.ErrorMessage.IsEmpty() ? TEXT("TTS stream error.") : StreamEvent.ErrorMessage, NowSeconds);
            Orchestrator->HandleServiceError(
                StreamEvent.ConversationID,
                StreamEvent.ErrorMessage.IsEmpty() ? TEXT("TTS stream error.") : StreamEvent.ErrorMessage);
            break;

        default:
            break;
        }
    }
}
