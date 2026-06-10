#include "LLM/OffgridAILLMPipeProtocol.h"

#include "Dom/JsonObject.h"
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

    void SetStringArrayField(TSharedRef<FJsonObject> Root, const TCHAR* FieldName, const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> ArrayValues;
        for (const FString& Value : Values)
        {
            ArrayValues.Add(MakeShared<FJsonValueString>(Value));
        }
        Root->SetArrayField(FieldName, ArrayValues);
    }

    void GetStringArrayField(const TSharedPtr<FJsonObject>& Root, const TCHAR* FieldName, TArray<FString>& OutValues)
    {
        OutValues.Reset();
        const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
        if (!Root->TryGetArrayField(FieldName, ArrayPtr) || !ArrayPtr)
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& Entry : *ArrayPtr)
        {
            FString StringValue;
            if (Entry.IsValid() && Entry->TryGetString(StringValue))
            {
                OutValues.Add(StringValue);
            }
        }
    }
}

namespace OffgridAILLMProtocol
{
    FString OpToString(EOffgridAILLMOp Op)
    {
        switch (Op)
        {
        case EOffgridAILLMOp::Startup: return TEXT("startup");
        case EOffgridAILLMOp::InitializeSession: return TEXT("initialize_session");
        case EOffgridAILLMOp::Generate: return TEXT("generate");
        case EOffgridAILLMOp::Cancel: return TEXT("cancel");
        case EOffgridAILLMOp::ClearSession: return TEXT("clear_session");
        case EOffgridAILLMOp::Health: return TEXT("health");
        case EOffgridAILLMOp::Shutdown: return TEXT("shutdown");
        default: return TEXT("unknown");
        }
    }

    bool StringToOp(const FString& Value, EOffgridAILLMOp& OutOp)
    {
        const FString Lower = Value.ToLower();
        if (Lower == TEXT("startup")) { OutOp = EOffgridAILLMOp::Startup; return true; }
        if (Lower == TEXT("initialize_session")) { OutOp = EOffgridAILLMOp::InitializeSession; return true; }
        if (Lower == TEXT("generate")) { OutOp = EOffgridAILLMOp::Generate; return true; }
        if (Lower == TEXT("cancel")) { OutOp = EOffgridAILLMOp::Cancel; return true; }
        if (Lower == TEXT("clear_session")) { OutOp = EOffgridAILLMOp::ClearSession; return true; }
        if (Lower == TEXT("health")) { OutOp = EOffgridAILLMOp::Health; return true; }
        if (Lower == TEXT("shutdown")) { OutOp = EOffgridAILLMOp::Shutdown; return true; }
        return false;
    }

    FString RequestKindToString(EOffgridAILLMRequestKind Kind)
    {
        switch (Kind)
        {
        case EOffgridAILLMRequestKind::EmotionImpactClassifier: return TEXT("emotion_impact_classifier");
        case EOffgridAILLMRequestKind::Dialogue:
        default: return TEXT("dialogue");
        }
    }

    EOffgridAILLMRequestKind StringToRequestKind(const FString& Value)
    {
        return Value.Equals(TEXT("emotion_impact_classifier"), ESearchCase::IgnoreCase)
            ? EOffgridAILLMRequestKind::EmotionImpactClassifier
            : EOffgridAILLMRequestKind::Dialogue;
    }

    bool SerializeRequest(const FOffgridAILLMRequest& Request, TArray<uint8>& OutBytes)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("op"), OpToString(Request.Op));
        Root->SetStringField(TEXT("request_kind"), RequestKindToString(Request.RequestKind));
        Root->SetStringField(TEXT("request_id"), Request.RequestId);
        Root->SetStringField(TEXT("conversation_id"), Request.ConversationId);
        SetStringArrayField(Root, TEXT("npc_ids"), Request.NPCIds);
        Root->SetStringField(TEXT("player_text"), Request.PlayerText);
        Root->SetStringField(TEXT("system_prompt"), Request.SystemPrompt);
        Root->SetStringField(TEXT("emotion_prompt"), Request.EmotionPrompt);
        Root->SetStringField(TEXT("response_schema_json"), Request.ResponseSchemaJson);
        Root->SetStringField(TEXT("example_json"), Request.ExampleJson);
        Root->SetStringField(TEXT("canonical_transcript"), Request.CanonicalTranscript);
        Root->SetStringField(TEXT("persistent_emotion_state"), Request.PersistentEmotionState);
        Root->SetStringField(TEXT("dialogue_output_contract"), Request.DialogueOutputContract);
        SetStringArrayField(Root, TEXT("allowed_emotion_labels"), Request.AllowedEmotionLabels);
        Root->SetBoolField(TEXT("use_dialogue_gbnf"), Request.bUseDialogueGBNF);
        Root->SetStringField(TEXT("dialogue_gbnf"), Request.DialogueGBNF);
        Root->SetBoolField(TEXT("use_emotion_gbnf"), Request.bUseEmotionGBNF);
        Root->SetStringField(TEXT("emotion_gbnf"), Request.EmotionGBNF);
        Root->SetNumberField(TEXT("max_dialogue_tokens"), Request.MaxDialogueTokens);
        Root->SetNumberField(TEXT("emotion_context_turn_count"), Request.EmotionContextTurnCount);
        Root->SetNumberField(TEXT("max_emotion_context_characters"), Request.MaxEmotionContextCharacters);
        Root->SetNumberField(TEXT("emotion_state_step"), Request.EmotionStateStep);
        Root->SetStringField(TEXT("mode"), Request.Mode == EOffgridAILLMMode::LlamaCpp ? TEXT("llama_cpp") : TEXT("passthrough"));
        Root->SetStringField(TEXT("model_path"), Request.ModelPath);
        Root->SetNumberField(TEXT("context_window_tokens"), Request.ContextWindowTokens);
        Root->SetNumberField(TEXT("max_output_tokens"), Request.MaxOutputTokens);
        Root->SetNumberField(TEXT("temperature"), Request.Temperature);
        Root->SetNumberField(TEXT("max_dialogue_characters"), Request.MaxDialogueCharacters);
        Root->SetNumberField(TEXT("gpu_layers"), Request.GPULayers);
        Root->SetNumberField(TEXT("parallel_slots"), Request.ParallelSlots);
        Root->SetBoolField(TEXT("verbose_backend_logging"), Request.bVerboseBackendLogging);

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

    bool DeserializeRequest(const TArray<uint8>& Bytes, FOffgridAILLMRequest& OutRequest)
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

        FString RequestKindString;
        if (Root->TryGetStringField(TEXT("request_kind"), RequestKindString))
        {
            OutRequest.RequestKind = StringToRequestKind(RequestKindString);
        }

        Root->TryGetStringField(TEXT("request_id"), OutRequest.RequestId);
        Root->TryGetStringField(TEXT("conversation_id"), OutRequest.ConversationId);
        GetStringArrayField(Root, TEXT("npc_ids"), OutRequest.NPCIds);
        Root->TryGetStringField(TEXT("player_text"), OutRequest.PlayerText);
        Root->TryGetStringField(TEXT("system_prompt"), OutRequest.SystemPrompt);
        Root->TryGetStringField(TEXT("emotion_prompt"), OutRequest.EmotionPrompt);
        Root->TryGetStringField(TEXT("response_schema_json"), OutRequest.ResponseSchemaJson);
        Root->TryGetStringField(TEXT("example_json"), OutRequest.ExampleJson);
        Root->TryGetStringField(TEXT("canonical_transcript"), OutRequest.CanonicalTranscript);
        Root->TryGetStringField(TEXT("persistent_emotion_state"), OutRequest.PersistentEmotionState);
        Root->TryGetStringField(TEXT("dialogue_output_contract"), OutRequest.DialogueOutputContract);
        if (OutRequest.DialogueOutputContract.IsEmpty())
        {
            OutRequest.DialogueOutputContract = TEXT("NPC|spoken line");
        }
        GetStringArrayField(Root, TEXT("allowed_emotion_labels"), OutRequest.AllowedEmotionLabels);
        Root->TryGetBoolField(TEXT("use_dialogue_gbnf"), OutRequest.bUseDialogueGBNF);
        Root->TryGetStringField(TEXT("dialogue_gbnf"), OutRequest.DialogueGBNF);
        Root->TryGetBoolField(TEXT("use_emotion_gbnf"), OutRequest.bUseEmotionGBNF);
        Root->TryGetStringField(TEXT("emotion_gbnf"), OutRequest.EmotionGBNF);
        Root->TryGetNumberField(TEXT("max_dialogue_tokens"), OutRequest.MaxDialogueTokens);
        Root->TryGetNumberField(TEXT("emotion_context_turn_count"), OutRequest.EmotionContextTurnCount);
        Root->TryGetNumberField(TEXT("max_emotion_context_characters"), OutRequest.MaxEmotionContextCharacters);
        double EmotionStateStep = OutRequest.EmotionStateStep;
        if (Root->TryGetNumberField(TEXT("emotion_state_step"), EmotionStateStep))
        {
            OutRequest.EmotionStateStep = static_cast<float>(EmotionStateStep);
        }

        FString ModeString;
        if (Root->TryGetStringField(TEXT("mode"), ModeString) && ModeString.Equals(TEXT("llama_cpp"), ESearchCase::IgnoreCase))
        {
            OutRequest.Mode = EOffgridAILLMMode::LlamaCpp;
        }
        else
        {
            OutRequest.Mode = EOffgridAILLMMode::Passthrough;
        }

        Root->TryGetStringField(TEXT("model_path"), OutRequest.ModelPath);
        Root->TryGetNumberField(TEXT("context_window_tokens"), OutRequest.ContextWindowTokens);
        Root->TryGetNumberField(TEXT("max_output_tokens"), OutRequest.MaxOutputTokens);
        Root->TryGetNumberField(TEXT("temperature"), OutRequest.Temperature);
        Root->TryGetNumberField(TEXT("max_dialogue_characters"), OutRequest.MaxDialogueCharacters);
        Root->TryGetNumberField(TEXT("gpu_layers"), OutRequest.GPULayers);
        Root->TryGetNumberField(TEXT("parallel_slots"), OutRequest.ParallelSlots);
        Root->TryGetBoolField(TEXT("verbose_backend_logging"), OutRequest.bVerboseBackendLogging);
        return true;
    }

    bool SerializeResponse(const FOffgridAILLMResponse& Response, TArray<uint8>& OutBytes)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetBoolField(TEXT("ok"), Response.bOk);
        Root->SetStringField(TEXT("request_id"), Response.RequestId);
        Root->SetStringField(TEXT("json_payload"), Response.JSONPayload);
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

    bool DeserializeResponse(const TArray<uint8>& Bytes, FOffgridAILLMResponse& OutResponse)
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
        Root->TryGetStringField(TEXT("json_payload"), OutResponse.JSONPayload);
        Root->TryGetStringField(TEXT("error_message"), OutResponse.ErrorMessage);
        return true;
    }
}
