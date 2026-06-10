#pragma once

#include "CoreMinimal.h"

struct FOffgridAIStreamingSpeechIsland
{
    int32 IslandIndex = INDEX_NONE;
    float AudioBufferStartSec = 0.0f;
    float AudioBufferLastSpeechSec = 0.0f;
    float AudioBufferEndSec = 0.0f;
    bool bStarted = false;
    bool bEnded = false;
};

// Lightweight 10 ms acoustic feature diagnostics.
// Runtime speech-start detection uses RMS/hysteresis only; these extra features
// are emitted for offline analysis, not for viseme rescue or arbitration.
struct FOffgridAIStreamingAudioFeatureFrame
{
    float AudioBufferCenterSec = 0.0f;
    float AudioBufferStartSec = 0.0f;
    float AudioBufferEndSec = 0.0f;

    float RMS = 0.0f;
    float RMSNorm = 0.0f;
    float DeltaRMS = 0.0f;
    float Flux = 0.0f;
    float ZCR = 0.0f;
    bool bLocalRMSPeak = false;
    bool bLocalRMSValley = false;
    bool bLocalFluxPeak = false;
};

// Streaming, RMS/hysteresis speech-island detector.
// Contract: converts arriving synthesized PCM into speech islands in AudioBufferSec seconds.
// It does not know about text, visemes, playback, or FaceDriver.
class OFFGRIDAI_API FOffgridAIStreamingSpeechDetector
{
public:
    void Reset();
    void AppendPCM16(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample = -1);
    void Finalize(float FinalObservedAudioBufferEndSec = -1.0f);

    const TArray<FOffgridAIStreamingSpeechIsland>& GetIslands() const { return Islands; }
    const TArray<FOffgridAIStreamingAudioFeatureFrame>& GetFeatureFrames() const { return FeatureFrames; }
    bool HasObservedFirstSpeechStart() const { return bHasObservedFirstSpeechStart; }
    float GetFirstSpeechAudioBufferStartSec() const { return FirstSpeechAudioBufferStartSec; }
    float GetObservedAudioBufferEndSec() const { return ObservedAudioBufferEndSec; }

private:
    void ProcessAnalysisFrame(float FrameStartSeconds, float FrameEndSeconds, float RMS);
    void RefreshLocalFeatureFlags();

    TArray<FOffgridAIStreamingSpeechIsland> Islands;
    TArray<FOffgridAIStreamingAudioFeatureFrame> FeatureFrames;
    bool bInSpeech = false;
    bool bSpeechCandidateActive = false;
    float SpeechCandidateStartSeconds = 0.0f;
    float SpeechCandidateAccumSeconds = 0.0f;
    float SilenceAccumSeconds = 0.0f;
    float SilenceStartSeconds = 0.0f;
    float ActiveIslandPeakRMS = 0.0001f;
    float ActiveIslandSpeechSeconds = 0.0f;
    float ActiveLowEnergyAccumSeconds = 0.0f;
    float ActiveLowEnergyStartSeconds = 0.0f;
    bool bHasObservedFirstSpeechStart = false;
    float FirstSpeechAudioBufferStartSec = 0.0f;
    float ObservedAudioBufferEndSec = 0.0f;

    TArray<float> PendingMonoSamples;
    int64 PendingSampleBase = 0;
    int32 ActiveSampleRate = 0;

    // Alpha.01 detector state: speech level and noise floor are tracked separately.
    // SpeechPeakRMS is only for feature normalization / voice-level context;
    // NoiseFloorRMS owns restart/open thresholds so loud previous phrases do not
    // inflate the next onset gate.
    float SpeechPeakRMS = 0.0001f;
    float NoiseFloorRMS = 0.0001f;
};
