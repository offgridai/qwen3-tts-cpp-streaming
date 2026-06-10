#include "Core/OffgridAIOrchestrator.h"
#include "Core/OffgridAIResourceMonitorSubsystem.h"

#include "OffgridAI.h"
#include "OffgridAIBoomOperator.h"
#include "OffgridAIConversationManager.h"
#include "OffgridAILineCoach.h"
#include "Data/OffgridAIASRServiceSettingsDataAsset.h"
#include "Data/OffgridAILLMServiceSettingsDataAsset.h"
#include "Data/OffgridAITTSServiceSettingsDataAsset.h"
#include "Containers/Ticker.h"

#if OFFGRIDAI_ENABLE_TEST_STUBS
#include "Core/OffgridAILocalServiceGateway.h"
#endif

UOffgridAIOrchestrator::~UOffgridAIOrchestrator() = default;

void UOffgridAIOrchestrator::ReplayCurrentState()
{
    OnOrchestratorStateChanged.Broadcast(CurrentOrchestratorState);
    OnConversationStateChanged.Broadcast(CurrentConversationState);
}

bool UOffgridAIOrchestrator::HandleDeferredStateReplay(float DeltaTimeSeconds)
{
    DeferredStateReplayHandle.Reset();
    ReplayCurrentState();
    return false;
}

void UOffgridAIOrchestrator::QueueDeferredStateReplay()
{
    if (DeferredStateReplayHandle.IsValid())
    {
        return;
    }

    DeferredStateReplayHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UOffgridAIOrchestrator::HandleDeferredStateReplay));
}

void UOffgridAIOrchestrator::NotifyBoomOperatorFinalizeRequested(FName InputSourceID)
{
    // Optional RTT hook called by BoomOperator just before captured player audio is finalized.
    // The authoritative RTT stopwatch is still started in EndPlayerAudioInput(), but keeping
    // this hook in the main Orchestrator implementation prevents a separate link-fix file
    // from being required.
    UE_LOG(LogOffgridAI, VeryVerbose,
        TEXT("[Latency][RoundTrip] BoomOperator finalize requested input_source=%s"),
        *InputSourceID.ToString());
}


bool UOffgridAIOrchestrator::HandleDeferredConversationPrime(float DeltaTimeSeconds)
{
    if (!ActiveConversationManager)
    {
        DeferredConversationPrimeHandle.Reset();
        return false;
    }

    if (!Services || !AreRequiredServicesReady())
    {
        UE_LOG(LogOffgridAI, Log,
            TEXT("[Orchestrator] Deferring conversation prime; required services are not ready yet."));
        return true;
    }

    if (!HasRequiredRegistrants())
    {
        UE_LOG(LogOffgridAI, Log,
            TEXT("[Orchestrator] Deferring conversation prime; required registrants are not ready yet."));
        return true;
    }

    UE_LOG(LogOffgridAI, Log,
        TEXT("[Orchestrator] Conversation creation broadcast settled; priming conversation now."));

    DeferredConversationPrimeHandle.Reset();
    ActiveConversationManager->MarkPrimedAndAwaitingInput();
    QueueDeferredStateReplay();
    return false;
}

void UOffgridAIOrchestrator::QueueDeferredConversationPrime()
{
    if (DeferredConversationPrimeHandle.IsValid())
    {
        return;
    }

    DeferredConversationPrimeHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UOffgridAIOrchestrator::HandleDeferredConversationPrime),
        0.0f);
}

void UOffgridAIOrchestrator::ClearDeferredConversationPrime()
{
    if (!DeferredConversationPrimeHandle.IsValid())
    {
        return;
    }

    FTSTicker::GetCoreTicker().RemoveTicker(DeferredConversationPrimeHandle);
    DeferredConversationPrimeHandle.Reset();
}

void UOffgridAIOrchestrator::SubmitHiddenPlayerTurnThroughNormalPath(const FGuid& ConversationID, FName PlayerID, const FText& HiddenPlayerText)
{
    if (!IsActiveConversationID(ConversationID) || !ActiveConversationManager)
    {
        UE_LOG(LogOffgridAI, Warning,
            TEXT("[Orchestrator] Hidden player turn ignored; no matching active conversation."));
        return;
    }

    if (!ActiveConversationManager->GetPlayerIDs().Contains(PlayerID))
    {
        UE_LOG(LogOffgridAI, Warning,
            TEXT("[Orchestrator] Hidden player turn player %s was not registered in the conversation; using first registered player."),
            *PlayerID.ToString());
        PlayerID = ActiveConversationManager->GetPlayerIDs().Num() > 0
            ? ActiveConversationManager->GetPlayerIDs()[0]
            : FName(TEXT("Player"));
    }

    ClearDeferredHiddenPlayerTurn();

    DeferredHiddenPlayerConversationID = ConversationID;
    DeferredHiddenPlayerID = PlayerID;
    DeferredHiddenPlayerText = HiddenPlayerText;
    bHasDeferredHiddenPlayerTurn = true;

    UE_LOG(LogOffgridAI, Log,
        TEXT("[Orchestrator] Hidden player turn entering normal timing path conversation=%s player=%s chars=%d"),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        *PlayerID.ToString(),
        HiddenPlayerText.ToString().Len());

    BeginTurnLatencyTraceIfNeeded(ConversationID, PlayerID);
    NoteTurnLatencyEvent(TEXT("PTTPressed"), FString::Printf(TEXT("player=%s hidden=true"), *PlayerID.ToString()));
    NoteTurnLatencyEvent(TEXT("ASRInputAccepted"), FString::Printf(TEXT("player=%s hidden=true"), *PlayerID.ToString()));
    NoteTurnLatencyEvent(TEXT("FirstMicChunkCaptured"), TEXT("bytes=0 sample_rate=0 channels=0 hidden=true"));

    ActiveInputPlayerID = PlayerID;
    ActiveConversationManager->BeginRecording();

    DeferredHiddenPlayerTurnFinalizeHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UOffgridAIOrchestrator::HandleDeferredHiddenPlayerTurnFinalize),
        0.0f);
}

bool UOffgridAIOrchestrator::HandleDeferredHiddenPlayerTurnFinalize(float DeltaTimeSeconds)
{
    DeferredHiddenPlayerTurnFinalizeHandle.Reset();

    if (!bHasDeferredHiddenPlayerTurn || !ActiveConversationManager || !IsActiveConversationID(DeferredHiddenPlayerConversationID))
    {
        ClearDeferredHiddenPlayerTurn();
        return false;
    }

    UE_LOG(LogOffgridAI, Log,
        TEXT("[Orchestrator] Hidden player turn synthetic finalize conversation=%s player=%s"),
        *DeferredHiddenPlayerConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        *DeferredHiddenPlayerID.ToString());

    NoteTurnLatencyEvent(TEXT("PTTReleased"), FString::Printf(TEXT("player=%s hidden=true"), *DeferredHiddenPlayerID.ToString()));
    NoteTurnLatencyEvent(TEXT("ASRFinalizeSent"), FString::Printf(TEXT("player=%s hidden=true"), *DeferredHiddenPlayerID.ToString()));
    StartRoundTripMetric(DeferredHiddenPlayerConversationID, DeferredHiddenPlayerID);
    NoteTurnLatencyEvent(TEXT("ASRFinalizeReturned"), FString::Printf(TEXT("player=%s hidden=true"), *DeferredHiddenPlayerID.ToString()));

    ActiveConversationManager->BeginProcessingASR();
    ActiveInputPlayerID = NAME_None;

    DeferredHiddenPlayerTurnTranscriptHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UOffgridAIOrchestrator::HandleDeferredHiddenPlayerTurnTranscript),
        0.0f);

    return false;
}

bool UOffgridAIOrchestrator::HandleDeferredHiddenPlayerTurnTranscript(float DeltaTimeSeconds)
{
    DeferredHiddenPlayerTurnTranscriptHandle.Reset();

    if (!bHasDeferredHiddenPlayerTurn || !ActiveConversationManager || !IsActiveConversationID(DeferredHiddenPlayerConversationID))
    {
        ClearDeferredHiddenPlayerTurn();
        return false;
    }

    UE_LOG(LogOffgridAI, Log,
        TEXT("[Orchestrator] Hidden player turn synthetic ASR transcript ready conversation=%s player=%s"),
        *DeferredHiddenPlayerConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        *DeferredHiddenPlayerID.ToString());

    // This is intentionally the same post-ASR timing as a real player turn:
    // Recording -> ProcessingASR -> text committed -> ProcessingLLM.
    // The hidden transcript remains hidden from UI, but the state/timing path is identical.
    ActiveConversationManager->SubmitHiddenPlayerTurnTextAfterSyntheticASR(DeferredHiddenPlayerText);

    ClearDeferredHiddenPlayerTurn();
    return false;
}

void UOffgridAIOrchestrator::ClearDeferredHiddenPlayerTurn()
{
    if (DeferredHiddenPlayerTurnFinalizeHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(DeferredHiddenPlayerTurnFinalizeHandle);
        DeferredHiddenPlayerTurnFinalizeHandle.Reset();
    }

    if (DeferredHiddenPlayerTurnTranscriptHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(DeferredHiddenPlayerTurnTranscriptHandle);
        DeferredHiddenPlayerTurnTranscriptHandle.Reset();
    }

    bHasDeferredHiddenPlayerTurn = false;
    DeferredHiddenPlayerConversationID = FGuid();
    DeferredHiddenPlayerID = NAME_None;
    DeferredHiddenPlayerText = FText::GetEmpty();
}



bool UOffgridAIOrchestrator::IsTTSReadyToAcceptRequest() const
{
    if (!Services)
    {
        return false;
    }

    const FOffgridAIServiceStatus TTSStatus = Services->GetServiceStatus(EOffgridAIServiceKind::TTS);
    return TTSStatus.State == EOffgridAIServiceState::Ready;
}

bool UOffgridAIOrchestrator::HandleDeferredNPCLineDispatch(float DeltaTimeSeconds)
{
    if (!bHasDeferredNPCLineDispatch)
    {
        DeferredNPCLineDispatchHandle.Reset();
        return false;
    }

    if (!ActiveConversationManager || !IsActiveConversationID(DeferredNPCLineConversationID))
    {
        ClearDeferredNPCLineDispatch();
        return false;
    }

    if (!IsTTSReadyToAcceptRequest())
    {
        UE_LOG(LogOffgridAI, Log,
            TEXT("[Orchestrator] Deferring NPC line dispatch; TTS is not ready yet. line=%s"),
            *DeferredNPCLineRequest.LineID.ToString());
        return true;
    }

    ClearDeferredNPCLineDispatch();
    PumpNPCLinePipeline();
    return false;
}

void UOffgridAIOrchestrator::QueueDeferredNPCLineDispatch(const FGuid& ConversationID, const FOffgridAILinePerformanceRequest& LineRequest)
{
    DeferredNPCLineConversationID = ConversationID;
    DeferredNPCLineRequest = LineRequest;
    bHasDeferredNPCLineDispatch = true;

    if (DeferredNPCLineDispatchHandle.IsValid())
    {
        return;
    }

    DeferredNPCLineDispatchHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UOffgridAIOrchestrator::HandleDeferredNPCLineDispatch),
        0.0f);
}

void UOffgridAIOrchestrator::ClearDeferredNPCLineDispatch()
{
    bHasDeferredNPCLineDispatch = false;
    DeferredNPCLineConversationID = FGuid();
    DeferredNPCLineRequest = FOffgridAILinePerformanceRequest();

    if (DeferredNPCLineDispatchHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(DeferredNPCLineDispatchHandle);
        DeferredNPCLineDispatchHandle.Reset();
    }
}

void UOffgridAIOrchestrator::ReinstallLocalServices()
{
#if OFFGRIDAI_ENABLE_TEST_STUBS
    InstallServices(MakeUnique<FOffgridAILocalServiceGateway>(
        FOffgridAIServiceSelection(),
        FOffgridAIServiceSupervisorSettings(),
        ASRServiceSettings,
        LLMServiceSettings,
        TTSServiceSettings));
#else
    RefreshIdleRuntimeState();
#endif
}

void UOffgridAIOrchestrator::EnsureLocalServicesInstalledIfNeeded()
{
#if OFFGRIDAI_ENABLE_TEST_STUBS
    if (!Services && HasRequiredRegistrants())
    {
        ReinstallLocalServices();
    }
#endif
}

void UOffgridAIOrchestrator::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    SetConversationState(EOffgridAIConversationState::Inactive);
    SetOrchestratorState(EOffgridAIOrchestratorState::Inactive);

    EmotionTransitionTickHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UOffgridAIOrchestrator::HandleEmotionTransitionTick));

#if OFFGRIDAI_ENABLE_TEST_STUBS
    EnsureLocalServicesInstalledIfNeeded();
#endif
}

void UOffgridAIOrchestrator::Deinitialize()
{
    ClearDeferredConversationPrime();
    ClearDeferredHiddenPlayerTurn();
    ClearDeferredNPCLineDispatch();

    if (DeferredStateReplayHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(DeferredStateReplayHandle);
        DeferredStateReplayHandle.Reset();
    }

    if (EmotionTransitionTickHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(EmotionTransitionTickHandle);
        EmotionTransitionTickHandle.Reset();
    }

    TearDownActiveConversation(false);

    if (Services)
    {
        Services->Shutdown();
        Services.Reset();
    }

    RegisteredBoomOperatorsByID.Empty();
    RegisteredLineCoachesByID.Empty();

    Super::Deinitialize();
}

void UOffgridAIOrchestrator::InstallServices(TUniquePtr<IOffgridAIServiceGateway> InServices)
{
    if (Services)
    {
        Services->Shutdown();
    }

    Services = MoveTemp(InServices);

    if (Services)
    {
        Services->Initialize(this);
        Services->StartupServices();
    }

    RefreshIdleRuntimeState();
    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::HandleServiceStatusChanged(const FOffgridAIServiceStatus& ServiceStatus)
{
    if (ActiveConversationManager)
    {
        // During an active conversation, transient service heartbeat/restart states should not
        // tear the conversation down. If an NPC line is waiting for TTS, the deferred ticker
        // will retry when TTS returns to Ready.
        if (ServiceStatus.ServiceKind == EOffgridAIServiceKind::TTS && ServiceStatus.State == EOffgridAIServiceState::Ready && bHasDeferredNPCLineDispatch)
        {
            QueueDeferredNPCLineDispatch(DeferredNPCLineConversationID, DeferredNPCLineRequest);
        }
        return;
    }

    if (ServiceStatus.State == EOffgridAIServiceState::Fatal)
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        QueueDeferredStateReplay();
        return;
    }

    RefreshIdleRuntimeState();
    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::SetASRServiceSettings(UOffgridAIASRServiceSettingsDataAsset* InASRServiceSettings)
{
    if (ASRServiceSettings == InASRServiceSettings)
    {
        return;
    }

    ASRServiceSettings = InASRServiceSettings;

    if (Services || HasRequiredRegistrants())
    {
        ReinstallLocalServices();
    }
    else
    {
        RefreshIdleRuntimeState();
    }

    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::SetLLMServiceSettings(UOffgridAILLMServiceSettingsDataAsset* InLLMServiceSettings)
{
    if (LLMServiceSettings == InLLMServiceSettings)
    {
        return;
    }

    LLMServiceSettings = InLLMServiceSettings;

    if (Services || HasRequiredRegistrants())
    {
        ReinstallLocalServices();
    }
    else
    {
        RefreshIdleRuntimeState();
    }

    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::SetTTSServiceSettings(UOffgridAITTSServiceSettingsDataAsset* InTTSServiceSettings)
{
    if (TTSServiceSettings == InTTSServiceSettings)
    {
        return;
    }

    TTSServiceSettings = InTTSServiceSettings;

    if (Services || HasRequiredRegistrants())
    {
        ReinstallLocalServices();
    }
    else
    {
        RefreshIdleRuntimeState();
    }

    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::RegisterBoomOperator(UOffgridAIBoomOperator* BoomOperator)
{
    if (!BoomOperator || BoomOperator->PlayerID == NAME_None)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("RegisterBoomOperator failed; invalid operator or PlayerID"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    RegisteredBoomOperatorsByID.FindOrAdd(BoomOperator->PlayerID) = BoomOperator;

    if (!ActiveConversationManager)
    {
        EnsureLocalServicesInstalledIfNeeded();
        RefreshIdleRuntimeState();
    }

    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::UnregisterBoomOperator(FName PlayerID, UOffgridAIBoomOperator* BoomOperator)
{
    TObjectPtr<UOffgridAIBoomOperator>* Found = RegisteredBoomOperatorsByID.Find(PlayerID);
    if (!Found)
    {
        return;
    }

    if (Found->Get() == BoomOperator || !Found->Get())
    {
        RegisteredBoomOperatorsByID.Remove(PlayerID);
    }

    if (ActiveInputPlayerID == PlayerID)
    {
        ActiveInputPlayerID = NAME_None;
    }

    if (!ActiveConversationManager)
    {
        RefreshIdleRuntimeState();
    }

    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::RegisterLineCoach(UOffgridAILineCoach* LineCoach)
{
    if (!LineCoach || LineCoach->NPCID == NAME_None)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("RegisterLineCoach failed; invalid coach or NPCID"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    if (TObjectPtr<UOffgridAILineCoach>* Existing = RegisteredLineCoachesByID.Find(LineCoach->NPCID))
    {
        if (Existing->Get() && Existing->Get() != LineCoach)
        {
            UE_LOG(LogOffgridAI, Error, TEXT("RegisterLineCoach failed; duplicate NPCID %s. Each LineCoach in a multi-NPC scene must have a unique NPCID."), *LineCoach->NPCID.ToString());
            SetOrchestratorState(EOffgridAIOrchestratorState::Error);
            return;
        }
    }

    if (LineCoach->VoiceID == NAME_None && !LineCoach->ShouldUseVoiceDesignForTTS())
    {
        UE_LOG(LogOffgridAI, Error, TEXT("RegisterLineCoach failed; NPC %s has no VoiceID and VoiceDesign is disabled"), *LineCoach->NPCID.ToString());
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    RegisteredLineCoachesByID.FindOrAdd(LineCoach->NPCID) = LineCoach;

    UE_LOG(LogOffgridAI, Log, TEXT("[Orchestrator] Registered LineCoach npc=%s voice=%s use_voice_design=%s actor=%s"),
        *LineCoach->NPCID.ToString(),
        *LineCoach->VoiceID.ToString(),
        LineCoach->ShouldUseVoiceDesignForTTS() ? TEXT("true") : TEXT("false"),
        LineCoach->GetOwner() ? *LineCoach->GetOwner()->GetName() : TEXT("<none>"));

    if (!ActiveConversationManager)
    {
        EnsureLocalServicesInstalledIfNeeded();
        RefreshIdleRuntimeState();
    }

    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::UnregisterLineCoach(FName NPCID, UOffgridAILineCoach* LineCoach)
{
    TObjectPtr<UOffgridAILineCoach>* Found = RegisteredLineCoachesByID.Find(NPCID);
    if (!Found)
    {
        return;
    }

    if (Found->Get() == LineCoach || !Found->Get())
    {
        RegisteredLineCoachesByID.Remove(NPCID);
    }

    if (!ActiveConversationManager)
    {
        RefreshIdleRuntimeState();
    }

    QueueDeferredStateReplay();
}

bool UOffgridAIOrchestrator::CreateConversationFromRegisteredParticipants(
    UOffgridAIConversationPromptDataAsset* ConversationPromptAsset)
{
    return CreateConversation(GetRegisteredPlayerIDs(), GetRegisteredNPCIDs(), ConversationPromptAsset);
}

bool UOffgridAIOrchestrator::CreateConversation(
    const TArray<FName>& PlayerIDs,
    const TArray<FName>& NPCIDs,
    UOffgridAIConversationPromptDataAsset* ConversationPromptAsset)
{
    if (ActiveConversationManager)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("CreateConversation failed; only one active conversation is currently supported"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return false;
    }

    if (!HasRequiredRegistrants() || !ConversationPromptAsset || PlayerIDs.IsEmpty() || NPCIDs.IsEmpty())
    {
        UE_LOG(LogOffgridAI, Error, TEXT("CreateConversation failed; missing participants, prompt asset, or registrations"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return false;
    }

    if (!AreRequiredServicesReady())
    {
        UE_LOG(LogOffgridAI, Error, TEXT("CreateConversation failed; required services are not ready"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return false;
    }

    if (!ValidateConversationParticipants(PlayerIDs, NPCIDs))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("CreateConversation failed; requested participants were not registered"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return false;
    }

    ActiveConversationManager = NewObject<UOffgridAIConversationManager>(this);
    ActiveConversationManager->InitializeConversation(this, PlayerIDs, NPCIDs, ConversationPromptAsset);

    if (!Services || !Services->InitializeLLMSession(
        ActiveConversationManager->GetConversationID(),
        ActiveConversationManager->GetNPCIDs(),
        ActiveConversationManager->GetConversationPromptAsset(),
        ActiveConversationManager->GetCanonicalConversationRecord()))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("CreateConversation failed; could not initialize LLM session"));
        TearDownActiveConversation(true);
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return false;
    }

    // Broadcast creation before allowing hidden/NPC-first auto-start to run.
    // Some listeners (LineCoach/UI/Blueprint presentation setup) bind or settle from
    // OnConversationCreated. If MarkPrimedAndAwaitingInput() fires first, a hidden
    // hot-start greeting can begin on a partially-settled runtime path while later
    // player-driven turns use the fully stable path.
    OnConversationCreated.Broadcast();

    // Prime on the next ticker turn, after creation listeners have had a chance to
    // observe the new conversation. Re-check readiness at that point so hidden
    // hot-start cannot jump ahead of service/registrant readiness.
    QueueDeferredConversationPrime();

    return true;
}

void UOffgridAIOrchestrator::ResetRuntime()
{
    TearDownActiveConversation(true);
    RefreshIdleRuntimeState();
    QueueDeferredStateReplay();
}

void UOffgridAIOrchestrator::TearDownActiveConversation(bool bPreserveRegistrations)
{
    ClearDeferredConversationPrime();
    ClearDeferredHiddenPlayerTurn();
    ClearDeferredNPCLineDispatch();
    ClearNPCLineSequence();

    const bool bHadActiveConversation = ActiveConversationManager != nullptr;

    if (bHadActiveConversation && ActiveTurnLatencyTrace.IsActive())
    {
        FinalizeTurnLatencyTrace(TEXT("conversation_teardown"));
        ClearRoundTripMetric();
    }

    ActiveInputPlayerID = NAME_None;
    bAwaitingPlayerImpulseClassifierResponse = false;
    PendingPlayerImpulseConversationID = FGuid();
    PendingPlayerImpulseText = FText::GetEmpty();

    if (Services && ActiveConversationManager)
    {
        const FGuid ConversationID = ActiveConversationManager->GetConversationID();
        Services->CancelLLMRequest(ConversationID);
        Services->ClearLLMSession(ConversationID);

        const TArray<FOffgridAILinePerformanceRequest>& PendingLineRequests = ActiveConversationManager->GetPendingNPCLineRequests();
        for (const FOffgridAILinePerformanceRequest& PendingLineRequest : PendingLineRequests)
        {
            Services->CancelTTS(PendingLineRequest.ConversationID, PendingLineRequest.NPCID, PendingLineRequest.LineID);
        }
    }

    ActiveConversationManager = nullptr;
    SetConversationState(EOffgridAIConversationState::Inactive);

    if (bHadActiveConversation)
    {
        OnConversationEnded.Broadcast();
    }

    if (!bPreserveRegistrations)
    {
        RegisteredBoomOperatorsByID.Empty();
        RegisteredLineCoachesByID.Empty();
    }
}


void UOffgridAIOrchestrator::BeginTurnLatencyTraceIfNeeded(const FGuid& ConversationID, FName PlayerID)
{
    if (ActiveTurnLatencyTrace.IsActive())
    {
        ActiveTurnLatencyTrace.EmitAndReset(TEXT("superseded"));
        ClearRoundTripMetric();
    }

    ActiveTurnLatencyTrace.BeginTurn(++TurnLatencyTraceCounter, ConversationID, PlayerID);
}

void UOffgridAIOrchestrator::FinalizeTurnLatencyTrace(const FString& Outcome)
{
    TArray<FOffgridAIMetricSample> MetricSamples;
    ActiveTurnLatencyTrace.GetCanonicalMetricSamples(MetricSamples);
    for (const FOffgridAIMetricSample& Sample : MetricSamples)
    {
        PerformanceMetricAccumulator.AddSample(Sample.Name, Sample.ValueMs);
    }

    if (!MetricSamples.IsEmpty())
    {
        PublishPerformanceMetricWindows();
        UE_LOG(LogOffgridAI, Log, TEXT("[Performance][Rolling] %s"), *PerformanceMetricAccumulator.BuildLogSummary());
    }

    ActiveTurnLatencyTrace.EmitAndReset(Outcome);
}

void UOffgridAIOrchestrator::PublishPerformanceMetricWindows()
{
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        if (UOffgridAIResourceMonitorSubsystem* ResourceMonitor = GameInstance->GetSubsystem<UOffgridAIResourceMonitorSubsystem>())
        {
            ResourceMonitor->SetLatestPerformanceMetricStats(PerformanceMetricAccumulator);
        }
    }
}

void UOffgridAIOrchestrator::StartRoundTripMetric(const FGuid& ConversationID, FName PlayerID)
{
    bRoundTripMetricActive = true;
    RoundTripMetricConversationID = ConversationID;
    RoundTripMetricPlayerID = PlayerID;
    RoundTripMetricStartSeconds = FPlatformTime::Seconds();
}

void UOffgridAIOrchestrator::CompleteRoundTripMetricIfActive(const FGuid& ConversationID, FName LineID)
{
    if (!bRoundTripMetricActive || RoundTripMetricConversationID != ConversationID || RoundTripMetricStartSeconds <= 0.0)
    {
        return;
    }

    const double RoundTripTimeMs = FMath::Max((FPlatformTime::Seconds() - RoundTripMetricStartSeconds) * 1000.0, 0.0);

    if (UGameInstance* GameInstance = GetGameInstance())
    {
        if (UOffgridAIResourceMonitorSubsystem* ResourceMonitor = GameInstance->GetSubsystem<UOffgridAIResourceMonitorSubsystem>())
        {
            ResourceMonitor->SetLatestRoundTripTimeMs(static_cast<float>(RoundTripTimeMs));
        }
    }

    UE_LOG(LogOffgridAI, Log,
        TEXT("[Latency][RoundTrip] finalize_to_first_audio_out=%.1fms conversation=%s player=%s line=%s"),
        RoundTripTimeMs,
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        *RoundTripMetricPlayerID.ToString(),
        *LineID.ToString());

    ClearRoundTripMetric();
}

void UOffgridAIOrchestrator::ClearRoundTripMetric()
{
    bRoundTripMetricActive = false;
    RoundTripMetricConversationID = FGuid();
    RoundTripMetricPlayerID = NAME_None;
    RoundTripMetricStartSeconds = 0.0;
}

void UOffgridAIOrchestrator::RefreshIdleRuntimeState()
{
    if (ActiveConversationManager)
    {
        return;
    }

    SetConversationState(EOffgridAIConversationState::Inactive);

    EnsureLocalServicesInstalledIfNeeded();

    if (!Services)
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Inactive);
        return;
    }

    if (!AreRequiredServicesReady())
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Booting);
        return;
    }

    if (!HasRequiredRegistrants())
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Inactive);
        return;
    }

    SetOrchestratorState(EOffgridAIOrchestratorState::Ready);
}

bool UOffgridAIOrchestrator::BeginPlayerAudioInput(FName PlayerID)
{
    if (!ActiveConversationManager || !Services)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("BeginPlayerAudioInput ignored; no active conversation or service adapter"));
        RefreshIdleRuntimeState();
        return false;
    }

    if (ActiveInputPlayerID != NAME_None || !FindBoomOperatorByID(PlayerID))
    {
        return false;
    }

    if (!ActiveConversationManager->GetPlayerIDs().Contains(PlayerID))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("BeginPlayerAudioInput failed; player %s is not in the active conversation"), *PlayerID.ToString());
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return false;
    }

    if (ActiveConversationManager->GetConversationState() != EOffgridAIConversationState::AwaitingInput)
    {
        return false;
    }

    const FGuid ConversationID = ActiveConversationManager->GetConversationID();
    if (!Services->BeginPlayerAudioInput(ConversationID, PlayerID))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("BeginPlayerAudioInput failed in service adapter"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return false;
    }

    BeginTurnLatencyTraceIfNeeded(ConversationID, PlayerID);
    NoteTurnLatencyEvent(TEXT("PTTPressed"), FString::Printf(TEXT("player=%s"), *PlayerID.ToString()));
    NoteTurnLatencyEvent(TEXT("ASRInputAccepted"), FString::Printf(TEXT("player=%s"), *PlayerID.ToString()));

    ActiveInputPlayerID = PlayerID;
    ActiveConversationManager->BeginRecording();
    return true;
}

void UOffgridAIOrchestrator::SubmitPlayerAudioChunk(FName PlayerID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels)
{
    if (!ActiveConversationManager || !Services || ActiveInputPlayerID != PlayerID || PCMChunk.IsEmpty())
    {
        return;
    }

    if (!ActiveTurnLatencyTrace.HasFirstMark(TEXT("FirstMicChunkCaptured")))
    {
        NoteTurnLatencyEvent(
            TEXT("FirstMicChunkCaptured"),
            FString::Printf(TEXT("bytes=%d sample_rate=%d channels=%d"), PCMChunk.Num(), SampleRate, NumChannels));
    }

    Services->SubmitPlayerAudioChunk(
        ActiveConversationManager->GetConversationID(),
        PlayerID,
        PCMChunk,
        SampleRate,
        NumChannels);
}

void UOffgridAIOrchestrator::EndPlayerAudioInput(FName PlayerID)
{
    const bool bHasConversationManager = (ActiveConversationManager != nullptr);
    const bool bHasServices = (Services != nullptr);

    FGuid ConversationID;
    if (bHasConversationManager)
    {
        ConversationID = ActiveConversationManager->GetConversationID();
    }

    UE_LOG(LogOffgridAI, Warning,
        TEXT("Orchestrator::EndPlayerAudioInput entered. HasConversationManager=%s HasServices=%s ActiveInputPlayerID=%s PlayerID=%s ConversationID=%s"),
        bHasConversationManager ? TEXT("true") : TEXT("false"),
        bHasServices ? TEXT("true") : TEXT("false"),
        *ActiveInputPlayerID.ToString(),
        *PlayerID.ToString(),
        bHasConversationManager ? *ConversationID.ToString() : TEXT("<none>"));

    if (!bHasConversationManager || !bHasServices || ActiveInputPlayerID != PlayerID)
    {
        UE_LOG(LogOffgridAI, Warning,
            TEXT("Orchestrator::EndPlayerAudioInput early out."));
        return;
    }

    UE_LOG(LogOffgridAI, Warning,
        TEXT("Orchestrator::EndPlayerAudioInput calling Services->EndPlayerAudioInput. ConversationID=%s PlayerID=%s"),
        *ConversationID.ToString(),
        *PlayerID.ToString());

    NoteTurnLatencyEvent(TEXT("ASRFinalizeSent"), FString::Printf(TEXT("player=%s"), *PlayerID.ToString()));
    StartRoundTripMetric(ConversationID, PlayerID);
    Services->EndPlayerAudioInput(ConversationID, PlayerID);
    NoteTurnLatencyEvent(TEXT("ASRFinalizeReturned"), FString::Printf(TEXT("player=%s"), *PlayerID.ToString()));

    UE_LOG(LogOffgridAI, Warning,
        TEXT("Orchestrator::EndPlayerAudioInput returned from Services->EndPlayerAudioInput"));

    // move state AFTER handing off to ASR
    ActiveConversationManager->BeginProcessingASR();

    // clear AFTER everything else
    ActiveInputPlayerID = NAME_None;
}

void UOffgridAIOrchestrator::SubmitConversationTurnToLLM(const FGuid& ConversationID, const FText& PlayerText)
{
    // First classify the emotional impulse caused by the latest player line.
    // ConversationManager applies that impulse to runtime PADS before dialogue is written,
    // so the line-writer, chat/TTS labels, and face expression all share one current state.
    SubmitPlayerImpulseClassifierToLLM(ConversationID, PlayerText);
}

void UOffgridAIOrchestrator::SubmitPlayerImpulseClassifierToLLM(const FGuid& ConversationID, const FText& PlayerText)
{
    if (!IsActiveConversationID(ConversationID) || !ActiveConversationManager || !Services)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("SubmitPlayerImpulseClassifierToLLM failed; invalid conversation or service adapter"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    bAwaitingPlayerImpulseClassifierResponse = true;
    PendingPlayerImpulseConversationID = ConversationID;
    PendingPlayerImpulseText = PlayerText;

    NoteTurnLatencyEvent(TEXT("PlayerImpulseClassifierRequestSent"), FString::Printf(TEXT("chars=%d"), PlayerText.ToString().Len()));

    const bool bSubmitted = Services->SubmitLLMRequest(
        ConversationID,
        ActiveConversationManager->GetNPCIDs(),
        PlayerText,
        EOffgridAILLMRequestKind::EmotionImpactClassifier,
        ActiveConversationManager->GetConversationPromptAsset(),
        ActiveConversationManager->GetCanonicalConversationRecord(),
        ActiveConversationManager->GetSignificantPersistentEmotionStatePrompt(),
        GetSupportedEmotionNamesForNPCs(ActiveConversationManager->GetNPCIDs()));

    NoteTurnLatencyEvent(TEXT("PlayerImpulseClassifierRequestReturned"));

    if (!bSubmitted)
    {
        bAwaitingPlayerImpulseClassifierResponse = false;
        PendingPlayerImpulseConversationID = FGuid();
        PendingPlayerImpulseText = FText::GetEmpty();
        UE_LOG(LogOffgridAI, Error, TEXT("SubmitPlayerImpulseClassifierToLLM failed in service adapter"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
    }
}

void UOffgridAIOrchestrator::SubmitDialogueTurnToLLM(const FGuid& ConversationID, const FText& PlayerText)
{
    if (!IsActiveConversationID(ConversationID) || !ActiveConversationManager || !Services)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("SubmitDialogueTurnToLLM failed; invalid conversation or service adapter"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    NoteTurnLatencyEvent(TEXT("LLMRequestSent"), FString::Printf(TEXT("chars=%d"), PlayerText.ToString().Len()));

    const bool bSubmitted = Services->SubmitLLMRequest(
        ConversationID,
        ActiveConversationManager->GetNPCIDs(),
        PlayerText,
        EOffgridAILLMRequestKind::Dialogue,
        ActiveConversationManager->GetConversationPromptAsset(),
        ActiveConversationManager->GetCanonicalConversationRecord(),
        ActiveConversationManager->GetSignificantPersistentEmotionStatePrompt(),
        GetSupportedEmotionNamesForNPCs(ActiveConversationManager->GetNPCIDs()));

    NoteTurnLatencyEvent(TEXT("LLMRequestReturned"));

    if (!bSubmitted)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("SubmitDialogueTurnToLLM failed in service adapter"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
    }
}


float UOffgridAIOrchestrator::GetEmotionStateStep() const
{
    return 0.1f;
}

bool UOffgridAIOrchestrator::ResetLLMSessionForActiveConversation(const FGuid& ConversationID)
{
    if (!IsActiveConversationID(ConversationID) || !ActiveConversationManager || !Services)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("ResetLLMSessionForActiveConversation ignored; invalid conversation or service adapter"));
        return false;
    }

    UE_LOG(LogOffgridAI, Warning, TEXT("[Orchestrator] Resetting and re-priming LLM session conversation=%s"),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens));

    Services->CancelLLMRequest(ConversationID);
    Services->ClearLLMSession(ConversationID);

    const bool bInitialized = Services->InitializeLLMSession(
        ConversationID,
        ActiveConversationManager->GetNPCIDs(),
        ActiveConversationManager->GetConversationPromptAsset(),
        ActiveConversationManager->GetCanonicalConversationRecord());

    if (!bInitialized)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("ResetLLMSessionForActiveConversation failed to reinitialize LLM session"));
        return false;
    }

    NoteTurnLatencyEvent(TEXT("LLMSessionResetForRetry"));
    return true;
}

void UOffgridAIOrchestrator::ClearNPCLineSequence()
{
    NPCLinePipeline.Empty();
    NPCLinePipelineConversationID = FGuid();
    ActiveNPCPlaybackIndex = INDEX_NONE;
    ActiveTTSGenerationIndex = INDEX_NONE;
}

int32 UOffgridAIOrchestrator::FindBufferedLineIndexByLineID(FName LineID) const
{
    for (int32 Index = 0; Index < NPCLinePipeline.Num(); ++Index)
    {
        if (NPCLinePipeline[Index].Request.LineID == LineID)
        {
            return Index;
        }
    }

    return INDEX_NONE;
}

void UOffgridAIOrchestrator::BeginNPCLineSequence(const FGuid& ConversationID, const TArray<FOffgridAILinePerformanceRequest>& LineRequests)
{
    if (!IsActiveConversationID(ConversationID) || !Services || LineRequests.Num() == 0)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("BeginNPCLineSequence failed; invalid conversation, service adapter, or empty sequence"));
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    ClearDeferredNPCLineDispatch();
    ClearNPCLineSequence();

    NPCLinePipelineConversationID = ConversationID;
    NPCLinePipeline.Reserve(LineRequests.Num());
    for (const FOffgridAILinePerformanceRequest& LineRequest : LineRequests)
    {
        if (LineRequest.ConversationID != ConversationID)
        {
            UE_LOG(LogOffgridAI, Error, TEXT("BeginNPCLineSequence rejected line with mismatched conversation. line=%s"), *LineRequest.LineID.ToString());
            ClearNPCLineSequence();
            SetOrchestratorState(EOffgridAIOrchestratorState::Error);
            return;
        }

        if (!FindLineCoachByID(LineRequest.NPCID))
        {
            UE_LOG(LogOffgridAI, Error, TEXT("BeginNPCLineSequence failed; LineCoach for NPC %s was not found"), *LineRequest.NPCID.ToString());
            ClearNPCLineSequence();
            SetOrchestratorState(EOffgridAIOrchestratorState::Error);
            return;
        }

        FBufferedTTSLine BufferedLine;
        BufferedLine.Request = LineRequest;
        NPCLinePipeline.Add(MoveTemp(BufferedLine));
    }

    ActiveNPCPlaybackIndex = 0;

    UE_LOG(LogOffgridAI, Log, TEXT("[Orchestrator] Starting NPC line pipeline conversation=%s line_count=%d"),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        NPCLinePipeline.Num());

    PumpNPCLinePipeline();
}

void UOffgridAIOrchestrator::DispatchNPCLine(const FGuid& ConversationID, const FOffgridAILinePerformanceRequest& LineRequest)
{
    TArray<FOffgridAILinePerformanceRequest> SingleLine;
    SingleLine.Add(LineRequest);
    BeginNPCLineSequence(ConversationID, SingleLine);
}

bool UOffgridAIOrchestrator::RequestTTSSynthesisForLineIndex(int32 LineIndex)
{
    if (!NPCLinePipeline.IsValidIndex(LineIndex) || !Services)
    {
        return false;
    }

    FBufferedTTSLine& BufferedLine = NPCLinePipeline[LineIndex];
    if (BufferedLine.bRequestAccepted || BufferedLine.bStreamCompleted)
    {
        return true;
    }

    if (!IsTTSReadyToAcceptRequest())
    {
        UE_LOG(LogOffgridAI, Verbose, TEXT("[Orchestrator] TTS pipeline waiting for service readiness. line=%s index=%d"),
            *BufferedLine.Request.LineID.ToString(),
            LineIndex);
        QueueDeferredNPCLineDispatch(BufferedLine.Request.ConversationID, BufferedLine.Request);
        return false;
    }

    ActiveTTSGenerationIndex = LineIndex;
    ActiveTurnLatencyTrace.SetCurrentLine(BufferedLine.Request.NPCID, BufferedLine.Request.LineID);

    if (UOffgridAILineCoach* LineCoach = FindLineCoachByID(BufferedLine.Request.NPCID))
    {
        if (LineCoach->ShouldUseVoiceDesignForTTS())
        {
            BufferedLine.Request.TTSVoiceMode = EOffgridAITTSVoiceMode::VoiceDesign;
            BufferedLine.Request.bTTSVoiceDesign = true;
            BufferedLine.Request.TTSInstruction = LineCoach->BuildVoiceDesignInstructionForLine(BufferedLine.Request);
            BufferedLine.Request.VoiceID = NAME_None;

            FString IdentityPreview = LineCoach->VoiceDesignIdentity.Left(160);
            IdentityPreview.ReplaceInline(TEXT("\r"), TEXT(" "));
            IdentityPreview.ReplaceInline(TEXT("\n"), TEXT(" | "));
            FString DeliveryPreview = LineCoach->VoiceDesignNeutralDelivery.Left(160);
            DeliveryPreview.ReplaceInline(TEXT("\r"), TEXT(" "));
            DeliveryPreview.ReplaceInline(TEXT("\n"), TEXT(" | "));

            UE_LOG(LogOffgridAI, Log, TEXT("[TTS][VoiceDesign] line prepared npc=%s line=%s identity=\"%s\" delivery=\"%s\" instruction_len=%d"),
                *BufferedLine.Request.NPCID.ToString(),
                *BufferedLine.Request.LineID.ToString(),
                *IdentityPreview,
                *DeliveryPreview,
                BufferedLine.Request.TTSInstruction.Len());
        }
        else
        {
            BufferedLine.Request.TTSVoiceMode = EOffgridAITTSVoiceMode::CustomVoice;
            BufferedLine.Request.bTTSVoiceDesign = false;
            BufferedLine.Request.TTSInstruction.Reset();
        }
    }

    const FString TTSRequestDetails = FString::Printf(TEXT("npc=%s line=%s index=%d chars=%d"),
        *BufferedLine.Request.NPCID.ToString(),
        *BufferedLine.Request.LineID.ToString(),
        LineIndex,
        BufferedLine.Request.Dialogue.ToString().Len());

    // Canonical telemetry uses TTSRequestSent as the start mark for TTS first-audio latency.
    // Keep the existing descriptive mark as an alias for detailed logs/backward readability.
    NoteTurnLatencyEvent(TEXT("TTSRequestSent"), TTSRequestDetails);
    NoteTurnLatencyEvent(TEXT("NPCLineTTSGenerationRequested"), TTSRequestDetails);

    if (!Services->BeginTTSRequest(BufferedLine.Request))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[Orchestrator] TTS pipeline request rejected; will retry. npc=%s line=%s index=%d"),
            *BufferedLine.Request.NPCID.ToString(),
            *BufferedLine.Request.LineID.ToString(),
            LineIndex);
        ActiveTTSGenerationIndex = INDEX_NONE;
        QueueDeferredNPCLineDispatch(BufferedLine.Request.ConversationID, BufferedLine.Request);
        return false;
    }

    BufferedLine.bRequestAccepted = true;
    UE_LOG(LogOffgridAI, Log, TEXT("[TTS_TRACE] T0 request_created npc=%s line=%s index=%d"),
        *BufferedLine.Request.NPCID.ToString(),
        *BufferedLine.Request.LineID.ToString(),
        LineIndex);
    NoteTurnLatencyEvent(TEXT("TTSRequestReturned"), FString::Printf(TEXT("npc=%s line=%s index=%d"),
        *BufferedLine.Request.NPCID.ToString(),
        *BufferedLine.Request.LineID.ToString(),
        LineIndex));
    return true;
}

void UOffgridAIOrchestrator::DeliverBufferedTTSLineToLineCoach(FBufferedTTSLine& BufferedLine, UOffgridAILineCoach* LineCoach)
{
    if (!LineCoach)
    {
        return;
    }

    LineCoach->PerformLine(BufferedLine.Request);

    if (BufferedLine.bStreamStarted)
    {
        LineCoach->BeginOutputAudioStream(BufferedLine.Request.LineID, BufferedLine.SampleRate, BufferedLine.NumChannels);
        for (const FBufferedTTSLine::FBufferedPCMChunk& BufferedChunk : BufferedLine.PCMChunks)
        {
            LineCoach->SubmitOutputAudioChunk(BufferedLine.Request.LineID, BufferedChunk.PCM, BufferedLine.SampleRate, BufferedLine.NumChannels, BufferedChunk.ChunkStartSample, BufferedChunk.ChunkSampleCount);
        }
        BufferedLine.PCMChunks.Empty();
    }

    if (BufferedLine.bStreamCompleted)
    {
        LineCoach->EndOutputAudioStream(BufferedLine.Request.LineID);
    }
}

bool UOffgridAIOrchestrator::StartPlaybackForLineIndex(int32 LineIndex)
{
    if (!NPCLinePipeline.IsValidIndex(LineIndex))
    {
        return false;
    }

    FBufferedTTSLine& BufferedLine = NPCLinePipeline[LineIndex];
    UOffgridAILineCoach* LineCoach = FindLineCoachByID(BufferedLine.Request.NPCID);
    if (!LineCoach)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[Orchestrator] Cannot start playback; LineCoach for NPC %s was not found"), *BufferedLine.Request.NPCID.ToString());
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return false;
    }

    if (!BufferedLine.bRequestAccepted)
    {
        if (!RequestTTSSynthesisForLineIndex(LineIndex))
        {
            return false;
        }
    }

    NoteTurnLatencyEvent(TEXT("NPCLinePlaybackStartedByPipeline"), FString::Printf(TEXT("npc=%s line=%s index=%d buffered_chunks=%d completed=%s"),
        *BufferedLine.Request.NPCID.ToString(),
        *BufferedLine.Request.LineID.ToString(),
        LineIndex,
        BufferedLine.PCMChunks.Num(),
        BufferedLine.bStreamCompleted ? TEXT("true") : TEXT("false")));

    DeliverBufferedTTSLineToLineCoach(BufferedLine, LineCoach);
    return true;
}

void UOffgridAIOrchestrator::PumpNPCLinePipeline()
{
    if (NPCLinePipeline.Num() == 0 || !IsActiveConversationID(NPCLinePipelineConversationID))
    {
        return;
    }

    if (ActiveNPCPlaybackIndex == INDEX_NONE)
    {
        ActiveNPCPlaybackIndex = 0;
    }

    if (NPCLinePipeline.IsValidIndex(ActiveNPCPlaybackIndex))
    {
        FBufferedTTSLine& PlaybackLine = NPCLinePipeline[ActiveNPCPlaybackIndex];
        UOffgridAILineCoach* LineCoach = FindLineCoachByID(PlaybackLine.Request.NPCID);
        const bool bAlreadyPerforming = LineCoach && LineCoach->IsPerformingLine(PlaybackLine.Request.LineID);
        if (!bAlreadyPerforming)
        {
            StartPlaybackForLineIndex(ActiveNPCPlaybackIndex);
        }
    }

    // Single-worker pre-generation: once the service has completed synthesis for the
    // current generated line, request the next line immediately. Playback remains
    // serial; chunks for future lines are buffered until their turn arrives.
    int32 NextGenerationIndex = INDEX_NONE;
    if (ActiveTTSGenerationIndex == INDEX_NONE)
    {
        for (int32 Index = 0; Index < NPCLinePipeline.Num(); ++Index)
        {
            if (!NPCLinePipeline[Index].bRequestAccepted && !NPCLinePipeline[Index].bStreamCompleted)
            {
                NextGenerationIndex = Index;
                break;
            }
        }
    }
    else if (NPCLinePipeline.IsValidIndex(ActiveTTSGenerationIndex) && NPCLinePipeline[ActiveTTSGenerationIndex].bStreamCompleted)
    {
        for (int32 Index = ActiveTTSGenerationIndex + 1; Index < NPCLinePipeline.Num(); ++Index)
        {
            if (!NPCLinePipeline[Index].bRequestAccepted && !NPCLinePipeline[Index].bStreamCompleted)
            {
                NextGenerationIndex = Index;
                break;
            }
        }
    }

    if (NextGenerationIndex != INDEX_NONE)
    {
        RequestTTSSynthesisForLineIndex(NextGenerationIndex);
    }
}

void UOffgridAIOrchestrator::HandleConversationStateChanged(const FGuid& ConversationID, EOffgridAIConversationState NewState)
{
    if (!IsActiveConversationID(ConversationID))
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        SetConversationState(EOffgridAIConversationState::Error);
        return;
    }

    NoteTurnLatencyEvent(TEXT("ConversationState"), StaticEnum<EOffgridAIConversationState>()->GetNameStringByValue(static_cast<int64>(NewState)));

    if (NewState == EOffgridAIConversationState::Error)
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        FinalizeTurnLatencyTrace(TEXT("conversation_error"));
        ClearRoundTripMetric();
    }
    else
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Ready);
    }

    if (NewState == EOffgridAIConversationState::AwaitingInput && ActiveTurnLatencyTrace.IsActive())
    {
        NoteTurnLatencyEvent(TEXT("ReturnedToAwaitingInput"));
        FinalizeTurnLatencyTrace(TEXT("completed"));
    }

    SetConversationState(NewState);
}

void UOffgridAIOrchestrator::PlayerTextTranscriptReady(const FGuid& ConversationID, const FOffgridAITranscriptLine& TranscriptLine)
{
    if (!IsActiveConversationID(ConversationID))
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    NoteTurnLatencyEvent(
        TEXT("PlayerTranscriptBroadcast"),
        FString::Printf(TEXT("speaker=%s chars=%d"), *TranscriptLine.SpeakerID.ToString(), TranscriptLine.Message.ToString().Len()));
    BroadcastTranscriptLine(TranscriptLine);
}

void UOffgridAIOrchestrator::NPCTextTranscriptReady(const FGuid& ConversationID, const FOffgridAITranscriptLine& TranscriptLine)
{
    if (!IsActiveConversationID(ConversationID))
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    NoteTurnLatencyEvent(
        TEXT("NPCTranscriptBroadcast"),
        FString::Printf(TEXT("speaker=%s chars=%d"), *TranscriptLine.SpeakerID.ToString(), TranscriptLine.Message.ToString().Len()));
    BroadcastTranscriptLine(TranscriptLine);
}

void UOffgridAIOrchestrator::NotifyLineFirstAudioSample(const FGuid& ConversationID, FName LineID, int32 BytesQueued)
{
    if (!IsActiveConversationID(ConversationID))
    {
        return;
    }

    if (!ActiveTurnLatencyTrace.HasFirstMark(TEXT("FirstAudioSample")))
    {
        NoteTurnLatencyEvent(TEXT("FirstAudioSample"), FString::Printf(TEXT("line=%s bytes=%d"), *LineID.ToString(), BytesQueued));
        CompleteRoundTripMetricIfActive(ConversationID, LineID);
    }
}

void UOffgridAIOrchestrator::NotifyLineOutputPlaybackStarted(const FGuid& ConversationID, FName LineID)
{
    if (!IsActiveConversationID(ConversationID))
    {
        return;
    }

    if (!ActiveTurnLatencyTrace.HasFirstMark(TEXT("PlaybackStarted")))
    {
        NoteTurnLatencyEvent(TEXT("PlaybackStarted"), FString::Printf(TEXT("line=%s"), *LineID.ToString()));
    }
}

void UOffgridAIOrchestrator::NotifyLinePerformanceCompleted(const FGuid& ConversationID, FName LineID)
{
    if (!IsActiveConversationID(ConversationID) || !ActiveConversationManager)
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    NoteTurnLatencyEvent(TEXT("LineCompleted"), FString::Printf(TEXT("line=%s"), *LineID.ToString()));

    const int32 CompletedPipelineIndex = FindBufferedLineIndexByLineID(LineID);
    ActiveConversationManager->NPCLinePerformanceComplete(LineID);

    if (CompletedPipelineIndex != INDEX_NONE && CompletedPipelineIndex == ActiveNPCPlaybackIndex)
    {
        ++ActiveNPCPlaybackIndex;
        if (NPCLinePipeline.IsValidIndex(ActiveNPCPlaybackIndex))
        {
            PumpNPCLinePipeline();
        }
        else
        {
            ClearNPCLineSequence();
        }
    }
}

bool UOffgridAIOrchestrator::HasActiveConversation() const
{
    return ActiveConversationManager != nullptr;
}

FGuid UOffgridAIOrchestrator::GetActiveConversationID() const
{
    return ActiveConversationManager ? ActiveConversationManager->GetConversationID() : FGuid();
}

TArray<FName> UOffgridAIOrchestrator::GetRegisteredPlayerIDs() const
{
    TArray<FName> Result;
    RegisteredBoomOperatorsByID.GetKeys(Result);
    Result.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
    return Result;
}

TArray<FName> UOffgridAIOrchestrator::GetRegisteredNPCIDs() const
{
    TArray<FName> Result;
    RegisteredLineCoachesByID.GetKeys(Result);
    Result.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
    return Result;
}

bool UOffgridAIOrchestrator::TryGetVoiceIDForNPC(FName NPCID, FName& OutVoiceID) const
{
    if (const TObjectPtr<UOffgridAILineCoach>* FoundLineCoach = RegisteredLineCoachesByID.Find(NPCID))
    {
        if (FoundLineCoach->Get())
        {
            OutVoiceID = (*FoundLineCoach)->VoiceID;
            return true;
        }
    }

    return false;
}

bool UOffgridAIOrchestrator::GetNPCStartingPADSState(FName NPCID, FOffgridAIPADSState& OutState) const
{
    if (const TObjectPtr<UOffgridAILineCoach>* Found = RegisteredLineCoachesByID.Find(NPCID))
    {
        if (const UOffgridAILineCoach* LineCoach = Found->Get())
        {
            OutState = LineCoach->GetStartingPADSState();
            return true;
        }
    }
    return false;
}

bool UOffgridAIOrchestrator::GetNPCSupportedEmotionNames(FName NPCID, TArray<FName>& OutSupportedEmotionNames) const
{
    OutSupportedEmotionNames.Reset();

    if (const TObjectPtr<UOffgridAILineCoach>* FoundLineCoach = RegisteredLineCoachesByID.Find(NPCID))
    {
        if (FoundLineCoach->Get())
        {
            OutSupportedEmotionNames = (*FoundLineCoach)->GetConfiguredSupportedEmotionNames();
            return OutSupportedEmotionNames.Num() > 0;
        }
    }

    return false;
}

TArray<FName> UOffgridAIOrchestrator::GetSupportedEmotionNamesForNPCs(const TArray<FName>& NPCIDs) const
{
    TArray<FName> Result;
    for (const FName NPCID : NPCIDs)
    {
        TArray<FName> NPCEmotions;
        if (GetNPCSupportedEmotionNames(NPCID, NPCEmotions))
        {
            for (const FName Emotion : NPCEmotions)
            {
                if (Emotion != NAME_None)
                {
                    Result.AddUnique(Emotion);
                }
            }
        }
    }
    return Result;
}

void UOffgridAIOrchestrator::HandleASRFinalTranscript(const FGuid& ConversationID, FName PlayerID, const FText& Transcript)
{
    if (!IsActiveConversationID(ConversationID) || !ActiveConversationManager)
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    if (!ActiveConversationManager->GetPlayerIDs().Contains(PlayerID))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("ASR transcript arrived for unknown player %s"), *PlayerID.ToString());
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    const FString TrimmedTranscript = Transcript.ToString().TrimStartAndEnd();
    NoteTurnLatencyEvent(TEXT("ASRFinalReceived"), FString::Printf(TEXT("player=%s chars=%d"), *PlayerID.ToString(), TrimmedTranscript.Len()));
    if (TrimmedTranscript.IsEmpty())
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("ASR returned an empty transcript; cancelling current turn."));
        NoteTurnLatencyEvent(TEXT("TurnCancelledEmptyTranscript"));
        FinalizeTurnLatencyTrace(TEXT("empty_transcript"));
        ClearRoundTripMetric();
        ActiveConversationManager->CancelCurrentTurn();
        return;
    }

    ActiveConversationManager->SubmitPlayerTurnText(PlayerID, FText::FromString(TrimmedTranscript));
}

void UOffgridAIOrchestrator::HandleLLMResponse(const FGuid& ConversationID, const FString& JSONPayload)
{
    if (!IsActiveConversationID(ConversationID) || !ActiveConversationManager)
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    if (bAwaitingPlayerImpulseClassifierResponse && ConversationID == PendingPlayerImpulseConversationID)
    {
        NoteTurnLatencyEvent(TEXT("PlayerImpulseClassifierResponseReceived"), FString::Printf(TEXT("chars=%d"), JSONPayload.Len()));

        ActiveConversationManager->ApplyPlayerImpulseClassifierResult(JSONPayload, PendingPlayerImpulseText);

        const FText DialoguePlayerText = PendingPlayerImpulseText;
        bAwaitingPlayerImpulseClassifierResponse = false;
        PendingPlayerImpulseConversationID = FGuid();
        PendingPlayerImpulseText = FText::GetEmpty();

        SubmitDialogueTurnToLLM(ConversationID, DialoguePlayerText);
        return;
    }

    if (!ActiveTurnLatencyTrace.HasFirstMark(TEXT("LLMFirstTokenReceived")))
    {
        NoteTurnLatencyEvent(TEXT("LLMFirstTokenReceived"), TEXT("fallback=response_received"));
    }


    NoteTurnLatencyEvent(TEXT("LLMResponseReceived"), FString::Printf(TEXT("chars=%d"), JSONPayload.Len()));
    ActiveConversationManager->SubmitNPCTurnJSON(JSONPayload);
}

void UOffgridAIOrchestrator::HandleTTSStreamStarted(const FGuid& ConversationID, FName NPCID, FName LineID, int32 SampleRate, int32 NumChannels)
{
    if (!IsActiveConversationID(ConversationID))
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    const int32 BufferedLineIndex = FindBufferedLineIndexByLineID(LineID);
    if (!NPCLinePipeline.IsValidIndex(BufferedLineIndex))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("Ignoring TTS stream start for untracked line. npc=%s line=%s"), *NPCID.ToString(), *LineID.ToString());
        return;
    }

    FBufferedTTSLine& BufferedLine = NPCLinePipeline[BufferedLineIndex];
    if (BufferedLine.Request.NPCID != NPCID)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("TTS stream start NPC mismatch. expected=%s actual=%s line=%s"),
            *BufferedLine.Request.NPCID.ToString(), *NPCID.ToString(), *LineID.ToString());
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    BufferedLine.bStreamStarted = true;
    BufferedLine.SampleRate = SampleRate;
    BufferedLine.NumChannels = NumChannels;

    NoteTurnLatencyEvent(TEXT("TTSStreamStarted"), FString::Printf(TEXT("npc=%s line=%s sample_rate=%d channels=%d index=%d"), *NPCID.ToString(), *LineID.ToString(), SampleRate, NumChannels, BufferedLineIndex));

    UOffgridAILineCoach* LineCoach = FindLineCoachByID(NPCID);
    if (LineCoach && LineCoach->IsPerformingLine(LineID))
    {
        LineCoach->BeginOutputAudioStream(LineID, SampleRate, NumChannels);
    }
}

void UOffgridAIOrchestrator::HandleTTSStreamChunk(const FGuid& ConversationID, FName NPCID, FName LineID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample, int32 ChunkSampleCount)
{
    if (!IsActiveConversationID(ConversationID))
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    const int32 BufferedLineIndex = FindBufferedLineIndexByLineID(LineID);
    if (!NPCLinePipeline.IsValidIndex(BufferedLineIndex))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("Ignoring TTS audio chunk for untracked line. npc=%s line=%s bytes=%d"), *NPCID.ToString(), *LineID.ToString(), PCMChunk.Num());
        return;
    }

    FBufferedTTSLine& BufferedLine = NPCLinePipeline[BufferedLineIndex];
    if (BufferedLine.Request.NPCID != NPCID)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("TTS stream chunk NPC mismatch. expected=%s actual=%s line=%s"),
            *BufferedLine.Request.NPCID.ToString(), *NPCID.ToString(), *LineID.ToString());
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    if (!ActiveTurnLatencyTrace.HasFirstMark(TEXT("FirstAudioChunkReceived")))
    {
        NoteTurnLatencyEvent(TEXT("FirstAudioChunkReceived"), FString::Printf(TEXT("npc=%s line=%s bytes=%d sample_rate=%d channels=%d index=%d"), *NPCID.ToString(), *LineID.ToString(), PCMChunk.Num(), SampleRate, NumChannels, BufferedLineIndex));
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[TTS_TRACE] T5b orchestrator_audio_chunk npc=%s line=%s index=%d bytes=%d sr=%d channels=%d start_sample=%lld samples=%d"),
        *NPCID.ToString(),
        *LineID.ToString(),
        BufferedLineIndex,
        PCMChunk.Num(),
        SampleRate,
        NumChannels,
        ChunkStartSample,
        ChunkSampleCount);

    UOffgridAILineCoach* LineCoach = FindLineCoachByID(NPCID);
    if (LineCoach && LineCoach->IsPerformingLine(LineID))
    {
        LineCoach->SubmitOutputAudioChunk(LineID, PCMChunk, SampleRate, NumChannels, ChunkStartSample, ChunkSampleCount);
    }
    else
    {
        FBufferedTTSLine::FBufferedPCMChunk& BufferedChunk = BufferedLine.PCMChunks.AddDefaulted_GetRef();
        BufferedChunk.PCM = PCMChunk;
        BufferedChunk.ChunkStartSample = ChunkStartSample;
        BufferedChunk.ChunkSampleCount = ChunkSampleCount;
        UE_LOG(LogOffgridAI, Verbose, TEXT("Buffered future TTS chunk npc=%s line=%s index=%d bytes=%d buffered_chunks=%d"),
            *NPCID.ToString(), *LineID.ToString(), BufferedLineIndex, PCMChunk.Num(), BufferedLine.PCMChunks.Num());
    }
}

void UOffgridAIOrchestrator::HandleTTSStreamCompleted(const FGuid& ConversationID, FName NPCID, FName LineID)
{
    if (!IsActiveConversationID(ConversationID))
    {
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    const int32 BufferedLineIndex = FindBufferedLineIndexByLineID(LineID);
    if (!NPCLinePipeline.IsValidIndex(BufferedLineIndex))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("Ignoring TTS completion for untracked line. npc=%s line=%s"), *NPCID.ToString(), *LineID.ToString());
        return;
    }

    FBufferedTTSLine& BufferedLine = NPCLinePipeline[BufferedLineIndex];
    if (BufferedLine.Request.NPCID != NPCID)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("TTS stream completion NPC mismatch. expected=%s actual=%s line=%s"),
            *BufferedLine.Request.NPCID.ToString(), *NPCID.ToString(), *LineID.ToString());
        SetOrchestratorState(EOffgridAIOrchestratorState::Error);
        return;
    }

    BufferedLine.bStreamCompleted = true;

    NoteTurnLatencyEvent(TEXT("TTSStreamCompleted"), FString::Printf(TEXT("npc=%s line=%s index=%d buffered_chunks=%d"), *NPCID.ToString(), *LineID.ToString(), BufferedLineIndex, BufferedLine.PCMChunks.Num()));

    UOffgridAILineCoach* LineCoach = FindLineCoachByID(NPCID);
    if (LineCoach && LineCoach->IsPerformingLine(LineID))
    {
        LineCoach->EndOutputAudioStream(LineID);
    }

    if (ActiveTTSGenerationIndex == BufferedLineIndex)
    {
        ActiveTTSGenerationIndex = INDEX_NONE;
    }

    PumpNPCLinePipeline();
}

void UOffgridAIOrchestrator::HandleServiceError(const FGuid& ConversationID, const FString& Context)
{
    UE_LOG(LogOffgridAI, Error, TEXT("Service adapter error%s%s"),
        Context.IsEmpty() ? TEXT("") : TEXT(": "),
        Context.IsEmpty() ? TEXT("") : *Context);

    NoteTurnLatencyEvent(TEXT("ServiceError"), Context);
    FinalizeTurnLatencyTrace(TEXT("service_error"));
    ClearRoundTripMetric();
    ResetRuntime();
    SetOrchestratorState(EOffgridAIOrchestratorState::Error);
}


void UOffgridAIOrchestrator::NoteTurnLatencyEvent(const FString& EventName, const FString& Detail)
{
    if (!ActiveTurnLatencyTrace.IsActive())
    {
        return;
    }

    ActiveTurnLatencyTrace.Mark(EventName, Detail);
}

void UOffgridAIOrchestrator::SetOrchestratorState(EOffgridAIOrchestratorState NewState)
{
    if (CurrentOrchestratorState == NewState)
    {
        return;
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[Orchestrator][State] %s -> %s"),
        *StaticEnum<EOffgridAIOrchestratorState>()->GetNameStringByValue(static_cast<int64>(CurrentOrchestratorState)),
        *StaticEnum<EOffgridAIOrchestratorState>()->GetNameStringByValue(static_cast<int64>(NewState)));

    CurrentOrchestratorState = NewState;
    OnOrchestratorStateChanged.Broadcast(NewState);
}

void UOffgridAIOrchestrator::SetConversationState(EOffgridAIConversationState NewState)
{
    if (CurrentConversationState == NewState)
    {
        return;
    }

    UE_LOG(LogOffgridAI, Verbose, TEXT("[Conversation][State] %s -> %s"),
        *StaticEnum<EOffgridAIConversationState>()->GetNameStringByValue(static_cast<int64>(CurrentConversationState)),
        *StaticEnum<EOffgridAIConversationState>()->GetNameStringByValue(static_cast<int64>(NewState)));

    CurrentConversationState = NewState;
    OnConversationStateChanged.Broadcast(NewState);
}

void UOffgridAIOrchestrator::BroadcastTranscriptLine(const FOffgridAITranscriptLine& TranscriptLine)
{
    OnTranscriptLineAdded.Broadcast(TranscriptLine);
}

bool UOffgridAIOrchestrator::HasRequiredRegistrants() const
{
    for (const TPair<FName, TObjectPtr<UOffgridAIBoomOperator>>& Pair : RegisteredBoomOperatorsByID)
    {
        if (Pair.Value.Get())
        {
            for (const TPair<FName, TObjectPtr<UOffgridAILineCoach>>& CoachPair : RegisteredLineCoachesByID)
            {
                if (CoachPair.Value.Get())
                {
                    return true;
                }
            }
            break;
        }
    }

    return false;
}

bool UOffgridAIOrchestrator::ValidateConversationParticipants(const TArray<FName>& PlayerIDs, const TArray<FName>& NPCIDs) const
{
    for (const FName PlayerID : PlayerIDs)
    {
        const TObjectPtr<UOffgridAIBoomOperator>* Found = RegisteredBoomOperatorsByID.Find(PlayerID);
        if (!Found || !Found->Get())
        {
            return false;
        }
    }

    for (const FName NPCID : NPCIDs)
    {
        const TObjectPtr<UOffgridAILineCoach>* Found = RegisteredLineCoachesByID.Find(NPCID);
        if (!Found || !Found->Get())
        {
            return false;
        }
    }

    return true;
}

bool UOffgridAIOrchestrator::IsActiveConversationID(const FGuid& ConversationID) const
{
    return ActiveConversationManager && ActiveConversationManager->GetConversationID() == ConversationID;
}

UOffgridAIBoomOperator* UOffgridAIOrchestrator::FindBoomOperatorByID(FName PlayerID) const
{
    if (const TObjectPtr<UOffgridAIBoomOperator>* FoundBoomOperator = RegisteredBoomOperatorsByID.Find(PlayerID))
    {
        return FoundBoomOperator->Get();
    }

    return nullptr;
}

void UOffgridAIOrchestrator::DriveNPCEmotionExpression(FName NPCID, FName Emotion, float Magnitude)
{
    UOffgridAILineCoach* LineCoach = FindLineCoachByID(NPCID);
    if (!LineCoach)
    {
        return;
    }

    LineCoach->SubmitDrivenEmotionExpression(Emotion, Magnitude);
}

bool UOffgridAIOrchestrator::HandleEmotionTransitionTick(float DeltaTimeSeconds)
{
    if (ActiveConversationManager)
    {
        ActiveConversationManager->TickEmotionTransitions(DeltaTimeSeconds);
    }

    return true;
}

UOffgridAILineCoach* UOffgridAIOrchestrator::FindLineCoachByID(FName NPCID) const
{
    if (const TObjectPtr<UOffgridAILineCoach>* FoundLineCoach = RegisteredLineCoachesByID.Find(NPCID))
    {
        return FoundLineCoach->Get();
    }

    return nullptr;
}

bool UOffgridAIOrchestrator::AreRequiredServicesReady() const
{
    return Services ? Services->AreRequiredServicesReady() : false;
}

FOffgridAIServiceStatus UOffgridAIOrchestrator::GetServiceStatus(EOffgridAIServiceKind ServiceKind) const
{
    return Services ? Services->GetServiceStatus(ServiceKind) : FOffgridAIServiceStatus();
}