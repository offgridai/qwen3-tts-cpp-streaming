#include "OffgridAIBoomOperator.h"
#include "OffgridAI.h"

#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Core/OffgridAIOrchestrator.h"
#include "Data/OffgridAIBoomOperatorAudioSettingsDataAsset.h"

namespace
{
    constexpr int32 DefaultPreferredSampleRate = 48000;
    constexpr int32 DefaultPreferredNumChannels = 1;
    constexpr int32 DefaultFramesPerBuffer = 480;
    constexpr float DefaultInputGain = 1.0f;
    constexpr bool DefaultEnableHighPass = true;
    constexpr float DefaultHighPassCutoffHz = 90.0f;

    // Poll interval after capture stops while waiting for queued game-thread
    // callbacks to finish forwarding audio to the orchestrator.
    constexpr float CaptureDrainPollSeconds = 0.01f;
}

void UOffgridAIBoomOperator::BeginPlay()
{
    Super::BeginPlay();

    if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
    {
        Orchestrator->RegisterBoomOperator(this);
    }
}

void UOffgridAIBoomOperator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(CaptureDrainTimerHandle);
    }

    bPendingFinalizeAfterDrain = false;
    bFinalizeInProgress = false;
    PendingCaptureCallbacks.Store(0);

    if (bIsCapturing)
    {
        StopAudioCapture();

        if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
        {
            Orchestrator->NotifyBoomOperatorFinalizeRequested(PlayerID);
        Orchestrator->EndPlayerAudioInput(PlayerID);
        }
    }

    if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
    {
        Orchestrator->UnregisterBoomOperator(PlayerID, this);
    }

    Super::EndPlay(EndPlayReason);
}

UOffgridAIOrchestrator* UOffgridAIBoomOperator::GetOrchestrator() const
{
    if (const UWorld* World = GetWorld())
    {
        if (UGameInstance* GI = World->GetGameInstance())
        {
            return GI->GetSubsystem<UOffgridAIOrchestrator>();
        }
    }

    return nullptr;
}

FOffgridAIResolvedAudioCaptureSettings UOffgridAIBoomOperator::ResolveAudioCaptureSettings() const
{
    FOffgridAIResolvedAudioCaptureSettings Result;
    Result.SampleRate = BoomAudioSettingsAsset ? BoomAudioSettingsAsset->PreferredCaptureSampleRate : DefaultPreferredSampleRate;
    Result.NumChannels = BoomAudioSettingsAsset ? BoomAudioSettingsAsset->PreferredCaptureNumChannels : DefaultPreferredNumChannels;
    Result.FramesPerBuffer = BoomAudioSettingsAsset ? BoomAudioSettingsAsset->FramesPerBuffer : DefaultFramesPerBuffer;
    Result.InputGain = BoomAudioSettingsAsset ? BoomAudioSettingsAsset->InputGain : DefaultInputGain;
    Result.bEnableHighPassFilter = BoomAudioSettingsAsset ? BoomAudioSettingsAsset->bEnableHighPassFilter : DefaultEnableHighPass;
    Result.HighPassCutoffHz = BoomAudioSettingsAsset ? BoomAudioSettingsAsset->HighPassCutoffHz : DefaultHighPassCutoffHz;
    Result.StopTailCaptureSeconds = BoomAudioSettingsAsset ? BoomAudioSettingsAsset->StopTailCaptureSeconds : 0.07f;
    Result.FinalizeSilencePaddingSeconds = BoomAudioSettingsAsset ? BoomAudioSettingsAsset->FinalizeSilencePaddingSeconds : 0.04f;

    if (Result.bEnableHighPassFilter && Result.SampleRate > 0)
    {
        const float CutoffHz = FMath::Max(Result.HighPassCutoffHz, 1.0f);
        const float Dt = 1.0f / static_cast<float>(Result.SampleRate);
        const float RC = 1.0f / (2.0f * PI * CutoffHz);
        Result.HighPassAlpha = RC / (RC + Dt);
    }

    return Result;
}

void UOffgridAIBoomOperator::PlayerPTTInputStart()
{
    UE_LOG(LogOffgridAI, Warning,
        TEXT("BoomOperator::PlayerPTTInputStart this=%s addr=%p PlayerID=%s"),
        *GetPathName(),
        this,
        *PlayerID.ToString());

    if (bIsCapturing)
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(CaptureDrainTimerHandle);
    }

    bPendingFinalizeAfterDrain = false;
    bFinalizeInProgress = false;
    PendingCaptureCallbacks.Store(0);

    if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
    {
        if (!Orchestrator->BeginPlayerAudioInput(PlayerID))
        {
            return;
        }

        if (!StartAudioCapture())
        {
            Orchestrator->NotifyBoomOperatorFinalizeRequested(PlayerID);
        Orchestrator->EndPlayerAudioInput(PlayerID);
            return;
        }
    }
}

void UOffgridAIBoomOperator::PlayerPTTInputEnd()
{
    UE_LOG(LogOffgridAI, Warning,
        TEXT("BoomOperator::PlayerPTTInputEnd entered. bIsCapturing=%s bPendingFinalizeAfterDrain=%s bFinalizeInProgress=%s PlayerID=%s"),
        bIsCapturing ? TEXT("true") : TEXT("false"),
        bPendingFinalizeAfterDrain ? TEXT("true") : TEXT("false"),
        bFinalizeInProgress ? TEXT("true") : TEXT("false"),
        *PlayerID.ToString());

    if (!bIsCapturing)
    {
        UE_LOG(LogOffgridAI, Warning,
            TEXT("BoomOperator::PlayerPTTInputEnd early out because bIsCapturing is false."));
        return;
    }

    if (bPendingFinalizeAfterDrain || bFinalizeInProgress)
    {
        UE_LOG(LogOffgridAI, Warning,
            TEXT("BoomOperator::PlayerPTTInputEnd ignored because finalize is already pending/in progress."));
        return;
    }

    if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
    {
        Orchestrator->NoteTurnLatencyEvent(TEXT("PTTReleased"), FString::Printf(TEXT("player=%s"), *PlayerID.ToString()));
    }

    bPendingFinalizeAfterDrain = true;

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(CaptureDrainTimerHandle);

        UE_LOG(LogOffgridAI, Warning,
            TEXT("BoomOperator::PlayerPTTInputEnd scheduling delayed stop/finalize."));

        World->GetTimerManager().SetTimer(
            CaptureDrainTimerHandle,
            this,
            &UOffgridAIBoomOperator::HandleCaptureDrainTimerFired,
            FMath::Max(0.0f, ActiveCaptureSettings.StopTailCaptureSeconds),
            false);
    }
    else
    {
        // Fallback: if no world/timer manager is available, stop and finalize now.
        StopAudioCapture();
        FinalizeCaptureIfDrained();
    }
}

bool UOffgridAIBoomOperator::StartAudioCapture()
{
    if (bIsCapturing)
    {
        return true;
    }

    AudioCapture = MakeUnique<Audio::FAudioCapture>();
    if (!AudioCapture.IsValid())
    {
        return false;
    }

    struct FCandidateFormat
    {
        int32 SampleRate;
        int32 NumChannels;
    };

    ActiveCaptureSettings = ResolveAudioCaptureSettings();

    const TArray<FCandidateFormat> CandidateFormats =
    {
        { ActiveCaptureSettings.SampleRate, ActiveCaptureSettings.NumChannels },
        { 48000, 1 },
        { 48000, 2 },
        { 44100, 1 },
        { 44100, 2 },
        { 16000, 1 }
    };

    ResetInputDSPState();
    bHasLoggedCaptureFormat = false;

    TWeakObjectPtr<UOffgridAIBoomOperator> WeakThis(this);

    for (const FCandidateFormat& Candidate : CandidateFormats)
    {
        Audio::FAudioCaptureDeviceParams DeviceParams;
        DeviceParams.DeviceIndex = INDEX_NONE;
        DeviceParams.NumInputChannels = Candidate.NumChannels;
        DeviceParams.SampleRate = Candidate.SampleRate;

        const bool bOpened = AudioCapture->OpenAudioCaptureStream(
            DeviceParams,
            [WeakThis](const void* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
            {
                if (!WeakThis.IsValid() || !AudioData || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
                {
                    return;
                }

                const float* FloatSamples = static_cast<const float*>(AudioData);

                TArray<uint8> PCMChunk;
                WeakThis->ProcessCapturedAudioToPCM16(FloatSamples, NumFrames, NumChannels, SampleRate, PCMChunk);

                if (PCMChunk.Num() == 0)
                {
                    return;
                }

                ++WeakThis->PendingCaptureCallbacks;

                AsyncTask(ENamedThreads::GameThread, [WeakThis, PCMChunk = MoveTemp(PCMChunk), SampleRate, bOverflow]() mutable
                    {
                        if (WeakThis.IsValid())
                        {
                            if (bOverflow)
                            {
                                UE_LOG(LogOffgridAI, Verbose, TEXT("Audio capture overflow detected"));
                            }

                            WeakThis->OnAudioChunkCaptured(PCMChunk, SampleRate, 1);
                            --WeakThis->PendingCaptureCallbacks;

                            if (WeakThis->bPendingFinalizeAfterDrain && !WeakThis->bIsCapturing)
                            {
                                WeakThis->FinalizeCaptureIfDrained();
                            }
                        }
                    });
            },
            static_cast<uint32>(ActiveCaptureSettings.FramesPerBuffer));

        if (!bOpened)
        {
            continue;
        }

        if (!AudioCapture->StartStream())
        {
            AudioCapture->CloseStream();
            continue;
        }

        ActiveCaptureSettings.SampleRate = Candidate.SampleRate;
        ActiveCaptureSettings.NumChannels = Candidate.NumChannels;
        if (ActiveCaptureSettings.bEnableHighPassFilter && ActiveCaptureSettings.SampleRate > 0)
        {
            const float CutoffHz = FMath::Max(ActiveCaptureSettings.HighPassCutoffHz, 1.0f);
            const float Dt = 1.0f / static_cast<float>(ActiveCaptureSettings.SampleRate);
            const float RC = 1.0f / (2.0f * PI * CutoffHz);
            ActiveCaptureSettings.HighPassAlpha = RC / (RC + Dt);
        }

        bIsCapturing = true;
        return true;
    }

    AudioCapture.Reset();
    return false;
}

void UOffgridAIBoomOperator::StopAudioCapture()
{
    if (!bIsCapturing)
    {
        return;
    }

    if (AudioCapture.IsValid())
    {
        AudioCapture->StopStream();
        AudioCapture->CloseStream();
        AudioCapture.Reset();
    }

    bIsCapturing = false;

    if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
    {
        Orchestrator->NoteTurnLatencyEvent(TEXT("CaptureStopped"), FString::Printf(TEXT("player=%s"), *PlayerID.ToString()));
    }
}

void UOffgridAIBoomOperator::OnAudioChunkCaptured(const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels)
{
    if (PCMChunk.Num() == 0)
    {
        return;
    }

    if (UOffgridAIOrchestrator* Orchestrator = GetOrchestrator())
    {
        Orchestrator->SubmitPlayerAudioChunk(PlayerID, PCMChunk, SampleRate, NumChannels);
    }
}

void UOffgridAIBoomOperator::ResetInputDSPState()
{
    PreviousMonoInputSample = 0.0f;
    PreviousMonoOutputSample = 0.0f;
}

void UOffgridAIBoomOperator::ProcessCapturedAudioToPCM16(
    const float* FloatSamples,
    int32 NumFrames,
    int32 NumChannels,
    int32 SampleRate,
    TArray<uint8>& OutPCMChunk)
{
    OutPCMChunk.Empty();

    if (!FloatSamples || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
    {
        return;
    }

    if (!bHasLoggedCaptureFormat)
    {
        UE_LOG(LogOffgridAI, Log, TEXT("Capture callback format SampleRate=%d Channels=%d NumFrames=%d"),
            SampleRate,
            NumChannels,
            NumFrames);

        bHasLoggedCaptureFormat = true;
    }

    OutPCMChunk.SetNumUninitialized(NumFrames * static_cast<int32>(sizeof(int16)));
    int16* PCM16Data = reinterpret_cast<int16*>(OutPCMChunk.GetData());

    for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
    {
        float MixedSample = 0.0f;

        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 SampleIndex = FrameIndex * NumChannels + ChannelIndex;
            MixedSample += FloatSamples[SampleIndex];
        }

        MixedSample /= static_cast<float>(NumChannels);
        MixedSample *= ActiveCaptureSettings.InputGain;

        if (ActiveCaptureSettings.bEnableHighPassFilter)
        {
            const float FilteredSample = ActiveCaptureSettings.HighPassAlpha *
                (PreviousMonoOutputSample + MixedSample - PreviousMonoInputSample);

            PreviousMonoInputSample = MixedSample;
            PreviousMonoOutputSample = FilteredSample;
            MixedSample = FilteredSample;
        }

        const float ClampedSample = FMath::Clamp(MixedSample, -1.0f, 1.0f);
        PCM16Data[FrameIndex] = static_cast<int16>(FMath::RoundToInt(ClampedSample * 32767.0f));
    }
}

void UOffgridAIBoomOperator::SubmitFinalizeSilencePadding()
{
    if (ActiveCaptureSettings.FinalizeSilencePaddingSeconds <= 0.0f)
    {
        return;
    }

    UOffgridAIOrchestrator* Orchestrator = GetOrchestrator();
    if (!Orchestrator || ActiveCaptureSettings.SampleRate <= 0)
    {
        return;
    }

    const int32 SilenceSamples = FMath::Max(1, FMath::RoundToInt(ActiveCaptureSettings.FinalizeSilencePaddingSeconds * static_cast<float>(ActiveCaptureSettings.SampleRate)));
    TArray<uint8> SilencePCM;
    SilencePCM.AddZeroed(SilenceSamples * static_cast<int32>(sizeof(int16)));

    UE_LOG(LogOffgridAI, Verbose,
        TEXT("BoomOperator::SubmitFinalizeSilencePadding samples=%d sampleRate=%d"),
        SilenceSamples,
        ActiveCaptureSettings.SampleRate);

    Orchestrator->NoteTurnLatencyEvent(
        TEXT("FinalizePaddingSubmitted"),
        FString::Printf(TEXT("samples=%d sample_rate=%d"), SilenceSamples, ActiveCaptureSettings.SampleRate));
    Orchestrator->SubmitPlayerAudioChunk(PlayerID, SilencePCM, ActiveCaptureSettings.SampleRate, 1);
}

void UOffgridAIBoomOperator::HandleCaptureDrainTimerFired()
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(CaptureDrainTimerHandle);
    }

    UE_LOG(LogOffgridAI, Warning,
        TEXT("BoomOperator::HandleCaptureDrainTimerFired stopping capture. PendingCallbacks=%d bPendingFinalizeAfterDrain=%s bFinalizeInProgress=%s"),
        PendingCaptureCallbacks.Load(),
        bPendingFinalizeAfterDrain ? TEXT("true") : TEXT("false"),
        bFinalizeInProgress ? TEXT("true") : TEXT("false"));

    if (!bPendingFinalizeAfterDrain || bFinalizeInProgress)
    {
        return;
    }

    StopAudioCapture();
    FinalizeCaptureIfDrained();
}

void UOffgridAIBoomOperator::FinalizeCaptureIfDrained()
{
    if (!bPendingFinalizeAfterDrain)
    {
        return;
    }

    if (bIsCapturing)
    {
        return;
    }

    const int32 PendingCallbacks = PendingCaptureCallbacks.Load();
    if (PendingCallbacks > 0)
    {
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().SetTimer(
                CaptureDrainTimerHandle,
                this,
                &UOffgridAIBoomOperator::HandleCaptureDrainTimerFired,
                CaptureDrainPollSeconds,
                false);
        }

        UE_LOG(LogOffgridAI, Verbose,
            TEXT("BoomOperator::FinalizeCaptureIfDrained waiting for pending callbacks: %d"),
            PendingCallbacks);
        return;
    }

    bPendingFinalizeAfterDrain = false;

    SubmitFinalizeSilencePadding();

    UOffgridAIOrchestrator* Orchestrator = GetOrchestrator();
    UE_LOG(LogOffgridAI, Warning,
        TEXT("BoomOperator::FinalizeCaptureIfDrained GetOrchestrator() = %s"),
        Orchestrator ? TEXT("valid") : TEXT("null"));

    if (Orchestrator)
    {
        UE_LOG(LogOffgridAI, Warning,
            TEXT("BoomOperator::FinalizeCaptureIfDrained calling Orchestrator->EndPlayerAudioInput PlayerID=%s"),
            *PlayerID.ToString());

        Orchestrator->NotifyBoomOperatorFinalizeRequested(PlayerID);
        Orchestrator->EndPlayerAudioInput(PlayerID);

        UE_LOG(LogOffgridAI, Warning,
            TEXT("BoomOperator::FinalizeCaptureIfDrained returned from Orchestrator->EndPlayerAudioInput"));
    }
}