#include "ASR/OffgridAIASRProtocol.h"

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

    FString BytesToBase64(const TArray<uint8>& Bytes)
    {
        return FBase64::Encode(Bytes);
    }

    bool Base64ToBytes(const FString& Encoded, TArray<uint8>& OutBytes)
    {
        OutBytes.Reset();
        if (Encoded.IsEmpty())
        {
            return true;
        }
        return FBase64::Decode(Encoded, OutBytes);
    }
}

namespace OffgridAIASRProtocol
{
    FString OpToString(EOffgridAIASROp Op)
    {
        switch (Op)
        {
        case EOffgridAIASROp::Startup: return TEXT("startup");
        case EOffgridAIASROp::StartUtterance: return TEXT("start_utterance");
        case EOffgridAIASROp::PushAudio: return TEXT("push_audio");
        case EOffgridAIASROp::FinalizeUtterance: return TEXT("finalize_utterance");
        case EOffgridAIASROp::CancelUtterance: return TEXT("cancel_utterance");
        case EOffgridAIASROp::GetPartial: return TEXT("get_partial");
        case EOffgridAIASROp::Health: return TEXT("health");
        case EOffgridAIASROp::Shutdown: return TEXT("shutdown");
        default: return TEXT("unknown");
        }
    }

    bool StringToOp(const FString& Value, EOffgridAIASROp& OutOp)
    {
        const FString Lower = Value.ToLower();
        if (Lower == TEXT("startup")) { OutOp = EOffgridAIASROp::Startup; return true; }
        if (Lower == TEXT("start_utterance")) { OutOp = EOffgridAIASROp::StartUtterance; return true; }
        if (Lower == TEXT("push_audio")) { OutOp = EOffgridAIASROp::PushAudio; return true; }
        if (Lower == TEXT("finalize_utterance")) { OutOp = EOffgridAIASROp::FinalizeUtterance; return true; }
        if (Lower == TEXT("cancel_utterance")) { OutOp = EOffgridAIASROp::CancelUtterance; return true; }
        if (Lower == TEXT("get_partial")) { OutOp = EOffgridAIASROp::GetPartial; return true; }
        if (Lower == TEXT("health")) { OutOp = EOffgridAIASROp::Health; return true; }
        if (Lower == TEXT("shutdown")) { OutOp = EOffgridAIASROp::Shutdown; return true; }
        return false;
    }

    bool SerializeRequest(const FOffgridAIASRRequest& Request, TArray<uint8>& OutBytes)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("op"), OpToString(Request.Op));
        Root->SetStringField(TEXT("request_id"), Request.RequestId);
        Root->SetStringField(TEXT("payload_base64"), BytesToBase64(Request.Payload));
        Root->SetNumberField(TEXT("sample_rate_hz"), Request.SampleRateHz);
        Root->SetNumberField(TEXT("num_channels"), Request.NumChannels);
        Root->SetStringField(TEXT("sample_format"), Request.SampleFormat);
        Root->SetStringField(TEXT("active_model_directory"), Request.ActiveModelDirectory);
        Root->SetStringField(TEXT("provider"), Request.Provider);
        Root->SetStringField(TEXT("decoding_method"), Request.DecodingMethod);
        Root->SetNumberField(TEXT("num_threads"), Request.NumThreads);
        Root->SetNumberField(TEXT("max_active_paths"), Request.MaxActivePaths);
        Root->SetNumberField(TEXT("expected_sample_rate"), Request.ExpectedSampleRate);
        Root->SetNumberField(TEXT("feature_dim"), Request.FeatureDim);
        Root->SetBoolField(TEXT("model_debug"), Request.bModelDebug);
        Root->SetNumberField(TEXT("finalize_silence_padding_ms"), Request.FinalizeSilencePaddingMs);
        Root->SetNumberField(TEXT("finalize_settle_delay_ms"), Request.FinalizeSettleDelayMs);

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

    bool DeserializeRequest(const TArray<uint8>& Bytes, FOffgridAIASRRequest& OutRequest)
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

        Root->TryGetStringField(TEXT("request_id"), OutRequest.RequestId);

        FString PayloadBase64;
        if (Root->TryGetStringField(TEXT("payload_base64"), PayloadBase64))
        {
            if (!Base64ToBytes(PayloadBase64, OutRequest.Payload))
            {
                return false;
            }
        }
        else
        {
            OutRequest.Payload.Reset();
        }

        Root->TryGetNumberField(TEXT("sample_rate_hz"), OutRequest.SampleRateHz);
        Root->TryGetNumberField(TEXT("num_channels"), OutRequest.NumChannels);
        Root->TryGetStringField(TEXT("sample_format"), OutRequest.SampleFormat);
        Root->TryGetStringField(TEXT("active_model_directory"), OutRequest.ActiveModelDirectory);
        Root->TryGetStringField(TEXT("provider"), OutRequest.Provider);
        Root->TryGetStringField(TEXT("decoding_method"), OutRequest.DecodingMethod);
        Root->TryGetNumberField(TEXT("num_threads"), OutRequest.NumThreads);
        Root->TryGetNumberField(TEXT("max_active_paths"), OutRequest.MaxActivePaths);
        Root->TryGetNumberField(TEXT("expected_sample_rate"), OutRequest.ExpectedSampleRate);
        Root->TryGetNumberField(TEXT("feature_dim"), OutRequest.FeatureDim);
        Root->TryGetBoolField(TEXT("model_debug"), OutRequest.bModelDebug);
        Root->TryGetNumberField(TEXT("finalize_silence_padding_ms"), OutRequest.FinalizeSilencePaddingMs);
        Root->TryGetNumberField(TEXT("finalize_settle_delay_ms"), OutRequest.FinalizeSettleDelayMs);
        return true;
    }

    bool SerializeResponse(const FOffgridAIASRResponse& Response, TArray<uint8>& OutBytes)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetBoolField(TEXT("ok"), Response.bOk);
        Root->SetStringField(TEXT("request_id"), Response.RequestId);
        Root->SetStringField(TEXT("transcript"), Response.Transcript);
        Root->SetStringField(TEXT("error_message"), Response.ErrorMessage);

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

    bool DeserializeResponse(const TArray<uint8>& Bytes, FOffgridAIASRResponse& OutResponse)
    {
        TSharedPtr<FJsonObject> Root;
        const FString Json = BytesToUtf8String(Bytes);
        auto Reader = TJsonReaderFactory<TCHAR>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return false;
        }

        Root->TryGetBoolField(TEXT("ok"), OutResponse.bOk);
        Root->TryGetStringField(TEXT("request_id"), OutResponse.RequestId);
        Root->TryGetStringField(TEXT("transcript"), OutResponse.Transcript);
        Root->TryGetStringField(TEXT("error_message"), OutResponse.ErrorMessage);
        return true;
    }
}
