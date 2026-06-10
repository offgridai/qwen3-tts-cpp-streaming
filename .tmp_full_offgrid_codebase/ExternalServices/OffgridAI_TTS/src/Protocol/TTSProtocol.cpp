#include "Protocol/TTSProtocol.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace
{
    static const char* Base64Table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    bool FindKeyPosition(const std::string& Json, const std::string& Key, size_t& OutPos)
    {
        const std::string Needle = "\"" + Key + "\"";
        OutPos = Json.find(Needle);
        return OutPos != std::string::npos;
    }

    bool ExtractJsonStringAt(const std::string& Json, size_t QuotePos, std::string& OutValue, size_t* OutEndPos = nullptr)
    {
        if (QuotePos >= Json.size() || Json[QuotePos] != '"') return false;
        std::string Raw;
        bool Escaped = false;
        for (size_t i = QuotePos + 1; i < Json.size(); ++i)
        {
            const char c = Json[i];
            if (Escaped) { Raw.push_back(c); Escaped = false; continue; }
            if (c == '\\') { Raw.push_back(c); Escaped = true; continue; }
            if (c == '"')
            {
                OutValue = TTSProtocol::UnescapeJson(Raw);
                if (OutEndPos) *OutEndPos = i + 1;
                return true;
            }
            Raw.push_back(c);
        }
        return false;
    }

    bool ExtractJsonStringField(const std::string& Json, const std::string& Key, std::string& OutValue)
    {
        size_t KeyPos = 0;
        if (!FindKeyPosition(Json, Key, KeyPos)) return false;
        const size_t Colon = Json.find(':', KeyPos);
        if (Colon == std::string::npos) return false;
        const size_t Quote = Json.find('"', Colon);
        if (Quote == std::string::npos) return false;
        return ExtractJsonStringAt(Json, Quote, OutValue);
    }

    bool ExtractJsonBoolField(const std::string& Json, const std::string& Key, bool& OutValue)
    {
        size_t KeyPos = 0;
        if (!FindKeyPosition(Json, Key, KeyPos)) return false;
        const size_t Colon = Json.find(':', KeyPos);
        if (Colon == std::string::npos) return false;
        size_t Pos = Colon + 1;
        while (Pos < Json.size() && std::isspace(static_cast<unsigned char>(Json[Pos]))) ++Pos;
        if (Json.compare(Pos, 4, "true") == 0) { OutValue = true; return true; }
        if (Json.compare(Pos, 5, "false") == 0) { OutValue = false; return true; }
        return false;
    }

    bool ExtractJsonNumberToken(const std::string& Json, const std::string& Key, std::string& OutValue)
    {
        size_t KeyPos = 0;
        if (!FindKeyPosition(Json, Key, KeyPos)) return false;
        const size_t Colon = Json.find(':', KeyPos);
        if (Colon == std::string::npos) return false;
        size_t Pos = Colon + 1;
        while (Pos < Json.size() && std::isspace(static_cast<unsigned char>(Json[Pos]))) ++Pos;
        const size_t Start = Pos;
        while (Pos < Json.size())
        {
            const char C = Json[Pos];
            if (!(std::isdigit(static_cast<unsigned char>(C)) || C=='-' || C=='+' || C=='.' || C=='e' || C=='E')) break;
            ++Pos;
        }
        if (Pos == Start) return false;
        OutValue = Json.substr(Start, Pos - Start);
        return true;
    }

    bool ExtractJsonIntField(const std::string& Json, const std::string& Key, int32_t& OutValue)
    {
        std::string Token; if (!ExtractJsonNumberToken(Json, Key, Token)) return false; OutValue = static_cast<int32_t>(std::strtol(Token.c_str(), nullptr, 10)); return true;
    }

    bool ExtractJsonFloatField(const std::string& Json, const std::string& Key, float& OutValue)
    {
        std::string Token; if (!ExtractJsonNumberToken(Json, Key, Token)) return false; OutValue = static_cast<float>(std::strtod(Token.c_str(), nullptr)); return true;
    }
}

namespace TTSProtocol
{
    std::string OpToString(ETTSOp Op)
    {
        switch (Op)
        {
        case ETTSOp::Startup: return "startup";
        case ETTSOp::BeginSynthesis: return "begin_synthesis";
        case ETTSOp::Cancel: return "cancel";
        case ETTSOp::PollEvent: return "poll_event";
        case ETTSOp::Health: return "health";
        case ETTSOp::Shutdown: return "shutdown";
        default: return "unknown";
        }
    }

    ETTSOp StringToOp(const std::string& Value)
    {
        if (Value == "startup") return ETTSOp::Startup;
        if (Value == "begin_synthesis") return ETTSOp::BeginSynthesis;
        if (Value == "cancel") return ETTSOp::Cancel;
        if (Value == "poll_event") return ETTSOp::PollEvent;
        if (Value == "health") return ETTSOp::Health;
        if (Value == "shutdown") return ETTSOp::Shutdown;
        return ETTSOp::Unknown;
    }

    std::string EventTypeToString(ETTSEventType Type)
    {
        switch (Type)
        {
        case ETTSEventType::StreamStarted: return "stream_started";
        case ETTSEventType::AudioChunk: return "audio_chunk";
        case ETTSEventType::Completed: return "completed";
        case ETTSEventType::Error: return "error";
        default: return "stream_started";
        }
    }

    ETTSEventType StringToEventType(const std::string& Value)
    {
        if (Value == "stream_started") return ETTSEventType::StreamStarted;
        if (Value == "audio_chunk") return ETTSEventType::AudioChunk;
        if (Value == "completed") return ETTSEventType::Completed;
        if (Value == "error") return ETTSEventType::Error;
        return ETTSEventType::StreamStarted;
    }

    bool DeserializeRequest(const std::vector<uint8_t>& Bytes, TTSRequest& OutRequest)
    {
        const std::string Json(Bytes.begin(), Bytes.end());
        std::string Op;
        if (!ExtractJsonStringField(Json, "op", Op)) return false;
        OutRequest.Op = StringToOp(Op);
        ExtractJsonStringField(Json, "request_id", OutRequest.RequestId);
        ExtractJsonStringField(Json, "conversation_id", OutRequest.ConversationId);
        ExtractJsonStringField(Json, "npc_id", OutRequest.NPCId);
        ExtractJsonStringField(Json, "line_id", OutRequest.LineId);
        ExtractJsonStringField(Json, "voice_id", OutRequest.VoiceId);
        ExtractJsonStringField(Json, "dialogue", OutRequest.Dialogue);
        ExtractJsonStringField(Json, "emotion", OutRequest.Emotion);
        ExtractJsonStringField(Json, "voice_mode", OutRequest.VoiceMode);
        ExtractJsonStringField(Json, "instruction", OutRequest.Instruction);
        ExtractJsonStringField(Json, "voice_design_instruction", OutRequest.VoiceDesignInstruction);
        ExtractJsonBoolField(Json, "voice_design", OutRequest.bVoiceDesign);
        ExtractJsonStringField(Json, "model_identifier", OutRequest.ModelIdentifier);
        ExtractJsonStringField(Json, "model_directory", OutRequest.ModelDirectory);
        ExtractJsonStringField(Json, "speaker_embedding_path", OutRequest.SpeakerEmbeddingPath);
        ExtractJsonStringField(Json, "reference_audio_path", OutRequest.ReferenceAudioPath);
        ExtractJsonStringField(Json, "reference_text", OutRequest.ReferenceText);
        ExtractJsonStringField(Json, "language", OutRequest.Language);
        ExtractJsonStringField(Json, "service_working_directory", OutRequest.ServiceWorkingDirectory);
        ExtractJsonBoolField(Json, "use_gpu", OutRequest.bUseGPU);
        ExtractJsonBoolField(Json, "prewarm_streaming", OutRequest.bPrewarmStreaming);
        ExtractJsonBoolField(Json, "forward_emotion_to_instruction", OutRequest.bForwardEmotionToInstruction);
        ExtractJsonBoolField(Json, "async_streaming_decode", OutRequest.bAsyncStreamingDecode);
        ExtractJsonBoolField(Json, "dump_first_frame_profile", OutRequest.bDumpFirstFrameProfile);
        ExtractJsonIntField(Json, "first_tail_window_frames", OutRequest.FirstTailWindowFrames);
        ExtractJsonIntField(Json, "steady_tail_window_frames", OutRequest.SteadyTailWindowFrames);
        ExtractJsonIntField(Json, "context_frames", OutRequest.ContextFrames);
        ExtractJsonIntField(Json, "final_context_frames", OutRequest.FinalContextFrames);
        ExtractJsonIntField(Json, "prewarm_frames", OutRequest.PrewarmFrames);
        ExtractJsonIntField(Json, "prewarm_repeats", OutRequest.PrewarmRepeats);
        ExtractJsonBoolField(Json, "prewarm_first_decode", OutRequest.bPrewarmFirstDecode);
        ExtractJsonBoolField(Json, "prewarm_steady_decode", OutRequest.bPrewarmSteadyDecode);
        ExtractJsonBoolField(Json, "prewarm_final_decode", OutRequest.bPrewarmFinalDecode);
        ExtractJsonIntField(Json, "max_audio_tokens", OutRequest.MaxAudioTokens);
        ExtractJsonIntField(Json, "top_k", OutRequest.TopK);
        ExtractJsonFloatField(Json, "top_p", OutRequest.TopP);
        ExtractJsonFloatField(Json, "temperature", OutRequest.Temperature);
        ExtractJsonFloatField(Json, "repetition_penalty", OutRequest.RepetitionPenalty);
        ExtractJsonIntField(Json, "output_sample_rate", OutRequest.OutputSampleRate);
        ExtractJsonIntField(Json, "num_channels", OutRequest.NumChannels);
        ExtractJsonStringField(Json, "sample_format", OutRequest.SampleFormat);
        ExtractJsonIntField(Json, "preferred_chunk_ms", OutRequest.PreferredChunkMs);
        ExtractJsonBoolField(Json, "verbose_logging", OutRequest.bVerboseLogging);
        return true;
    }

    bool SerializeResponse(const TTSResponse& Response, std::vector<uint8_t>& OutBytes)
    {
        std::ostringstream Json;
        Json << '{'
             << "\"ok\":" << (Response.Ok ? "true" : "false") << ','
             << "\"request_id\":\"" << EscapeJson(Response.RequestId) << "\"," 
             << "\"error_message\":\"" << EscapeJson(Response.ErrorMessage) << "\"," 
             << "\"has_event\":" << (Response.HasEvent ? "true" : "false");

        if (Response.HasEvent)
        {
            Json << ','
                 << "\"event\":{"
                 << "\"request_id\":\"" << EscapeJson(Response.Event.RequestId) << "\"," 
                 << "\"conversation_id\":\"" << EscapeJson(Response.Event.ConversationId) << "\"," 
                 << "\"npc_id\":\"" << EscapeJson(Response.Event.NPCId) << "\"," 
                 << "\"line_id\":\"" << EscapeJson(Response.Event.LineId) << "\"," 
                 << "\"type\":\"" << EscapeJson(EventTypeToString(Response.Event.Type)) << "\"," 
                 << "\"sample_rate\":" << Response.Event.SampleRate << ','
                 << "\"num_channels\":" << Response.Event.NumChannels << ','
                 << "\"chunk_start_sample\":" << Response.Event.ChunkStartSample << ','
                 << "\"chunk_sample_count\":" << Response.Event.ChunkSampleCount << ','
                 << "\"stream_total_samples\":" << Response.Event.StreamTotalSamples << ','
                 << "\"pcm_chunk_b64\":\"" << Base64Encode(Response.Event.PCMChunk) << "\"," 
                 << "\"error_message\":\"" << EscapeJson(Response.Event.ErrorMessage) << "\"}";
        }

        Json << '}';
        const std::string Serialized = Json.str();
        OutBytes.assign(Serialized.begin(), Serialized.end());
        return true;
    }

    std::string EscapeJson(const std::string& Value)
    {
        std::ostringstream Out;
        for (const char c : Value)
        {
            switch (c)
            {
            case '\\': Out << "\\\\"; break;
            case '"': Out << "\\\""; break;
            case '\n': Out << "\\n"; break;
            case '\r': Out << "\\r"; break;
            case '\t': Out << "\\t"; break;
            default: Out << c; break;
            }
        }
        return Out.str();
    }

    std::string UnescapeJson(const std::string& Value)
    {
        std::ostringstream Out; bool Escaped = false;
        for (const char c : Value)
        {
            if (!Escaped) { if (c == '\\') { Escaped = true; } else { Out << c; } continue; }
            switch (c)
            {
            case '\\': Out << '\\'; break;
            case '"': Out << '"'; break;
            case 'n': Out << '\n'; break;
            case 'r': Out << '\r'; break;
            case 't': Out << '\t'; break;
            default: Out << c; break;
            }
            Escaped = false;
        }
        return Out.str();
    }

    std::string Base64Encode(const std::vector<uint8_t>& Value)
    {
        std::string Out;
        size_t i = 0;
        for (; i + 2 < Value.size(); i += 3)
        {
            const uint32_t Triple = (static_cast<uint32_t>(Value[i]) << 16) |
                                    (static_cast<uint32_t>(Value[i + 1]) << 8) |
                                    (static_cast<uint32_t>(Value[i + 2]));
            Out.push_back(Base64Table[(Triple >> 18) & 0x3F]);
            Out.push_back(Base64Table[(Triple >> 12) & 0x3F]);
            Out.push_back(Base64Table[(Triple >> 6) & 0x3F]);
            Out.push_back(Base64Table[Triple & 0x3F]);
        }
        if (i < Value.size())
        {
            uint32_t Triple = static_cast<uint32_t>(Value[i]) << 16;
            if (i + 1 < Value.size()) Triple |= static_cast<uint32_t>(Value[i + 1]) << 8;
            Out.push_back(Base64Table[(Triple >> 18) & 0x3F]);
            Out.push_back(Base64Table[(Triple >> 12) & 0x3F]);
            Out.push_back((i + 1 < Value.size()) ? Base64Table[(Triple >> 6) & 0x3F] : '=');
            Out.push_back('=');
        }
        return Out;
    }

    bool Base64Decode(const std::string& Value, std::vector<uint8_t>& OutBytes)
    {
        auto DecodeChar = [](char C) -> int
        {
            if (C >= 'A' && C <= 'Z') return C - 'A';
            if (C >= 'a' && C <= 'z') return C - 'a' + 26;
            if (C >= '0' && C <= '9') return C - '0' + 52;
            if (C == '+') return 62;
            if (C == '/') return 63;
            if (C == '=') return -2;
            return -1;
        };

        OutBytes.clear();
        int Val = 0;
        int ValBits = -8;
        for (const char C : Value)
        {
            if (std::isspace(static_cast<unsigned char>(C))) continue;
            const int Decoded = DecodeChar(C);
            if (Decoded == -2) break;
            if (Decoded < 0) return false;
            Val = (Val << 6) + Decoded;
            ValBits += 6;
            if (ValBits >= 0)
            {
                OutBytes.push_back(static_cast<uint8_t>((Val >> ValBits) & 0xFF));
                ValBits -= 8;
            }
        }
        return true;
    }

}
