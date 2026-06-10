#pragma once

#include "CoreMinimal.h"
#include "Core/OffgridAITypes.h"
#include "TTS/OffgridAITTSService.h"

enum class EOffgridAITTSOp : uint8
{
    Startup,
    BeginSynthesis,
    Cancel,
    PollEvent,
    Health,
    Shutdown
};

struct FOffgridAITTSRequest
{
    EOffgridAITTSOp Op = EOffgridAITTSOp::Health;
    FString RequestID;
    FString ConversationID;
    FString NPCID;
    FString LineID;
    FString VoiceID;
    FString VoiceMode = TEXT("custom_voice");
    bool bVoiceDesign = false;
    FString Instruction;
    FString VoiceDesignInstruction;
    FString Dialogue;
    FString Emotion;

    FString ModelIdentifier;
    FString ModelDirectory;
    FString SpeakerEmbeddingPath;
    FString ReferenceAudioPath;
    FString ReferenceText;
    FString Language = TEXT("English");
    FString ServiceWorkingDirectory;

    bool bUseGPU = true;
    bool bPrewarmStreaming = true;
    bool bForwardEmotionToInstruction = false;
    bool bAsyncStreamingDecode = true;
    bool bDumpFirstFrameProfile = false;
    int32 FirstTailWindowFrames = 3;
    int32 SteadyTailWindowFrames = 8;
    int32 ContextFrames = 4;
    int32 FinalContextFrames = 4;
    int32 PrewarmFrames = 1;
    int32 PrewarmRepeats = 1;
    bool bPrewarmFirstDecode = true;
    bool bPrewarmSteadyDecode = true;
    bool bPrewarmFinalDecode = true;
    int32 MaxAudioTokens = 4096;
    int32 TopK = 75;
    float TopP = 1.0f;
    float Temperature = 0.9f;
    float RepetitionPenalty = 1.05f;

    int32 OutputSampleRate = 24000;
    int32 NumChannels = 1;
    FString SampleFormat = TEXT("pcm16");
    int32 PreferredChunkMs = 40;
    bool bVerboseLogging = false;
};

struct FOffgridAITTSResponse
{
    bool bOk = false;
    FString RequestID;
    FString ErrorMessage;
    bool bHasEvent = false;
    FOffgridAITTSStreamEvent Event;
};

namespace OffgridAITTSProtocol
{
    OFFGRIDAI_API FString OpToString(EOffgridAITTSOp Op);
    OFFGRIDAI_API bool StringToOp(const FString& Value, EOffgridAITTSOp& OutOp);
    OFFGRIDAI_API bool SerializeRequest(const FOffgridAITTSRequest& Request, TArray<uint8>& OutBytes);
    OFFGRIDAI_API bool DeserializeRequest(const TArray<uint8>& Bytes, FOffgridAITTSRequest& OutRequest);
    OFFGRIDAI_API bool SerializeResponse(const FOffgridAITTSResponse& Response, TArray<uint8>& OutBytes);
    OFFGRIDAI_API bool DeserializeResponse(const TArray<uint8>& Bytes, FOffgridAITTSResponse& OutResponse);
}
