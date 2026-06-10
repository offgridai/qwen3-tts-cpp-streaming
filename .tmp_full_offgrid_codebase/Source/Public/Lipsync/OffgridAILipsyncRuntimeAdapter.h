#pragma once

#include "CoreMinimal.h"
#include "OffgridAILineCoach.h"
#include "Lipsync/OffgridAITextVisemePlanner.h"
#include "Lipsync/OffgridAIStreamingSpeechDetector.h"
#include "Lipsync/OffgridAIAudioOccupancyScheduler.h"

struct FOffgridAILipsyncRuntimeUpdateInput
{
    const FOffgridAITextVisemePlan* TextPlan = nullptr;
    const TArray<FOffgridAIStreamingSpeechIsland>* SpeechIslands = nullptr;
    const TArray<FOffgridAIStreamingAudioFeatureFrame>* AudioFeatureFrames = nullptr;

    float CurrentPlaybackSec = 0.0f;
    float PrerollSec = 0.150f;
    float IslandDurationScale = 1.0f;
    bool bInputStreamClosed = false;

    FName NPCID = NAME_None;
    FName LineID = NAME_None;
    FOffgridAIAudioOccupancySchedulerState* AudioOccupancyState = nullptr;
};

struct FOffgridAIStreamTailDiagnosticRow
{
    FName LineID = NAME_None;
    int32 PCMChunkCount = 0;
    int64 PCMBytesReceived = 0;
    int64 PCMSamplesReceived = 0;
    int32 LastSampleRate = 0;
    int32 LastNumChannels = 0;
    int64 LastChunkStartSample = -1;
    int64 LastChunkEndSample = -1;
    float ObservedAudioBufferEndSec = 0.0f;
    float FirstSpeechAudioBufferStartSec = 0.0f;
    int32 SpeechIslandCount = 0;
    bool bInputStreamClosed = false;
    FName DiagnosticKind = NAME_None;
};

struct FOffgridAILipsyncRuntimeBeginInput
{
    FString DialogueText;
    FName NPCID = NAME_None;
    FName LineID = NAME_None;
    float PrerollSec = 0.150f;
    float IslandDurationScale = 1.0f;
};

// Shared session for the liplab harness and Unreal adapter. The session owns
// text planning, streaming speech detection, and the audio-occupancy scheduler.
class OFFGRIDAI_API FOffgridAILipsyncRuntimeSession
{
public:
    void Reset();
    void BeginLine(const FOffgridAILipsyncRuntimeBeginInput& Input);
    void PushAudioPCM16(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample = -1);
    void Update(float CurrentPlaybackSec);

    // Finalize is a playback-complete operation, not an input-stream-close
    // operation. Hosts may stop pushing PCM as soon as TTS synthesis finishes,
    // but must keep calling Update() until the already-buffered audio has
    // audibly drained; otherwise eligible late visemes are forced through the
    // final end-of-stream rescue path.
    void Finalize(float FinalPlaybackSec);

    const FOffgridAITextVisemePlan& GetTextPlan() const { return TextPlan; }
    const FOffgridAIStreamingSpeechDetector& GetSpeechDetector() const { return Detector; }
    const TArray<FOffgridAIStreamingSpeechIsland>& GetSpeechIslands() const { return Detector.GetIslands(); }
    const TArray<FOffgridAIStreamingAudioFeatureFrame>& GetAudioFeatureFrames() const { return Detector.GetFeatureFrames(); }
    const FOffgridAIAlignedVisemeTrack& GetCommittedTrack() const { return CommittedTrack; }
    const TArray<FOffgridAIAudioOccupancyDiagnosticRow>& GetAudioOccupancyDiagnosticRows() const { return AudioOccupancyDiagnosticRows; }
    const FOffgridAIStreamTailDiagnosticRow& GetStreamTailDiagnosticRow() const { return StreamTailDiagnosticRow; }
    FOffgridAIAlignedVisemeTrack& GetMutableCommittedTrack() { return CommittedTrack; }
    bool IsCommittedTrackBuilt() const { return bCommittedTrackBuilt; }
    bool HasPlaybackStarted() const { return bPlaybackStarted; }
    float GetPlaybackSeconds() const { return PlaybackSec; }
    float GetPrerollSeconds() const { return PrerollSec; }

private:
    void UpdatePlaybackGate(float ObservedEndSec);
    void RecordAudioOccupancyDiagnostics(float CurrentPlaybackSec, bool bFinalReplay);

    FName NPCID = NAME_None;
    FName LineID = NAME_None;
    FString DialogueText;
    float PrerollSec = 0.150f;
    float IslandDurationScale = 1.0f;
    float PlaybackSec = 0.0f;
    bool bBegun = false;
    bool bPlaybackStarted = false;
    bool bCommittedTrackBuilt = false;

    FOffgridAITextVisemePlan TextPlan;
    FOffgridAIStreamingSpeechDetector Detector;
    FOffgridAIAlignedVisemeTrack CommittedTrack;
    FOffgridAIAudioOccupancySchedulerState AudioOccupancyState;
    TArray<FOffgridAIAudioOccupancyDiagnosticRow> AudioOccupancyDiagnosticRows;
    int32 AudioOccupancyDiagnosticUpdateOrdinal = 0;
    FOffgridAIStreamTailDiagnosticRow StreamTailDiagnosticRow;

    int32 PCMChunkCount = 0;
    int64 PCMBytesReceived = 0;
    int64 PCMSamplesReceived = 0;
    int32 LastPCMChunkSampleRate = 0;
    int32 LastPCMChunkChannels = 0;
    int64 LastPCMChunkStartSample = -1;
    int64 LastPCMChunkEndSample = -1;
};

class OFFGRIDAI_API FOffgridAILipsyncRuntimeAdapter
{
public:
    static void UpdateCommittedTrack(
        const FOffgridAILipsyncRuntimeUpdateInput& Input,
        FOffgridAIAlignedVisemeTrack& InOutTrack,
        bool& bInOutTrackBuilt);
};
