#include "TTS/OffgridAITTSPipeProtocol.h"

#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    FString BytesToUtf8String(const TArray<uint8>& Bytes)
    {
        if (Bytes.IsEmpty())
        {
            return FString();
        }

        FUTF8ToTCHAR Converter(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
        return FString(Converter.Length(), Converter.Get());
    }

    FString EventTypeToString(EOffgridAITTSStreamEventType Type)
    {
        switch (Type)
        {
        case EOffgridAITTSStreamEventType::StreamStarted: return TEXT("stream_started");
        case EOffgridAITTSStreamEventType::AudioChunk: return TEXT("audio_chunk");
        case EOffgridAITTSStreamEventType::Completed: return TEXT("completed");
        case EOffgridAITTSStreamEventType::Error: return TEXT("error");
        default: return TEXT("stream_started");
        }
    }

    EOffgridAITTSStreamEventType StringToEventType(const FString& Value)
    {
        const FString Lower = Value.ToLower();
        if (Lower == TEXT("stream_started")) return EOffgridAITTSStreamEventType::StreamStarted;
        if (Lower == TEXT("audio_chunk")) return EOffgridAITTSStreamEventType::AudioChunk;
        if (Lower == TEXT("completed")) return EOffgridAITTSStreamEventType::Completed;
        if (Lower == TEXT("error")) return EOffgridAITTSStreamEventType::Error;
        return EOffgridAITTSStreamEventType::StreamStarted;
    }
}

namespace OffgridAITTSProtocol
{
    FString OpToString(EOffgridAITTSOp Op)
    {
        switch (Op)
        {
        case EOffgridAITTSOp::Startup: return TEXT("startup");
        case EOffgridAITTSOp::BeginSynthesis: return TEXT("begin_synthesis");
        case EOffgridAITTSOp::Cancel: return TEXT("cancel");
        case EOffgridAITTSOp::PollEvent: return TEXT("poll_event");
        case EOffgridAITTSOp::Health: return TEXT("health");
        case EOffgridAITTSOp::Shutdown: return TEXT("shutdown");
        default: return TEXT("health");
        }
    }

    bool StringToOp(const FString& Value, EOffgridAITTSOp& OutOp)
    {
        const FString Lower = Value.ToLower();
        if (Lower == TEXT("startup")) { OutOp = EOffgridAITTSOp::Startup; return true; }
        if (Lower == TEXT("begin_synthesis")) { OutOp = EOffgridAITTSOp::BeginSynthesis; return true; }
        if (Lower == TEXT("cancel")) { OutOp = EOffgridAITTSOp::Cancel; return true; }
        if (Lower == TEXT("poll_event")) { OutOp = EOffgridAITTSOp::PollEvent; return true; }
        if (Lower == TEXT("health")) { OutOp = EOffgridAITTSOp::Health; return true; }
        if (Lower == TEXT("shutdown")) { OutOp = EOffgridAITTSOp::Shutdown; return true; }
        return false;
    }

    bool SerializeRequest(const FOffgridAITTSRequest& Request, TArray<uint8>& OutBytes)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("op"), OpToString(Request.Op));
        Root->SetStringField(TEXT("request_id"), Request.RequestID);
        Root->SetStringField(TEXT("conversation_id"), Request.ConversationID);
        Root->SetStringField(TEXT("npc_id"), Request.NPCID);
        Root->SetStringField(TEXT("line_id"), Request.LineID);
        Root->SetStringField(TEXT("voice_id"), Request.VoiceID);
        Root->SetStringField(TEXT("voice_mode"), Request.VoiceMode);
        Root->SetBoolField(TEXT("voice_design"), Request.bVoiceDesign);
        Root->SetStringField(TEXT("instruction"), Request.Instruction);
        Root->SetStringField(TEXT("voice_design_instruction"), Request.VoiceDesignInstruction);
        Root->SetStringField(TEXT("dialogue"), Request.Dialogue);
        Root->SetStringField(TEXT("emotion"), Request.Emotion);
        Root->SetStringField(TEXT("model_identifier"), Request.ModelIdentifier);
        Root->SetStringField(TEXT("model_directory"), Request.ModelDirectory);
        Root->SetStringField(TEXT("speaker_embedding_path"), Request.SpeakerEmbeddingPath);
        Root->SetStringField(TEXT("reference_audio_path"), Request.ReferenceAudioPath);
        Root->SetStringField(TEXT("reference_text"), Request.ReferenceText);
        Root->SetStringField(TEXT("language"), Request.Language);
        Root->SetStringField(TEXT("service_working_directory"), Request.ServiceWorkingDirectory);
        Root->SetBoolField(TEXT("use_gpu"), Request.bUseGPU);
        Root->SetBoolField(TEXT("prewarm_streaming"), Request.bPrewarmStreaming);
        Root->SetBoolField(TEXT("forward_emotion_to_instruction"), Request.bForwardEmotionToInstruction);
        Root->SetBoolField(TEXT("async_streaming_decode"), Request.bAsyncStreamingDecode);
        Root->SetBoolField(TEXT("dump_first_frame_profile"), Request.bDumpFirstFrameProfile);
        Root->SetNumberField(TEXT("first_tail_window_frames"), Request.FirstTailWindowFrames);
        Root->SetNumberField(TEXT("steady_tail_window_frames"), Request.SteadyTailWindowFrames);
        Root->SetNumberField(TEXT("context_frames"), Request.ContextFrames);
        Root->SetNumberField(TEXT("final_context_frames"), Request.FinalContextFrames);
        Root->SetNumberField(TEXT("prewarm_frames"), Request.PrewarmFrames);
        Root->SetNumberField(TEXT("prewarm_repeats"), Request.PrewarmRepeats);
        Root->SetBoolField(TEXT("prewarm_first_decode"), Request.bPrewarmFirstDecode);
        Root->SetBoolField(TEXT("prewarm_steady_decode"), Request.bPrewarmSteadyDecode);
        Root->SetBoolField(TEXT("prewarm_final_decode"), Request.bPrewarmFinalDecode);
        Root->SetNumberField(TEXT("max_audio_tokens"), Request.MaxAudioTokens);
        Root->SetNumberField(TEXT("top_k"), Request.TopK);
        Root->SetNumberField(TEXT("top_p"), Request.TopP);
        Root->SetNumberField(TEXT("temperature"), Request.Temperature);
        Root->SetNumberField(TEXT("repetition_penalty"), Request.RepetitionPenalty);
        Root->SetNumberField(TEXT("output_sample_rate"), Request.OutputSampleRate);
        Root->SetNumberField(TEXT("num_channels"), Request.NumChannels);
        Root->SetStringField(TEXT("sample_format"), Request.SampleFormat);
        Root->SetNumberField(TEXT("preferred_chunk_ms"), Request.PreferredChunkMs);
        Root->SetBoolField(TEXT("verbose_logging"), Request.bVerboseLogging);

        FString Json;
        auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
        if (!FJsonSerializer::Serialize(Root, Writer))
        {
            return false;
        }

        FTCHARToUTF8 Utf8(*Json);
        OutBytes.SetNumUninitialized(Utf8.Length());
        FMemory::Memcpy(OutBytes.GetData(), Utf8.Get(), Utf8.Length());
        return true;
    }

    bool DeserializeRequest(const TArray<uint8>& Bytes, FOffgridAITTSRequest& OutRequest)
    {
        TSharedPtr<FJsonObject> Root;
        const FString Json = BytesToUtf8String(Bytes);
        auto Reader = TJsonReaderFactory<TCHAR>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return false;
        }

        FString OpString;
        if (!Root->TryGetStringField(TEXT("op"), OpString) || !StringToOp(OpString, OutRequest.Op))
        {
            return false;
        }

        Root->TryGetStringField(TEXT("request_id"), OutRequest.RequestID);
        Root->TryGetStringField(TEXT("conversation_id"), OutRequest.ConversationID);
        Root->TryGetStringField(TEXT("npc_id"), OutRequest.NPCID);
        Root->TryGetStringField(TEXT("line_id"), OutRequest.LineID);
        Root->TryGetStringField(TEXT("voice_id"), OutRequest.VoiceID);
        Root->TryGetStringField(TEXT("voice_mode"), OutRequest.VoiceMode);
        Root->TryGetBoolField(TEXT("voice_design"), OutRequest.bVoiceDesign);
        Root->TryGetStringField(TEXT("instruction"), OutRequest.Instruction);
        Root->TryGetStringField(TEXT("voice_design_instruction"), OutRequest.VoiceDesignInstruction);
        Root->TryGetStringField(TEXT("dialogue"), OutRequest.Dialogue);
        Root->TryGetStringField(TEXT("emotion"), OutRequest.Emotion);
        Root->TryGetStringField(TEXT("model_identifier"), OutRequest.ModelIdentifier);
        Root->TryGetStringField(TEXT("model_directory"), OutRequest.ModelDirectory);
        Root->TryGetStringField(TEXT("speaker_embedding_path"), OutRequest.SpeakerEmbeddingPath);
        Root->TryGetStringField(TEXT("reference_audio_path"), OutRequest.ReferenceAudioPath);
        Root->TryGetStringField(TEXT("reference_text"), OutRequest.ReferenceText);
        Root->TryGetStringField(TEXT("language"), OutRequest.Language);
        Root->TryGetStringField(TEXT("service_working_directory"), OutRequest.ServiceWorkingDirectory);
        Root->TryGetBoolField(TEXT("use_gpu"), OutRequest.bUseGPU);
        Root->TryGetBoolField(TEXT("prewarm_streaming"), OutRequest.bPrewarmStreaming);
        Root->TryGetBoolField(TEXT("forward_emotion_to_instruction"), OutRequest.bForwardEmotionToInstruction);
        Root->TryGetBoolField(TEXT("async_streaming_decode"), OutRequest.bAsyncStreamingDecode);
        Root->TryGetBoolField(TEXT("dump_first_frame_profile"), OutRequest.bDumpFirstFrameProfile);
        Root->TryGetNumberField(TEXT("first_tail_window_frames"), OutRequest.FirstTailWindowFrames);
        Root->TryGetNumberField(TEXT("steady_tail_window_frames"), OutRequest.SteadyTailWindowFrames);
        Root->TryGetNumberField(TEXT("context_frames"), OutRequest.ContextFrames);
        Root->TryGetNumberField(TEXT("final_context_frames"), OutRequest.FinalContextFrames);
        Root->TryGetNumberField(TEXT("prewarm_frames"), OutRequest.PrewarmFrames);
        Root->TryGetNumberField(TEXT("prewarm_repeats"), OutRequest.PrewarmRepeats);
        Root->TryGetBoolField(TEXT("prewarm_first_decode"), OutRequest.bPrewarmFirstDecode);
        Root->TryGetBoolField(TEXT("prewarm_steady_decode"), OutRequest.bPrewarmSteadyDecode);
        Root->TryGetBoolField(TEXT("prewarm_final_decode"), OutRequest.bPrewarmFinalDecode);
        Root->TryGetNumberField(TEXT("max_audio_tokens"), OutRequest.MaxAudioTokens);
        Root->TryGetNumberField(TEXT("top_k"), OutRequest.TopK);
        Root->TryGetNumberField(TEXT("top_p"), OutRequest.TopP);
        Root->TryGetNumberField(TEXT("temperature"), OutRequest.Temperature);
        Root->TryGetNumberField(TEXT("repetition_penalty"), OutRequest.RepetitionPenalty);
        Root->TryGetNumberField(TEXT("output_sample_rate"), OutRequest.OutputSampleRate);
        Root->TryGetNumberField(TEXT("num_channels"), OutRequest.NumChannels);
        Root->TryGetStringField(TEXT("sample_format"), OutRequest.SampleFormat);
        Root->TryGetNumberField(TEXT("preferred_chunk_ms"), OutRequest.PreferredChunkMs);
        Root->TryGetBoolField(TEXT("verbose_logging"), OutRequest.bVerboseLogging);
        return true;
    }

    bool SerializeResponse(const FOffgridAITTSResponse& Response, TArray<uint8>& OutBytes)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetBoolField(TEXT("ok"), Response.bOk);
        Root->SetStringField(TEXT("request_id"), Response.RequestID);
        Root->SetStringField(TEXT("error_message"), Response.ErrorMessage);
        Root->SetBoolField(TEXT("has_event"), Response.bHasEvent);

        if (Response.bHasEvent)
        {
            TSharedRef<FJsonObject> EventObject = MakeShared<FJsonObject>();
            EventObject->SetStringField(TEXT("request_id"), Response.Event.RequestID);
            EventObject->SetStringField(TEXT("conversation_id"), Response.Event.ConversationID.ToString(EGuidFormats::DigitsWithHyphens));
            EventObject->SetStringField(TEXT("npc_id"), Response.Event.NPCID.ToString());
            EventObject->SetStringField(TEXT("line_id"), Response.Event.LineID.ToString());
            EventObject->SetStringField(TEXT("type"), EventTypeToString(Response.Event.Type));
            EventObject->SetNumberField(TEXT("sample_rate"), Response.Event.SampleRate);
            EventObject->SetNumberField(TEXT("num_channels"), Response.Event.NumChannels);
            EventObject->SetNumberField(TEXT("chunk_start_sample"), static_cast<double>(Response.Event.ChunkStartSample));
            EventObject->SetNumberField(TEXT("chunk_sample_count"), Response.Event.ChunkSampleCount);
            EventObject->SetNumberField(TEXT("stream_total_samples"), static_cast<double>(Response.Event.StreamTotalSamples));
            EventObject->SetStringField(TEXT("pcm_chunk_b64"), FBase64::Encode(Response.Event.PCMChunk));
            EventObject->SetStringField(TEXT("error_message"), Response.Event.ErrorMessage);
            Root->SetObjectField(TEXT("event"), EventObject);
        }

        FString Json;
        auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
        if (!FJsonSerializer::Serialize(Root, Writer))
        {
            return false;
        }

        FTCHARToUTF8 Utf8(*Json);
        OutBytes.SetNumUninitialized(Utf8.Length());
        FMemory::Memcpy(OutBytes.GetData(), Utf8.Get(), Utf8.Length());
        return true;
    }

    bool DeserializeResponse(const TArray<uint8>& Bytes, FOffgridAITTSResponse& OutResponse)
    {
        TSharedPtr<FJsonObject> Root;
        const FString Json = BytesToUtf8String(Bytes);
        auto Reader = TJsonReaderFactory<TCHAR>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return false;
        }

        Root->TryGetBoolField(TEXT("ok"), OutResponse.bOk);
        Root->TryGetStringField(TEXT("request_id"), OutResponse.RequestID);
        Root->TryGetStringField(TEXT("error_message"), OutResponse.ErrorMessage);
        Root->TryGetBoolField(TEXT("has_event"), OutResponse.bHasEvent);

        if (OutResponse.bHasEvent)
        {
            const TSharedPtr<FJsonObject>* EventObject = nullptr;
            if (Root->TryGetObjectField(TEXT("event"), EventObject) && EventObject && EventObject->IsValid())
            {
                FString ConversationIDString;
                FString NPCIDString;
                FString LineIDString;
                FString EventTypeString;
                FString PCMChunkB64;
                (*EventObject)->TryGetStringField(TEXT("request_id"), OutResponse.Event.RequestID);
                (*EventObject)->TryGetStringField(TEXT("conversation_id"), ConversationIDString);
                FGuid::Parse(ConversationIDString, OutResponse.Event.ConversationID);
                (*EventObject)->TryGetStringField(TEXT("npc_id"), NPCIDString);
                (*EventObject)->TryGetStringField(TEXT("line_id"), LineIDString);
                OutResponse.Event.NPCID = FName(*NPCIDString);
                OutResponse.Event.LineID = FName(*LineIDString);
                (*EventObject)->TryGetStringField(TEXT("type"), EventTypeString);
                OutResponse.Event.Type = StringToEventType(EventTypeString);
                (*EventObject)->TryGetNumberField(TEXT("sample_rate"), OutResponse.Event.SampleRate);
                (*EventObject)->TryGetNumberField(TEXT("num_channels"), OutResponse.Event.NumChannels);
                double ChunkStartSampleDouble = 0.0;
                double StreamTotalSamplesDouble = 0.0;
                (*EventObject)->TryGetNumberField(TEXT("chunk_start_sample"), ChunkStartSampleDouble);
                (*EventObject)->TryGetNumberField(TEXT("chunk_sample_count"), OutResponse.Event.ChunkSampleCount);
                (*EventObject)->TryGetNumberField(TEXT("stream_total_samples"), StreamTotalSamplesDouble);
                OutResponse.Event.ChunkStartSample = static_cast<int64>(ChunkStartSampleDouble);
                OutResponse.Event.StreamTotalSamples = static_cast<int64>(StreamTotalSamplesDouble);
                (*EventObject)->TryGetStringField(TEXT("pcm_chunk_b64"), PCMChunkB64);
                FBase64::Decode(PCMChunkB64, OutResponse.Event.PCMChunk);
                (*EventObject)->TryGetStringField(TEXT("error_message"), OutResponse.Event.ErrorMessage);
            }
        }

        return true;
    }
}
