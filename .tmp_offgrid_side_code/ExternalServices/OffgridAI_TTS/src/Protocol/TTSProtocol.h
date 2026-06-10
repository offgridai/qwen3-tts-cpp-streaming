#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ETTSOp : uint8_t
{
    Startup,
    BeginSynthesis,
    Cancel,
    PollEvent,
    Health,
    Shutdown,
    Unknown
};

enum class ETTSEventType : uint8_t
{
    StreamStarted,
    AudioChunk,
    Completed,
    Error
};

struct TTSRequest
{
    ETTSOp Op = ETTSOp::Unknown;
    std::string RequestId;
    std::string ConversationId;
    std::string NPCId;
    std::string LineId;
    std::string VoiceId;
    std::string Dialogue;
    std::string Emotion;

    // Clean service v1: explicit voice routing.
    // Supported values: "base", "custom_voice", "voice_design".
    std::string VoiceMode = "base";
    std::string Instruction;
    std::string VoiceDesignInstruction;
    bool bVoiceDesign = false;

    std::string ModelIdentifier;
    std::string ModelDirectory;
    std::string SpeakerEmbeddingPath;
    std::string ReferenceAudioPath;
    std::string ReferenceText;
    std::string Language = "English";
    std::string ServiceWorkingDirectory;

    bool bUseGPU = true;
    bool bPrewarmStreaming = true;
    bool bForwardEmotionToInstruction = false;
    bool bAsyncStreamingDecode = true;
    bool bDumpFirstFrameProfile = false;
    int32_t FirstTailWindowFrames = 3;
    int32_t SteadyTailWindowFrames = 12;
    int32_t ContextFrames = 4;
    int32_t FinalContextFrames = 4;
    int32_t PrewarmFrames = 1;
    int32_t PrewarmRepeats = 1;
    bool bPrewarmFirstDecode = true;
    bool bPrewarmSteadyDecode = true;
    bool bPrewarmFinalDecode = true;
    int32_t MaxAudioTokens = 4096;
    int32_t TopK = 75;
    float TopP = 1.0f;
    float Temperature = 0.9f;
    float RepetitionPenalty = 1.05f;

    int32_t OutputSampleRate = 24000;
    int32_t NumChannels = 1;
    std::string SampleFormat = "pcm16";
    int32_t PreferredChunkMs = 40;
    bool bVerboseLogging = false;
};

struct TTSEvent
{
    std::string RequestId;
    std::string ConversationId;
    std::string NPCId;
    std::string LineId;
    ETTSEventType Type = ETTSEventType::StreamStarted;
    int32_t SampleRate = 0;
    int32_t NumChannels = 0;

    // Phase 1A speech timeline metadata. AudioChunk events carry the exact
    // sample range of the chunk inside the synthesized utterance. StreamStarted
    // and Completed events carry the total synthesized samples known at that
    // point when available. Values are mono/sample-frame counts, not bytes.
    int64_t ChunkStartSample = 0;
    int32_t ChunkSampleCount = 0;
    int64_t StreamTotalSamples = 0;

    std::vector<uint8_t> PCMChunk;
    std::string ErrorMessage;
};

struct TTSResponse
{
    bool Ok = false;
    std::string RequestId;
    std::string ErrorMessage;
    bool HasEvent = false;
    TTSEvent Event;
};

namespace TTSProtocol
{
    std::string OpToString(ETTSOp Op);
    ETTSOp StringToOp(const std::string& Value);
    std::string EventTypeToString(ETTSEventType Type);
    ETTSEventType StringToEventType(const std::string& Value);

    bool DeserializeRequest(const std::vector<uint8_t>& Bytes, TTSRequest& OutRequest);
    bool SerializeResponse(const TTSResponse& Response, std::vector<uint8_t>& OutBytes);

    std::string EscapeJson(const std::string& Value);
    std::string UnescapeJson(const std::string& Value);
    std::string Base64Encode(const std::vector<uint8_t>& Value);
    bool Base64Decode(const std::string& Value, std::vector<uint8_t>& OutBytes);
}
