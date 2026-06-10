#include "LLM/OffgridAILLMNamedPipeService.h"

#include "OffgridAI.h"
#include "Data/OffgridAIConversationPromptDataAsset.h"
#include "Data/OffgridAILLMServiceSettingsDataAsset.h"
#include "Data/OffgridAIEmotionSettingsDataAsset.h"
#include "LLM/OffgridAILLMPipeProtocol.h"
#include "LLM/Transport/OffgridAILLMPipeClient.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString QuoteArg(const FString& Value)
	{
		if (Value.Contains(TEXT(" ")) || Value.Contains(TEXT("\t")) || Value.Contains(TEXT("\"")))
		{
			FString Escaped = Value;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("\"%s\""), *Escaped);
		}
		return Value;
	}

	bool RewriteEmotionEnumRecursive(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& AllowedEmotionLabels)
	{
		if (!Object.IsValid() || AllowedEmotionLabels.IsEmpty())
		{
			return false;
		}

		bool bChanged = false;
		const TSharedPtr<FJsonObject>* EmotionObject = nullptr;
		if (Object->TryGetObjectField(TEXT("emotion"), EmotionObject) && EmotionObject && EmotionObject->IsValid())
		{
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			for (const FString& Label : AllowedEmotionLabels)
			{
				EnumValues.Add(MakeShared<FJsonValueString>(Label));
			}
			(*EmotionObject)->SetArrayField(TEXT("enum"), EnumValues);
			bChanged = true;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!Pair.Value.IsValid())
			{
				continue;
			}

			if (Pair.Value->Type == EJson::Object)
			{
				bChanged |= RewriteEmotionEnumRecursive(Pair.Value->AsObject(), AllowedEmotionLabels);
			}
			else if (Pair.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Entry : Pair.Value->AsArray())
				{
					if (Entry.IsValid() && Entry->Type == EJson::Object)
					{
						bChanged |= RewriteEmotionEnumRecursive(Entry->AsObject(), AllowedEmotionLabels);
					}
				}
			}
		}

		return bChanged;
	}

	FString RewriteResponseSchemaEmotionEnum(const FString& SchemaJson, const TArray<FString>& AllowedEmotionLabels)
	{
		if (SchemaJson.IsEmpty() || AllowedEmotionLabels.IsEmpty())
		{
			return SchemaJson;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SchemaJson);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			return SchemaJson;
		}

		if (!RewriteEmotionEnumRecursive(RootObject, AllowedEmotionLabels))
		{
			return SchemaJson;
		}

		FString RewrittenJson;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RewrittenJson);
		return FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer) ? RewrittenJson : SchemaJson;
	}

	FString ResolveFullPath(const FString& InPath)
	{
		FString Path = InPath;
		FPaths::NormalizeFilename(Path);
		return FPaths::ConvertRelativePathToFull(Path);
	}

	bool FileExists(const FString& InPath)
	{
		return !InPath.IsEmpty() && FPaths::FileExists(ResolveFullPath(InPath));
	}

	bool DirectoryExists(const FString& InPath)
	{
		return !InPath.IsEmpty() && FPaths::DirectoryExists(ResolveFullPath(InPath));
	}
	FString CanonicalEmotionLabelFromName(FName RawName)
	{
		FString Value = RawName.ToString().TrimStartAndEnd().ToLower();
		Value.ReplaceInline(TEXT(" "), TEXT(""));
		Value.ReplaceInline(TEXT("_"), TEXT(""));

		if (Value == TEXT("happy") || Value == TEXT("happiness") || Value == TEXT("joyful")) return TEXT("joy");
		if (Value == TEXT("angry") || Value == TEXT("mad") || Value == TEXT("rage")) return TEXT("anger");
		if (Value == TEXT("sad") || Value == TEXT("sorrow")) return TEXT("sadness");
		if (Value == TEXT("fearful") || Value == TEXT("afraid") || Value == TEXT("scared")) return TEXT("fear");
		if (Value == TEXT("surprised") || Value == TEXT("shock") || Value == TEXT("shocked")) return TEXT("surprise");
		if (Value == TEXT("disgusted") || Value == TEXT("grossedout")) return TEXT("disgust");
		if (Value == TEXT("none") || Value == TEXT("noop")) return TEXT("neutral");
		return Value;
	}

	void BuildAllowedEmotionLabelsFromNames(const TArray<FName>& SupportedEmotionNames, TArray<FString>& OutLabels)
	{
		OutLabels.Reset();
		for (const FName RawEmotion : SupportedEmotionNames)
		{
			const FString Label = CanonicalEmotionLabelFromName(RawEmotion);
			if (!Label.IsEmpty())
			{
				OutLabels.AddUnique(Label);
			}
		}

		if (OutLabels.IsEmpty())
		{
			OutLabels = { TEXT("neutral"), TEXT("joy"), TEXT("anger"), TEXT("sadness"), TEXT("fear"), TEXT("surprise"), TEXT("disgust") };
		}
		else if (!OutLabels.Contains(TEXT("neutral")))
		{
			OutLabels.Insert(TEXT("neutral"), 0);
		}
	}

}

FOffgridAILLMNamedPipeService::FOffgridAILLMNamedPipeService(const UOffgridAILLMServiceSettingsDataAsset* InSettings)
	: Settings(InSettings)
	, PipeClient(MakeUnique<FOffgridAILLMPipeClient>())
{
}

FOffgridAILLMNamedPipeService::~FOffgridAILLMNamedPipeService()
{
	Reset();
}

void FOffgridAILLMNamedPipeService::Reset()
{
	CompletedQueue.Empty();

	if (PipeClient && PipeClient->IsConnected())
	{
		FOffgridAILLMRequest ShutdownRequest;
		PopulateCommonRequestFields(ShutdownRequest);
		ShutdownRequest.Op = EOffgridAILLMOp::Shutdown;
		SendRequest(ShutdownRequest, nullptr, false);
		PipeClient->Disconnect();
	}

	bStartupCompleted = false;

	if (ServiceProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(ServiceProcessHandle, true);
		FPlatformProcess::CloseProc(ServiceProcessHandle);
		ServiceProcessHandle.Reset();
	}

	ServiceProcessId = 0;
}

void FOffgridAILLMNamedPipeService::Tick(double NowSeconds)
{
}

bool FOffgridAILLMNamedPipeService::ValidateStartupSettings(FString& OutError) const
{
	OutError.Reset();

	if (!Settings)
	{
		OutError = TEXT("LLM settings asset is null.");
		return false;
	}

	if (Settings->ServiceExecutablePath.IsEmpty())
	{
		OutError = TEXT("LLM ServiceExecutablePath is empty.");
		return false;
	}

	if (!FileExists(Settings->ServiceExecutablePath))
	{
		OutError = FString::Printf(TEXT("LLM ServiceExecutablePath does not exist: %s"), *Settings->ServiceExecutablePath);
		return false;
	}

	if (!Settings->ServiceWorkingDirectory.IsEmpty() && !DirectoryExists(Settings->ServiceWorkingDirectory))
	{
		OutError = FString::Printf(TEXT("LLM ServiceWorkingDirectory does not exist: %s"), *Settings->ServiceWorkingDirectory);
		return false;
	}

	if (Settings->Mode == EOffgridAILLMMode::LlamaCpp)
	{
		if (Settings->ModelPath.IsEmpty())
		{
			OutError = TEXT("LLM ModelPath is empty for LlamaCpp mode.");
			return false;
		}

		if (!FileExists(Settings->ModelPath))
		{
			OutError = FString::Printf(TEXT("LLM ModelPath does not exist: %s"), *Settings->ModelPath);
			return false;
		}
	}

	return true;
}

bool FOffgridAILLMNamedPipeService::EnsureServiceReady(FString& OutError)
{
	OutError.Reset();

	if (!ValidateStartupSettings(OutError))
	{
		return false;
	}

	if (!EnsureServiceConnected())
	{
		// Not connected yet is normally transient during startup; keep polling until supervisor timeout.
		return false;
	}

	if (!StartupServiceIfNeeded(&OutError))
	{
		// Empty OutError means transient startup/pipe race; non-empty means deterministic fatal startup failure.
		return false;
	}

	return true;
}

bool FOffgridAILLMNamedPipeService::InitializeSession(
	const FGuid& ConversationID,
	const TArray<FName>& NPCIDs,
	const UOffgridAIConversationPromptDataAsset* PromptAsset,
	const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord)
{
	if (!ConversationID.IsValid() || NPCIDs.IsEmpty() || !EnsureServiceConnected() || !StartupServiceIfNeeded())
	{
		return false;
	}

	FOffgridAILLMRequest Request;
	PopulateCommonRequestFields(Request);
	Request.Op = EOffgridAILLMOp::InitializeSession;
	Request.RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	Request.ConversationId = ConversationID.ToString(EGuidFormats::DigitsWithHyphens);
	PopulatePromptFields(Request, NPCIDs, PromptAsset, CanonicalRecord, TArray<FName>());

	if (Settings && Settings->bVerboseLogging)
	{
		UE_LOG(LogOffgridAI, Warning,
			TEXT("[LLM INIT REQUEST DUMP]\nConversationId: %s\nRequestId: %s\nNPCIds: [%s]\nModelPath: %s\nSystemPrompt:\n%s\nEmotionPrompt:\n%s\nDialogueGBNF=%s EmotionGBNF=%s\nResponseSchemaJson:\n%s\nExampleJson:\n%s"),
			*Request.ConversationId,
			*Request.RequestId,
			*FString::Join(Request.NPCIds, TEXT(", ")),
			*Request.ModelPath,
			*Request.SystemPrompt,
			*Request.EmotionPrompt,
			Request.bUseDialogueGBNF ? TEXT("true") : TEXT("false"),
			Request.bUseEmotionGBNF ? TEXT("true") : TEXT("false"),
			*Request.ResponseSchemaJson,
			*Request.ExampleJson);
	}

	FOffgridAILLMResponse Response;
	return SendRequest(Request, &Response, true) && Response.bOk;
}

bool FOffgridAILLMNamedPipeService::SubmitRequest(
	const FString& RequestID,
	const FGuid& ConversationID,
	const TArray<FName>& NPCIDs,
	const FText& PlayerText,
	EOffgridAILLMRequestKind RequestKind,
	const UOffgridAIConversationPromptDataAsset* PromptAsset,
	const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
	const FString& PersistentEmotionState,
	const TArray<FName>& SupportedEmotionNames)
{
	if (!ConversationID.IsValid() || NPCIDs.IsEmpty() || !EnsureServiceConnected() || !StartupServiceIfNeeded())
	{
		return false;
	}

	FOffgridAILLMRequest Request;
	PopulateCommonRequestFields(Request);
	Request.Op = EOffgridAILLMOp::Generate;
	Request.RequestKind = RequestKind;
	Request.RequestId = RequestID;
	Request.ConversationId = ConversationID.ToString(EGuidFormats::DigitsWithHyphens);
	Request.PlayerText = PlayerText.ToString();
	PopulatePromptFields(Request, NPCIDs, PromptAsset, CanonicalRecord, SupportedEmotionNames);
	Request.PersistentEmotionState = PersistentEmotionState;

	if (RequestKind == EOffgridAILLMRequestKind::EmotionImpactClassifier)
	{
		// This request must be a true classifier, not a normal assistant/dialogue turn.
		// The external service may choose to use either SystemPrompt+PlayerText or
		// EmotionPrompt+PlayerText, so make every prompt-bearing field task-only.
		static const TArray<FString> CanonicalLabels = {
			TEXT("joy"), TEXT("anger"), TEXT("sadness"), TEXT("fear"), TEXT("surprise"), TEXT("disgust"), TEXT("neutral")
		};

		Request.AllowedEmotionLabels = CanonicalLabels;
		Request.ResponseSchemaJson.Reset();
		Request.ExampleJson.Reset();
		Request.DialogueOutputContract.Reset();
		Request.bUseDialogueGBNF = false;
		Request.DialogueGBNF.Reset();
		Request.bUseEmotionGBNF = false;
		Request.EmotionGBNF.Reset();
		Request.Temperature = 0.0f;
		Request.MaxOutputTokens = 12;
		Request.MaxDialogueTokens = 12;

		const int32 MaxEmotionTurns = PromptAsset
			? FMath::Clamp(PromptAsset->EmotionContextTurnCount, 0, 20)
			: 8;
		Request.CanonicalTranscript = BuildRecentTurnTranscript(CanonicalRecord, MaxEmotionTurns);

		const FString BaseInstruction = PromptAsset && !PromptAsset->EmotionPrompt.IsEmpty()
			? PromptAsset->EmotionPrompt.ToString().TrimStartAndEnd()
			: FString(TEXT("Given the latest player line and recent context, choose the emotional impulse it should cause in each NPC:\njoy, anger, sadness, fear, surprise, disgust, neutral\nrespond with one word only"));

		Request.SystemPrompt = TEXT("You are an emotion impulse classifier. You do not chat, explain, roleplay, give advice, or answer the player. You only return labels from the allowed list.");
		Request.EmotionPrompt = BaseInstruction;

		FString ClassifierUserPrompt;
		ClassifierUserPrompt += BaseInstruction;
		ClassifierUserPrompt += TEXT("\n\nAllowed labels: joy, anger, sadness, fear, surprise, disgust, neutral.\n");
		if (!Request.CanonicalTranscript.IsEmpty())
		{
			ClassifierUserPrompt += TEXT("\nRecent context:\n");
			ClassifierUserPrompt += Request.CanonicalTranscript.Left(Request.MaxEmotionContextCharacters);
		}
		ClassifierUserPrompt += TEXT("\nLatest player line:\n\"");
		ClassifierUserPrompt += PlayerText.ToString().TrimStartAndEnd();
		ClassifierUserPrompt += TEXT("\"\n");

		if (NPCIDs.Num() <= 1)
		{
			const FString NPCName = NPCIDs.Num() == 1 ? NPCIDs[0].ToString() : FString(TEXT("NPC"));
			ClassifierUserPrompt += FString::Printf(TEXT("\nNPC to judge: %s\nRespond with exactly one word from the allowed labels."), *NPCName);
		}
		else
		{
			ClassifierUserPrompt += TEXT("\nNPCs to judge:\n");
			for (const FName NPCID : NPCIDs)
			{
				ClassifierUserPrompt += FString::Printf(TEXT("- %s\n"), *NPCID.ToString());
			}
			ClassifierUserPrompt += TEXT("Respond one per line as NPC=label. Do not explain.");
		}

		Request.PlayerText = ClassifierUserPrompt;

		UE_LOG(LogOffgridAI, Log,
			TEXT("[LLM][PlayerImpulseClassifier][Prompt] conversation=%s request=%s prompt=\"%s\""),
			*Request.ConversationId,
			*Request.RequestId,
			*Request.PlayerText);
	}

	if (Settings && Settings->bVerboseLogging)
	{
		const FString NPCList = FString::Join(Request.NPCIds, TEXT(", "));
		const FString AllowedEmotionList = Request.AllowedEmotionLabels.Num() > 0 ? FString::Join(Request.AllowedEmotionLabels, TEXT(", ")) : TEXT("<asset empty; service fallback>");
		UE_LOG(LogOffgridAI, Warning,
			TEXT("[LLM GENERATE TEXT UPDATE]\nConversationId: %s\nRequestId: %s\nNPCIds: [%s]\nModelPath: %s\nPlayerText:\n%s\n\nPersistentEmotionState:\n%s\n\nRecentTurnTranscript:\n%s\n\nSystemPrompt:\n%s\n\nDialogueOutputContract:\n%s\n\nEmotionPrompt:\n%s\nEmotion classifier allowed emotions: %s"),
			*Request.ConversationId,
			*Request.RequestId,
			*NPCList,
			*Request.ModelPath,
			*Request.PlayerText,
			*Request.PersistentEmotionState,
			*Request.CanonicalTranscript,
			*Request.SystemPrompt,
			*Request.DialogueOutputContract,
			*Request.EmotionPrompt,
			*AllowedEmotionList);
	}

	FOffgridAILLMResponse Response;
	if (!SendRequest(Request, &Response, true) || !Response.bOk)
	{
		UE_LOG(LogOffgridAI, Error, TEXT("LLM NamedPipeService: generate failed. %s"), *Response.ErrorMessage);
		return false;
	}

	FOffgridAILLMCompletedRequest Completed;
	Completed.RequestID = RequestID;
	Completed.ConversationID = ConversationID;
	Completed.JSONPayload = Response.JSONPayload;
	CompletedQueue.Enqueue(Completed);
	return true;
}


void FOffgridAILLMNamedPipeService::CancelActiveRequest(const FGuid& ConversationID)
{
	if (!ConversationID.IsValid() || !PipeClient || !PipeClient->IsConnected())
	{
		return;
	}

	FOffgridAILLMRequest Request;
	PopulateCommonRequestFields(Request);
	Request.Op = EOffgridAILLMOp::Cancel;
	Request.RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	Request.ConversationId = ConversationID.ToString(EGuidFormats::DigitsWithHyphens);
	SendRequest(Request, nullptr, false);
}

void FOffgridAILLMNamedPipeService::ClearSession(const FGuid& ConversationID)
{
	if (!ConversationID.IsValid() || !PipeClient || !PipeClient->IsConnected())
	{
		return;
	}

	FOffgridAILLMRequest Request;
	PopulateCommonRequestFields(Request);
	Request.Op = EOffgridAILLMOp::ClearSession;
	Request.RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	Request.ConversationId = ConversationID.ToString(EGuidFormats::DigitsWithHyphens);
	SendRequest(Request, nullptr, false);
}

bool FOffgridAILLMNamedPipeService::TryPopCompletedRequest(FOffgridAILLMCompletedRequest& OutCompletedRequest)
{
	return CompletedQueue.Dequeue(OutCompletedRequest);
}

bool FOffgridAILLMNamedPipeService::EnsureServiceConnected()
{
	if (PipeClient && PipeClient->IsConnected())
	{
		return true;
	}

	if (!Settings)
	{
		return false;
	}

	if (!ServiceProcessHandle.IsValid())
	{
		const FString ExecutablePath = Settings->ServiceExecutablePath;
		if (ExecutablePath.IsEmpty())
		{
			UE_LOG(LogOffgridAI, Error, TEXT("LLM NamedPipeService: ServiceExecutablePath was empty."));
			return false;
		}

		uint32 LocalProcessId = 0;
		ServiceProcessHandle = FPlatformProcess::CreateProc(
			*ExecutablePath,
			*BuildLaunchArguments(),
			false,
			true,
			true,
			&LocalProcessId,
			0,
			Settings->ServiceWorkingDirectory.IsEmpty() ? nullptr : *Settings->ServiceWorkingDirectory,
			nullptr,
			nullptr);

		ServiceProcessId = static_cast<int32>(LocalProcessId);

		if (!ServiceProcessHandle.IsValid())
		{
			UE_LOG(LogOffgridAI, Error, TEXT("LLM NamedPipeService: failed to launch service executable '%s'"), *ExecutablePath);
			return false;
		}
	}

	// Do not block here. Startup polling is driven by FOffgridAILocalServiceGateway::TickServiceStartup.
	// A single connect attempt per tick lets ASR, LLM, and TTS launch in parallel instead of
	// waiting for each service's full pipe timeout before starting the next one.
	if (PipeClient->Connect(Settings->PipeName))
	{
		return true;
	}

	return false;

}

bool FOffgridAILLMNamedPipeService::StartupServiceIfNeeded(FString* OutFatalError)
{
	if (bStartupCompleted)
	{
		return true;
	}

	FOffgridAILLMRequest Request;
	PopulateCommonRequestFields(Request);
	Request.Op = EOffgridAILLMOp::Startup;
	Request.RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);

	FOffgridAILLMResponse Response;
	if (!SendRequest(Request, &Response, true))
	{
		return false;
	}

	if (!Response.bOk)
	{
		const FString FatalError = Response.ErrorMessage.IsEmpty()
			? TEXT("LLM startup failed with no error message.")
			: Response.ErrorMessage;
		UE_LOG(LogOffgridAI, Error, TEXT("LLM NamedPipeService: startup failed: %s"), *FatalError);
		if (OutFatalError)
		{
			*OutFatalError = FatalError;
		}
		return false;
	}

	bStartupCompleted = true;
	return true;
}

bool FOffgridAILLMNamedPipeService::SendRequest(const FOffgridAILLMRequest& Request, FOffgridAILLMResponse* OutResponse, bool bExpectResponse)
{
	if (!PipeClient || !PipeClient->IsConnected())
	{
		return false;
	}

	TArray<uint8> Bytes;
	if (!OffgridAILLMProtocol::SerializeRequest(Request, Bytes) || !PipeClient->SendBytes(Bytes))
	{
		return false;
	}

	if (!bExpectResponse)
	{
		return true;
	}

	TArray<uint8> ResponseBytes;
	if (!PipeClient->ReceiveBytes(ResponseBytes))
	{
		return false;
	}

	if (OutResponse)
	{
		return OffgridAILLMProtocol::DeserializeResponse(ResponseBytes, *OutResponse);
	}

	return true;
}

void FOffgridAILLMNamedPipeService::PopulateCommonRequestFields(FOffgridAILLMRequest& OutRequest) const
{
	if (!Settings)
	{
		return;
	}

	OutRequest.Mode = Settings->Mode;
	OutRequest.ModelPath = Settings->ModelPath;
	OutRequest.ContextWindowTokens = Settings->ContextWindowTokens;
	OutRequest.MaxOutputTokens = Settings->MaxOutputTokens;
	OutRequest.Temperature = Settings->Temperature;
	OutRequest.MaxDialogueCharacters = Settings->MaxDialogueCharacters;
	OutRequest.GPULayers = Settings->GPULayers;
	OutRequest.ParallelSlots = Settings->ParallelSlots;
	OutRequest.bVerboseBackendLogging = Settings->bVerboseLogging;
	OutRequest.EmotionStateStep = 0.1f;
	BuildAllowedEmotionLabelsFromNames(TArray<FName>(), OutRequest.AllowedEmotionLabels);
}

void FOffgridAILLMNamedPipeService::PopulatePromptFields(
	FOffgridAILLMRequest& OutRequest,
	const TArray<FName>& NPCIDs,
	const UOffgridAIConversationPromptDataAsset* PromptAsset,
	const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord,
	const TArray<FName>& SupportedEmotionNames) const
{
	OutRequest.NPCIds.Reset();
	for (const FName NPCID : NPCIDs)
	{
		OutRequest.NPCIds.Add(NPCID.ToString());
	}

	if (OutRequest.AllowedEmotionLabels.IsEmpty())
	{
		BuildAllowedEmotionLabelsFromNames(SupportedEmotionNames, OutRequest.AllowedEmotionLabels);
	}

	const bool bIsEmotionImpactClassifier =
		OutRequest.RequestKind == EOffgridAILLMRequestKind::EmotionImpactClassifier;

	OutRequest.SystemPrompt = PromptAsset ? PromptAsset->SystemPrompt.ToString() : FString();
	// Dialogue generation is followed by the service-side line-emotion classifier.
	// ConversationManager maps those base emotion labels into persistent scholarly PAD state.
	OutRequest.EmotionPrompt = PromptAsset ? PromptAsset->EmotionPrompt.ToString() : FString();
	OutRequest.ResponseSchemaJson = bIsEmotionImpactClassifier ? FString() : (PromptAsset ? PromptAsset->ResponseSchemaJson.ToString() : FString());
	OutRequest.ExampleJson = bIsEmotionImpactClassifier ? FString() : (PromptAsset ? PromptAsset->ExampleJson.ToString() : FString());
	// The lean line-writer contract is prose/pipe-delimited, not JSON.
	// Emotion is intentionally excluded from the dialogue pass. The external LLM
	// service should first write NPC|spoken line, then run the separate emotion
	// classifier to assign the final performance label. Existing assets may still
	// carry old JSON GBNF grammars; forwarding those grammars forces the model to
	// emit JSON and makes the service reject the prose line-writer response. Keep
	// the asset fields for compatibility, but do not apply dialogue GBNF here.
	OutRequest.DialogueOutputContract = TEXT("NPC|spoken line");
	OutRequest.bUseDialogueGBNF = false;
	OutRequest.DialogueGBNF.Reset();
	OutRequest.bUseEmotionGBNF = false;
	OutRequest.EmotionGBNF.Reset();
	OutRequest.MaxDialogueTokens = PromptAsset
		? FMath::Clamp(PromptAsset->MaxDialogueTokens, 1, 256)
		: 32;
	OutRequest.EmotionContextTurnCount = PromptAsset ? FMath::Clamp(PromptAsset->EmotionContextTurnCount, 0, 20) : 10;
	OutRequest.MaxEmotionContextCharacters = PromptAsset ? FMath::Clamp(PromptAsset->MaxEmotionContextCharacters, 0, 8000) : 1200;
	const int32 MaxRecentTurns = PromptAsset ? FMath::Clamp(PromptAsset->RecentDialogueTurnCount, 0, 64) : 16;
	OutRequest.CanonicalTranscript = BuildRecentTurnTranscript(CanonicalRecord, MaxRecentTurns);
	OutRequest.PersistentEmotionState.Reset();
	BuildAllowedEmotionLabelsFromNames(
		bIsEmotionImpactClassifier ? TArray<FName>() : SupportedEmotionNames,
		OutRequest.AllowedEmotionLabels);

	if (OutRequest.AllowedEmotionLabels.Num() > 0)
	{
		OutRequest.ResponseSchemaJson = RewriteResponseSchemaEmotionEnum(OutRequest.ResponseSchemaJson, OutRequest.AllowedEmotionLabels);
	}

	if (OutRequest.bUseEmotionGBNF && OutRequest.AllowedEmotionLabels.Num() > 0)
	{
		TArray<FString> QuotedLabels;
		for (const FString& Label : OutRequest.AllowedEmotionLabels)
		{
			FString EscapedLabel = Label;
			EscapedLabel.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			EscapedLabel.ReplaceInline(TEXT("\""), TEXT("\\\""));
			QuotedLabels.Add(FString::Printf(TEXT("\"%s\""), *EscapedLabel));
		}

		OutRequest.EmotionGBNF = FString::Printf(
			TEXT("root ::= item | item \"\\n\" item | item \"\\n\" item \"\\n\" item | item \"\\n\" item \"\\n\" item \"\\n\" item\n")
			TEXT("item ::= [1-9] \"=\" emotion\n")
			TEXT("emotion ::= %s"),
			*FString::Join(QuotedLabels, TEXT(" | ")));
	}
}


FString FOffgridAILLMNamedPipeService::BuildRecentTurnTranscript(const TArray<FOffgridAIConversationRecordLine>& CanonicalRecord, int32 MaxRecentTurns) const
{
	if (CanonicalRecord.IsEmpty() || MaxRecentTurns <= 0)
	{
		return FString();
	}

	int32 StartIndex = 0;
	int32 PlayerTurnsSeen = 0;
	for (int32 Index = CanonicalRecord.Num() - 1; Index >= 0; --Index)
	{
		if (CanonicalRecord[Index].bIsPlayer)
		{
			++PlayerTurnsSeen;
			if (PlayerTurnsSeen >= MaxRecentTurns)
			{
				StartIndex = Index;
				break;
			}
		}
	}

	FString Result;
	for (int32 Index = StartIndex; Index < CanonicalRecord.Num(); ++Index)
	{
		const FOffgridAIConversationRecordLine& Line = CanonicalRecord[Index];
		if (Line.bIsPlayer)
		{
			Result += FString::Printf(TEXT("Player[%s]: %s\n"), *Line.SpeakerID.ToString(), *Line.Message.ToString());
		}
		else
		{
			Result += FString::Printf(TEXT("NPC[%s]: %s\n"), *Line.SpeakerID.ToString(), *Line.Message.ToString());
		}
	}
	return Result;
}

FString FOffgridAILLMNamedPipeService::BuildLaunchArguments() const
{
	if (!Settings)
	{
		return FString();
	}

	FString Arguments;
	for (const FString& Arg : Settings->LaunchArguments)
	{
		if (!Arguments.IsEmpty())
		{
			Arguments += TEXT(" ");
		}
		Arguments += QuoteArg(Arg);
	}
	return Arguments;
}
