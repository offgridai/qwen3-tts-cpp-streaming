#include "ASR/OffgridAIASRNamedPipeService.h"

#include "ASR/OffgridAIASRProtocol.h"
#include "ASR/Transport/OffgridAIASRPipeClient.h"
#include "Core/OffgridAIServiceProcessLauncher.h"
#include "Misc/Paths.h"

namespace
{
    FString OffgridAIResolveFullPath(const FString& InPath)
    {
        FString Path = InPath;
        FPaths::NormalizeFilename(Path);
        return FPaths::ConvertRelativePathToFull(Path);
    }

    bool OffgridAIFileExists(const FString& InPath)
    {
        return !InPath.IsEmpty() && FPaths::FileExists(OffgridAIResolveFullPath(InPath));
    }

    bool OffgridAIDirectoryExists(const FString& InPath)
    {
        return !InPath.IsEmpty() && FPaths::DirectoryExists(OffgridAIResolveFullPath(InPath));
    }
}

bool FOffgridAIASRNamedPipeService::StartupServiceIfNeeded(FString* OutFatalError)
{
    if (bStartupCompleted)
    {
        return true;
    }

    FOffgridAIASRRuntimeConfig RuntimeConfig;
    FString Error;
    if (!BuildRuntimeConfig(RuntimeConfig, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("ASR NamedPipeService: invalid startup config: %s"), *Error);
        if (OutFatalError)
        {
            *OutFatalError = Error;
        }
        return false;
    }

    FOffgridAIASRResponse Response;
    const TArray<uint8> EmptyPayload;
    if (!SendRequest(EOffgridAIASROp::Startup, FString(), EmptyPayload, 0, 0, FString(), &Response, true, &RuntimeConfig))
    {
        return false;
    }

    if (!Response.bOk)
    {
        const FString FatalError = Response.ErrorMessage.IsEmpty()
            ? TEXT("ASR startup failed with no error message.")
            : Response.ErrorMessage;
        UE_LOG(LogTemp, Error, TEXT("ASR NamedPipeService: startup failed: %s"), *FatalError);
        if (OutFatalError)
        {
            *OutFatalError = FatalError;
        }
        return false;
    }

    bStartupCompleted = true;
    return true;
}

bool FOffgridAIASRNamedPipeService::ValidateStartupSettings(FString& OutError) const
{
    OutError.Reset();

    if (!Settings)
    {
        OutError = TEXT("ASR settings asset is null.");
        return false;
    }

    if (Settings->ServiceExecutablePath.IsEmpty())
    {
        OutError = TEXT("ASR ServiceExecutablePath is empty.");
        return false;
    }

    if (!OffgridAIFileExists(Settings->ServiceExecutablePath))
    {
        OutError = FString::Printf(TEXT("ASR ServiceExecutablePath does not exist: %s"), *Settings->ServiceExecutablePath);
        return false;
    }

    if (!Settings->ServiceWorkingDirectory.IsEmpty() && !OffgridAIDirectoryExists(Settings->ServiceWorkingDirectory))
    {
        OutError = FString::Printf(TEXT("ASR ServiceWorkingDirectory does not exist: %s"), *Settings->ServiceWorkingDirectory);
        return false;
    }

    const FString ModelDirectory = Settings->ActiveModelDirectory.IsEmpty()
        ? Settings->ServiceWorkingDirectory
        : Settings->ActiveModelDirectory;

    if (ModelDirectory.IsEmpty())
    {
        OutError = TEXT("ASR ActiveModelDirectory is empty and ServiceWorkingDirectory cannot be used as a fallback.");
        return false;
    }

    if (!OffgridAIDirectoryExists(ModelDirectory))
    {
        OutError = FString::Printf(TEXT("ASR ActiveModelDirectory does not exist: %s"), *ModelDirectory);
        return false;
    }

    return true;
}

bool FOffgridAIASRNamedPipeService::EnsureServiceReady(FString& OutError)
{
    OutError.Reset();

    if (!ValidateStartupSettings(OutError))
    {
        return false;
    }

    if (!EnsureServiceConnected())
    {
        // Not connected yet is normally transient during startup; keep polling until supervisor timeout.
        return false;
    }

    if (!StartupServiceIfNeeded(&OutError))
    {
        // Empty OutError means transient startup/pipe race; non-empty means deterministic fatal startup failure.
        return false;
    }

    return true;
}

FOffgridAIASRNamedPipeService::FOffgridAIASRNamedPipeService(const UOffgridAIASRServiceSettingsDataAsset* InSettings)
    : Settings(InSettings)
    , PipeClient(MakeUnique<FOffgridAIASRPipeClient>())
{
}

FOffgridAIASRNamedPipeService::~FOffgridAIASRNamedPipeService()
{
    Reset();
}

void FOffgridAIASRNamedPipeService::Reset()
{
    ActiveSession = FActiveSession();

    FOffgridAIASRCompletedRequest Discarded;
    while (CompletedQueue.Dequeue(Discarded))
    {
    }

    CapturedPCMBuffer.Reset();
    CapturedSampleRate = 0;
    CapturedNumChannels = 0;
    SubmittedPCMBytes = 0;

    if (PipeClient && PipeClient->IsConnected())
    {
        const TArray<uint8> EmptyPayload;
        SendRequest(EOffgridAIASROp::Shutdown, FString(), EmptyPayload, 0, 0, FString(), nullptr, false);
        PipeClient->Disconnect();
    }

    bStartupCompleted = false;
    FOffgridAIServiceProcessLauncher::Terminate(ServiceProcessHandle);
    ServiceProcessId = 0;
}

void FOffgridAIASRNamedPipeService::Tick(double NowSeconds)
{
}

bool FOffgridAIASRNamedPipeService::BeginPlayerAudioInput(const FGuid& ConversationID, FName PlayerID)
{
    UE_LOG(LogTemp, Log, TEXT("ASR NamedPipeService: BeginPlayerAudioInput entered. active=%s conversation=%s player=%s"),
        ActiveSession.bActive ? TEXT("true") : TEXT("false"),
        *ConversationID.ToString(),
        *PlayerID.ToString());

    if (ActiveSession.bActive || !EnsureServiceConnected() || !StartupServiceIfNeeded())
    {
        return false;
    }

    ActiveSession.bActive = true;
    ActiveSession.ConversationID = ConversationID;
    ActiveSession.PlayerID = PlayerID;
    ActiveSession.RequestID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);

    CapturedPCMBuffer.Reset();
    CapturedSampleRate = 0;
    CapturedNumChannels = 0;
    SubmittedPCMBytes = 0;

    const TArray<uint8> EmptyPayload;
    FOffgridAIASRResponse Response;
    if (!SendRequest(EOffgridAIASROp::StartUtterance, ActiveSession.RequestID, EmptyPayload, 0, 0, FString(), &Response, true))
    {
        ActiveSession = FActiveSession();
        return false;
    }

    if (!Response.bOk)
    {
        UE_LOG(LogTemp, Error, TEXT("ASR NamedPipeService: start_utterance failed: %s"), *Response.ErrorMessage);
        ActiveSession = FActiveSession();
        return false;
    }

    return true;
}

void FOffgridAIASRNamedPipeService::SubmitPlayerAudioChunk(
    const FGuid& ConversationID,
    FName PlayerID,
    const TArray<uint8>& PCMData,
    int32 SampleRate,
    int32 NumChannels)
{
    if (!ActiveSession.bActive)
    {
        return;
    }

    if (ActiveSession.ConversationID != ConversationID || ActiveSession.PlayerID != PlayerID)
    {
        return;
    }

    if (PCMData.Num() <= 0)
    {
        return;
    }

    CapturedPCMBuffer.Append(PCMData);
    CapturedSampleRate = SampleRate;
    CapturedNumChannels = NumChannels;

    UE_LOG(LogTemp, Verbose,
        TEXT("ASR push_audio request=%s bytes=%d sampleRate=%d channels=%d submitted=%d captured=%d"),
        *ActiveSession.RequestID,
        PCMData.Num(),
        SampleRate,
        NumChannels,
        SubmittedPCMBytes,
        CapturedPCMBuffer.Num());

    if (!SendRequest(
        EOffgridAIASROp::PushAudio,
        ActiveSession.RequestID,
        PCMData,
        SampleRate,
        NumChannels,
        GetConfiguredSampleFormat(),
        nullptr,
        false))
    {
        UE_LOG(LogTemp, Error, TEXT("ASR NamedPipeService: push_audio failed for request %s"), *ActiveSession.RequestID);
        return;
    }

    SubmittedPCMBytes += PCMData.Num();
}

bool FOffgridAIASRNamedPipeService::EndPlayerAudioInput(
    const FString& RequestID,
    const FGuid& ConversationID,
    FName PlayerID)
{
    UE_LOG(LogTemp, Log,
        TEXT("ASR NamedPipeService: EndPlayerAudioInput entered. ParamRequestID=%s ParamConversationID=%s ParamPlayerID=%s SessionActive=%s SessionRequestID=%s SessionConversationID=%s SessionPlayerID=%s"),
        *RequestID,
        *ConversationID.ToString(),
        *PlayerID.ToString(),
        ActiveSession.bActive ? TEXT("true") : TEXT("false"),
        *ActiveSession.RequestID,
        *ActiveSession.ConversationID.ToString(),
        *ActiveSession.PlayerID.ToString());

    if (!ActiveSession.bActive)
    {
        UE_LOG(LogTemp, Warning, TEXT("ASR NamedPipeService: EndPlayerAudioInput rejected because no active session exists."));
        return false;
    }

    if (ActiveSession.ConversationID != ConversationID || ActiveSession.PlayerID != PlayerID)
    {
        UE_LOG(LogTemp, Error,
            TEXT("ASR NamedPipeService: EndPlayerAudioInput rejected because session identity mismatched. ParamConversationID=%s ParamPlayerID=%s SessionConversationID=%s SessionPlayerID=%s"),
            *ConversationID.ToString(),
            *PlayerID.ToString(),
            *ActiveSession.ConversationID.ToString(),
            *ActiveSession.PlayerID.ToString());
        return false;
    }

    const FString EffectiveRequestID = ActiveSession.RequestID;

    if (!RequestID.IsEmpty() && RequestID != EffectiveRequestID)
    {
        UE_LOG(LogTemp, Verbose,
            TEXT("ASR NamedPipeService: ignoring mismatched EndPlayerAudioInput request id. ParamRequestID=%s SessionRequestID=%s"),
            *RequestID,
            *EffectiveRequestID);
    }

    // Flush any captured tail that never got pushed yet.
    if (CapturedPCMBuffer.Num() > SubmittedPCMBytes && CapturedSampleRate > 0 && CapturedNumChannels > 0)
    {
        const int32 RemainingBytes = CapturedPCMBuffer.Num() - SubmittedPCMBytes;

        TArray<uint8> RemainingPCM;
        RemainingPCM.Append(CapturedPCMBuffer.GetData() + SubmittedPCMBytes, RemainingBytes);

        UE_LOG(LogTemp, Warning,
            TEXT("ASR final flush: request=%s remaining=%d totalCaptured=%d submitted=%d sampleRate=%d channels=%d"),
            *EffectiveRequestID,
            RemainingBytes,
            CapturedPCMBuffer.Num(),
            SubmittedPCMBytes,
            CapturedSampleRate,
            CapturedNumChannels);

        if (!SendRequest(
            EOffgridAIASROp::PushAudio,
            EffectiveRequestID,
            RemainingPCM,
            CapturedSampleRate,
            CapturedNumChannels,
            GetConfiguredSampleFormat(),
            nullptr,
            false))
        {
            UE_LOG(LogTemp, Error, TEXT("ASR NamedPipeService: final flush push_audio failed for request %s"), *EffectiveRequestID);
            return false;
        }

        SubmittedPCMBytes = CapturedPCMBuffer.Num();
    }

    FOffgridAIASRResponse Response;
    const TArray<uint8> EmptyPayload;
    UE_LOG(LogTemp, Log, TEXT("ASR NamedPipeService: sending finalize_utterance request_id=%s"), *EffectiveRequestID);

    if (!SendRequest(EOffgridAIASROp::FinalizeUtterance, EffectiveRequestID, EmptyPayload, 0, 0, FString(), &Response, true))
    {
        UE_LOG(LogTemp, Error, TEXT("ASR NamedPipeService: finalize SendRequest failed for request %s"), *EffectiveRequestID);
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("ASR NamedPipeService: finalize response ok=%s error='%s' transcript='%s'"),
        Response.bOk ? TEXT("true") : TEXT("false"),
        *Response.ErrorMessage,
        *Response.Transcript);

    if (!Response.bOk)
    {
        UE_LOG(LogTemp, Error, TEXT("ASR NamedPipeService: finalize failed for request %s: %s"), *EffectiveRequestID, *Response.ErrorMessage);
        ActiveSession = FActiveSession();
        SubmittedPCMBytes = 0;
        return false;
    }

    FOffgridAIASRCompletedRequest Result;
    Result.RequestID = EffectiveRequestID;
    Result.ConversationID = ActiveSession.ConversationID;
    Result.PlayerID = ActiveSession.PlayerID;
    Result.Transcript = FText::FromString(Response.Transcript);

    CompletedQueue.Enqueue(Result);
    ActiveSession = FActiveSession();
    SubmittedPCMBytes = 0;
    return true;
}

bool FOffgridAIASRNamedPipeService::TryPopCompletedRequest(FOffgridAIASRCompletedRequest& OutResult)
{
    return CompletedQueue.Dequeue(OutResult);
}

bool FOffgridAIASRNamedPipeService::GetMostRecentCapture(
    TArray<uint8>& OutPCM,
    int32& OutSampleRate,
    int32& OutNumChannels) const
{
    if (CapturedPCMBuffer.Num() <= 0 || CapturedSampleRate <= 0 || CapturedNumChannels <= 0)
    {
        return false;
    }

    OutPCM = CapturedPCMBuffer;
    OutSampleRate = CapturedSampleRate;
    OutNumChannels = CapturedNumChannels;
    return true;
}

bool FOffgridAIASRNamedPipeService::EnsureServiceConnected()
{
    if (!PipeClient)
    {
        PipeClient = MakeUnique<FOffgridAIASRPipeClient>();
    }

    if (PipeClient->IsConnected())
    {
        return true;
    }

    if (Settings && !Settings->ServiceExecutablePath.IsEmpty() && !ServiceProcessHandle.IsValid())
    {
        const FString Args = BuildLaunchArguments();
        ServiceProcessHandle = FOffgridAIServiceProcessLauncher::LaunchDetached(
            Settings->ServiceExecutablePath,
            Args,
            Settings->ServiceWorkingDirectory,
            ServiceProcessId);

        UE_LOG(LogTemp, Log, TEXT("ASR NamedPipeService: launched process. Handle valid=%s PID=%u"),
            ServiceProcessHandle.IsValid() ? TEXT("true") : TEXT("false"),
            ServiceProcessId);
    }

    const FString PipeName = Settings ? Settings->PipeName : TEXT(R"(\\.\pipe\OffgridAI_ASR)");

    // Do not block here. Startup polling is driven by FOffgridAILocalServiceGateway::TickServiceStartup.
    // A single connect attempt per tick lets ASR, LLM, and TTS launch in parallel instead of
    // waiting for each service's full pipe timeout before starting the next one.
    if (PipeClient->Connect(PipeName))
    {
        return true;
    }

    return false;

}

bool FOffgridAIASRNamedPipeService::BuildRuntimeConfig(FOffgridAIASRRuntimeConfig& OutConfig, FString& OutError) const
{
    if (!Settings)
    {
        OutError = TEXT("ASR settings asset is null.");
        return false;
    }

    FString ModelDir = Settings->ActiveModelDirectory;

    if (ModelDir.IsEmpty())
    {
        ModelDir = Settings->ServiceWorkingDirectory;
    }

    // FIX: repair and normalize path
    FPaths::NormalizeDirectoryName(ModelDir);
    ModelDir = FPaths::ConvertRelativePathToFull(ModelDir);

    OutConfig.ActiveModelDirectory = ModelDir;

    OutConfig.Provider = Settings->Provider;
    OutConfig.DecodingMethod = Settings->DecodingMethod;
    OutConfig.NumThreads = Settings->NumThreads;
    OutConfig.MaxActivePaths = Settings->MaxActivePaths;
    OutConfig.ExpectedSampleRate = Settings->ModelSampleRate;
    OutConfig.FeatureDim = Settings->FeatureDim;
    OutConfig.bModelDebug = Settings->bModelDebug;
    OutConfig.FinalizeSilencePaddingMs = Settings->FinalizeSilencePaddingMs;
    OutConfig.FinalizeSettleDelayMs = Settings->FinalizeSettleDelayMs;

    UE_LOG(LogTemp, Warning, TEXT("ASR normalized model dir: %s"), *OutConfig.ActiveModelDirectory);

    return OutConfig.IsValid(OutError);
}

bool FOffgridAIASRNamedPipeService::SendRequest(
    EOffgridAIASROp Op,
    const FString& RequestId,
    const TArray<uint8>& Payload,
    int32 SampleRate,
    int32 NumChannels,
    const FString& SampleFormat,
    FOffgridAIASRResponse* OutResponse,
    bool bExpectResponse,
    const FOffgridAIASRRuntimeConfig* RuntimeConfig)
{
    if (!EnsureServiceConnected() || !PipeClient)
    {
        return false;
    }

    FOffgridAIASRRequest Request;
    Request.Op = Op;
    Request.RequestId = RequestId;
    Request.Payload = Payload;
    Request.SampleRateHz = SampleRate;
    Request.NumChannels = NumChannels;
    Request.SampleFormat = SampleFormat;

    if (RuntimeConfig)
    {
        Request.ActiveModelDirectory = RuntimeConfig->ActiveModelDirectory;
        Request.Provider = RuntimeConfig->Provider;
        Request.DecodingMethod = RuntimeConfig->DecodingMethod;
        Request.NumThreads = RuntimeConfig->NumThreads;
        Request.MaxActivePaths = RuntimeConfig->MaxActivePaths;
        Request.ExpectedSampleRate = RuntimeConfig->ExpectedSampleRate;
        Request.FeatureDim = RuntimeConfig->FeatureDim;
        Request.bModelDebug = RuntimeConfig->bModelDebug;
        Request.FinalizeSilencePaddingMs = RuntimeConfig->FinalizeSilencePaddingMs;
        Request.FinalizeSettleDelayMs = RuntimeConfig->FinalizeSettleDelayMs;
    }

    TArray<uint8> RequestBytes;
    if (!OffgridAIASRProtocol::SerializeRequest(Request, RequestBytes) || !PipeClient->SendBytes(RequestBytes))
    {
        PipeClient->Disconnect();
        bStartupCompleted = false;
        return false;
    }

    if (!bExpectResponse)
    {
        return true;
    }

    TArray<uint8> ResponseBytes;
    if (!PipeClient->ReceiveBytes(ResponseBytes))
    {
        PipeClient->Disconnect();
        bStartupCompleted = false;
        return false;
    }

    if (OutResponse)
    {
        return OffgridAIASRProtocol::DeserializeResponse(ResponseBytes, *OutResponse);
    }

    FOffgridAIASRResponse IgnoredResponse;
    return OffgridAIASRProtocol::DeserializeResponse(ResponseBytes, IgnoredResponse);
}

FString FOffgridAIASRNamedPipeService::BuildLaunchArguments() const
{
    if (!Settings)
    {
        return FString();
    }

    FString Joined;
    for (const FString& Arg : Settings->LaunchArguments)
    {
        if (!Joined.IsEmpty())
        {
            Joined += TEXT(" ");
        }
        Joined += Arg;
    }
    return Joined;
}

FString FOffgridAIASRNamedPipeService::GetConfiguredSampleFormat() const
{
    // BoomOperator currently converts capture data to 16-bit PCM before the ASR service boundary.
    return TEXT("pcm16");
}
