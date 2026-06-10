#include "Lipsync/OffgridAIStreamingSpeechDetector.h"
#include "OffgridAI.h"

void FOffgridAIStreamingSpeechDetector::Reset()
{
    Islands.Reset();
    FeatureFrames.Reset();
    bInSpeech = false;
    bSpeechCandidateActive = false;
    SpeechCandidateStartSeconds = 0.0f;
    SpeechCandidateAccumSeconds = 0.0f;
    SilenceAccumSeconds = 0.0f;
    SilenceStartSeconds = 0.0f;
    ActiveIslandPeakRMS = 0.0001f;
    ActiveIslandSpeechSeconds = 0.0f;
    ActiveLowEnergyAccumSeconds = 0.0f;
    ActiveLowEnergyStartSeconds = 0.0f;
    bHasObservedFirstSpeechStart = false;
    FirstSpeechAudioBufferStartSec = 0.0f;
    ObservedAudioBufferEndSec = 0.0f;
    PendingMonoSamples.Reset();
    PendingSampleBase = 0;
    ActiveSampleRate = 0;
    SpeechPeakRMS = 0.0001f;
    NoiseFloorRMS = 0.0001f;
}

void FOffgridAIStreamingSpeechDetector::AppendPCM16(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample)
{
    if (SampleRate <= 0 || NumChannels <= 0 || BytesToUse <= 0)
    {
        return;
    }

    const int32 BytesPerFrame = NumChannels * static_cast<int32>(sizeof(int16));
    const int32 FrameCount = BytesToUse / BytesPerFrame;
    if (FrameCount <= 0)
    {
        return;
    }

    if (ActiveSampleRate != SampleRate)
    {
        PendingMonoSamples.Reset();
        PendingSampleBase = FMath::Max<int64>(ChunkStartSample, 0);
        ActiveSampleRate = SampleRate;
    }
    else if (ChunkStartSample >= 0)
    {
        const int64 ExpectedNext = PendingSampleBase + PendingMonoSamples.Num();
        if (ChunkStartSample != ExpectedNext)
        {
            PendingMonoSamples.Reset();
            PendingSampleBase = ChunkStartSample;
        }
    }

    const int16* Samples = reinterpret_cast<const int16*>(PCMChunk.GetData());
    PendingMonoSamples.Reserve(PendingMonoSamples.Num() + FrameCount);
    for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
    {
        float Mono = 0.0f;
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            Mono += static_cast<float>(Samples[FrameIndex * NumChannels + ChannelIndex]) / 32768.0f;
        }
        PendingMonoSamples.Add(Mono / static_cast<float>(NumChannels));
    }

    const int32 AnalysisFrameSamples = FMath::Max(1, FMath::RoundToInt(static_cast<float>(SampleRate) * 0.010f));
    while (PendingMonoSamples.Num() >= AnalysisFrameSamples)
    {
        double SumSquares = 0.0;
        int32 ZeroCrossings = 0;
        double AbsDeltaSum = 0.0;
        float PrevSample = PendingMonoSamples[0];
        for (int32 I = 0; I < AnalysisFrameSamples; ++I)
        {
            const float S = PendingMonoSamples[I];
            SumSquares += static_cast<double>(S) * static_cast<double>(S);
            if (I > 0)
            {
                if ((S >= 0.0f) != (PrevSample >= 0.0f))
                {
                    ++ZeroCrossings;
                }
                AbsDeltaSum += FMath::Abs(S - PrevSample);
                PrevSample = S;
            }
        }
        const float RMS = FMath::Sqrt(static_cast<float>(SumSquares / static_cast<double>(AnalysisFrameSamples)));

        // Alpha.01: keep speech loudness and ambient floor separate.  The old
        // detector used a single PeakRMS for opening thresholds; after a loud
        // phrase that made non-initial restarts artificially hard to detect.
        if (bInSpeech)
        {
            SpeechPeakRMS = FMath::Max(SpeechPeakRMS * 0.992f, RMS);
            // Freeze NoiseFloorRMS during speech; TTS background is effectively
            // silent and speech samples should not raise the floor.
        }
        else
        {
            SpeechPeakRMS = FMath::Max(0.0001f, SpeechPeakRMS * 0.970f);

            // L02: while an opening candidate is accumulating sustain, do not
            // teach the noise floor from those same frames.  Line-start speech
            // was raising OpenThreshold under its own feet, causing valid
            // onsets to be repeatedly cancelled until much later in the WAV.
            if (!bSpeechCandidateActive)
            {
                NoiseFloorRMS = FMath::Clamp(NoiseFloorRMS * 0.980f + RMS * 0.020f, 0.000001f, 0.050f);
            }
        }

        const float FrameStart = static_cast<float>(static_cast<double>(PendingSampleBase) / static_cast<double>(SampleRate));
        const float FrameEnd = static_cast<float>(static_cast<double>(PendingSampleBase + AnalysisFrameSamples) / static_cast<double>(SampleRate));

        FOffgridAIStreamingAudioFeatureFrame& Feature = FeatureFrames.AddDefaulted_GetRef();
        Feature.AudioBufferStartSec = FrameStart;
        Feature.AudioBufferEndSec = FrameEnd;
        Feature.AudioBufferCenterSec = 0.5f * (FrameStart + FrameEnd);
        Feature.RMS = RMS;
        Feature.RMSNorm = FMath::Clamp(RMS / FMath::Max(SpeechPeakRMS, 0.0001f), 0.0f, 1.0f);
        Feature.DeltaRMS = FeatureFrames.Num() > 1 ? RMS - FeatureFrames[FeatureFrames.Num() - 2].RMS : 0.0f;
        Feature.Flux = FMath::Clamp(static_cast<float>(AbsDeltaSum / static_cast<double>(FMath::Max(AnalysisFrameSamples - 1, 1))) * 18.0f, 0.0f, 1.0f);
        Feature.ZCR = static_cast<float>(ZeroCrossings) / static_cast<float>(FMath::Max(AnalysisFrameSamples - 1, 1));
        RefreshLocalFeatureFlags();

        ProcessAnalysisFrame(FrameStart, FrameEnd, RMS);

        PendingMonoSamples.RemoveAt(0, AnalysisFrameSamples, EAllowShrinking::No);
        PendingSampleBase += AnalysisFrameSamples;
    }

    ObservedAudioBufferEndSec = FMath::Max(ObservedAudioBufferEndSec, static_cast<float>(static_cast<double>((ChunkStartSample >= 0 ? ChunkStartSample : PendingSampleBase) + FrameCount) / static_cast<double>(SampleRate)));
}


void FOffgridAIStreamingSpeechDetector::Finalize(float FinalObservedAudioBufferEndSec)
{
    if (FinalObservedAudioBufferEndSec >= 0.0f)
    {
        ObservedAudioBufferEndSec = FMath::Max(ObservedAudioBufferEndSec, FinalObservedAudioBufferEndSec);
    }

    if (bInSpeech && Islands.Num() > 0)
    {
        FOffgridAIStreamingSpeechIsland& Active = Islands.Last();
        Active.bEnded = true;
        Active.AudioBufferEndSec = FMath::Max(Active.AudioBufferEndSec, FMath::Max(Active.AudioBufferLastSpeechSec, ObservedAudioBufferEndSec));
    }

    bInSpeech = false;
    bSpeechCandidateActive = false;
    SpeechCandidateAccumSeconds = 0.0f;
    SilenceAccumSeconds = 0.0f;
    SilenceStartSeconds = 0.0f;
}

void FOffgridAIStreamingSpeechDetector::RefreshLocalFeatureFlags()
{
    const int32 LastIndex = FeatureFrames.Num() - 1;
    if (LastIndex < 2)
    {
        return;
    }

    // Re-evaluate a tiny trailing window for diagnostics only. Runtime island
    // detection does not consume these local feature flags.
    for (int32 I = FMath::Max(1, LastIndex - 3); I <= LastIndex - 1; ++I)
    {
        FOffgridAIStreamingAudioFeatureFrame& Curr = FeatureFrames[I];
        const FOffgridAIStreamingAudioFeatureFrame& Prev = FeatureFrames[I - 1];
        const FOffgridAIStreamingAudioFeatureFrame& Next = FeatureFrames[I + 1];
        Curr.bLocalRMSPeak = Curr.RMSNorm >= Prev.RMSNorm + 0.045f && Curr.RMSNorm >= Next.RMSNorm + 0.045f;
        Curr.bLocalRMSValley = Curr.RMSNorm + 0.045f <= Prev.RMSNorm && Curr.RMSNorm + 0.045f <= Next.RMSNorm;
        Curr.bLocalFluxPeak = Curr.Flux >= Prev.Flux + 0.035f && Curr.Flux >= Next.Flux + 0.035f;
    }
}

void FOffgridAIStreamingSpeechDetector::ProcessAnalysisFrame(float FrameStartSeconds, float FrameEndSeconds, float RMS)
{
    // Alpha.01: threshold from ambient/noise floor, not prior speech peak.
    // This makes restart detection recover quickly after loud previous phrases.
    const float OpenThreshold = FMath::Max(0.004f, NoiseFloorRMS * 3.0f + 0.003f);
    const float CloseThreshold = FMath::Max(0.002f, NoiseFloorRMS * 1.5f + 0.002f);

    // Alpha12b UE roundtrip: first-island onset must not promote a breath,
    // mouth click, or tiny speaker pop to speech.  Non-initial restarts remain
    // responsive, but the very first launch now needs a slightly longer sustain
    // plus voiced-ish evidence from the already-computed feature frame.
    const bool bFirstOnset = !bHasObservedFirstSpeechStart && Islands.Num() <= 0;
    const float OpenSustainSeconds = bFirstOnset ? 0.055f : 0.020f;
    const float CloseSustainSeconds = 0.075f;
    // L19: punctuation gates always wait for an audio resume.  Keep the
    // detector responsive enough to emit short pause/resume islands when TTS
    // only gives a brief comma hesitation, but still close only on sustained
    // low energy rather than arbitrary intra-speech valleys.

    const FOffgridAIStreamingAudioFeatureFrame* LatestFeature = FeatureFrames.Num() > 0 ? &FeatureFrames.Last() : nullptr;
    const bool bHasVoicedEvidence = !bFirstOnset || !LatestFeature ||
        (LatestFeature->ZCR <= 0.180f && LatestFeature->Flux >= 0.010f) ||
        (LatestFeature->RMSNorm >= 0.42f && LatestFeature->ZCR <= 0.240f);

    const bool bOpenFrame = RMS >= OpenThreshold && bHasVoicedEvidence;
    const bool bKeepOpenFrame = RMS >= CloseThreshold;
    const float FrameDuration = FMath::Max(FrameEndSeconds - FrameStartSeconds, 0.0f);

    if (!bInSpeech)
    {
        if (bOpenFrame)
        {
            if (!bSpeechCandidateActive)
            {
                bSpeechCandidateActive = true;
                SpeechCandidateStartSeconds = FrameStartSeconds;
                SpeechCandidateAccumSeconds = 0.0f;
            }
            SpeechCandidateAccumSeconds += FrameDuration;
            if (SpeechCandidateAccumSeconds >= OpenSustainSeconds)
            {
                FOffgridAIStreamingSpeechIsland& Island = Islands.AddDefaulted_GetRef();
                Island.IslandIndex = Islands.Num() - 1;
                // For the first island, do not backdate to the first candidate
                // frame.  A breath/pop can begin the candidate; confirmed speech
                // should launch near the sustained voiced evidence.
                Island.AudioBufferStartSec = bFirstOnset ? FMath::Max(0.0f, FrameEndSeconds - OpenSustainSeconds) : SpeechCandidateStartSeconds;
                Island.AudioBufferLastSpeechSec = FrameEndSeconds;
                Island.AudioBufferEndSec = FrameEndSeconds;
                Island.bStarted = true;
                Island.bEnded = false;

                bInSpeech = true;
                SilenceAccumSeconds = 0.0f;
                SilenceStartSeconds = 0.0f;
                ActiveIslandPeakRMS = FMath::Max(RMS, 0.0001f);
                ActiveIslandSpeechSeconds = 0.0f;
                ActiveLowEnergyAccumSeconds = 0.0f;
                ActiveLowEnergyStartSeconds = 0.0f;
                bSpeechCandidateActive = false;
                if (!bHasObservedFirstSpeechStart)
                {
                    bHasObservedFirstSpeechStart = true;
                    FirstSpeechAudioBufferStartSec = Island.AudioBufferStartSec;
                }
            }
        }
        else
        {
            bSpeechCandidateActive = false;
            SpeechCandidateAccumSeconds = 0.0f;
        }
        return;
    }

    if (Islands.Num() <= 0)
    {
        bInSpeech = false;
        return;
    }

    FOffgridAIStreamingSpeechIsland& Active = Islands.Last();

    // L19: no scheduler-side coalescing.  Audio islands are the only resume
    // signals, so short sustained low-energy gaps should produce real islands.
    // Local RMS valleys alone still do not close an island.
    ActiveIslandSpeechSeconds += FrameDuration;
    ActiveIslandPeakRMS = FMath::Max(ActiveIslandPeakRMS * 0.998f, RMS);
    ActiveLowEnergyAccumSeconds = 0.0f;
    ActiveLowEnergyStartSeconds = 0.0f;

    if (bKeepOpenFrame)
    {
        Active.AudioBufferLastSpeechSec = FrameEndSeconds;
        Active.AudioBufferEndSec = FrameEndSeconds;
        SilenceAccumSeconds = 0.0f;
        SilenceStartSeconds = 0.0f;
    }
    else
    {
        if (SilenceAccumSeconds <= 0.0f)
        {
            SilenceStartSeconds = FrameStartSeconds;
        }
        SilenceAccumSeconds += FrameDuration;
        if (SilenceAccumSeconds >= CloseSustainSeconds)
        {
            Active.bEnded = true;
            Active.AudioBufferEndSec = FMath::Max(Active.AudioBufferLastSpeechSec, FrameEndSeconds - SilenceAccumSeconds);
            bInSpeech = false;
            bSpeechCandidateActive = false;
            SpeechCandidateAccumSeconds = 0.0f;
            SilenceAccumSeconds = 0.0f;
            SilenceStartSeconds = 0.0f;
            ActiveIslandPeakRMS = 0.0001f;
            ActiveIslandSpeechSeconds = 0.0f;
            ActiveLowEnergyAccumSeconds = 0.0f;
            ActiveLowEnergyStartSeconds = 0.0f;
        }
    }
}
