#include "OffgridAIConversationManager.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "OffgridAI.h"
#include "Core/OffgridAIOrchestrator.h"

namespace
{

    FString OffgridExtractBaseEmotionToken(const FString& RawEmotion);
    FString OffgridExtractStrictBaseEmotionToken(const FString& RawEmotion);

    int32 ParseImpactDirectionToken(const FString& InText)
    {
        FString Value = InText.TrimStartAndEnd().ToLower();
        Value.ReplaceInline(TEXT("."), TEXT(""));
        Value.ReplaceInline(TEXT(","), TEXT(""));
        Value.ReplaceInline(TEXT(":"), TEXT(""));

        if (Value.Contains(TEXT("higher")) || Value.Contains(TEXT("up")) || Value.Contains(TEXT("increase")) || Value.Contains(TEXT("raised")))
        {
            return 1;
        }
        if (Value.Contains(TEXT("lower")) || Value.Contains(TEXT("down")) || Value.Contains(TEXT("decrease")) || Value.Contains(TEXT("reduced")))
        {
            return -1;
        }
        return 0;
    }

    bool ExtractDirectionForKey(const FString& Response, const FString& Key, int32& OutDirection)
    {
        TArray<FString> Lines;
        Response.ParseIntoArrayLines(Lines, false);
        for (const FString& Line : Lines)
        {
            if (Line.Contains(Key, ESearchCase::IgnoreCase))
            {
                FString Left;
                FString Right;
                if (Line.Split(TEXT(":"), &Left, &Right) || Line.Split(TEXT("="), &Left, &Right))
                {
                    OutDirection = ParseImpactDirectionToken(Right);
                    return true;
                }

                OutDirection = ParseImpactDirectionToken(Line);
                return true;
            }
        }
        return false;
    }

    bool FindEmotionEnumRecursive(const TSharedPtr<FJsonObject>& Object, TArray<FString>& OutAllowed)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* EmotionObject = nullptr;
        if (Object->TryGetObjectField(TEXT("emotion"), EmotionObject) && EmotionObject && EmotionObject->IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* EnumArray = nullptr;
            if ((*EmotionObject)->TryGetArrayField(TEXT("enum"), EnumArray) && EnumArray)
            {
                for (const TSharedPtr<FJsonValue>& Entry : *EnumArray)
                {
                    FString Value;
                    if (Entry.IsValid() && Entry->TryGetString(Value))
                    {
                        OutAllowed.AddUnique(Value);
                    }
                }
                if (OutAllowed.Num() > 0)
                {
                    return true;
                }
            }
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
        {
            if (!Pair.Value.IsValid())
            {
                continue;
            }

            if (Pair.Value->Type == EJson::Object)
            {
                if (FindEmotionEnumRecursive(Pair.Value->AsObject(), OutAllowed))
                {
                    return true;
                }
            }
            else if (Pair.Value->Type == EJson::Array)
            {
                for (const TSharedPtr<FJsonValue>& Entry : Pair.Value->AsArray())
                {
                    if (Entry.IsValid() && Entry->Type == EJson::Object && FindEmotionEnumRecursive(Entry->AsObject(), OutAllowed))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    bool ExtractAllowedEmotionsFromSchema(const UOffgridAIConversationPromptDataAsset* PromptAsset, TArray<FString>& OutAllowed)
    {
        OutAllowed.Reset();
        if (!PromptAsset)
        {
            return false;
        }

        const FString SchemaJson = PromptAsset->ResponseSchemaJson.ToString();
        if (SchemaJson.IsEmpty())
        {
            return false;
        }

        TSharedPtr<FJsonObject> RootObject;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SchemaJson);
        if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
        {
            return false;
        }

        return FindEmotionEnumRecursive(RootObject, OutAllowed) && OutAllowed.Num() > 0;
    }

    FString NormalizeEmotionLabelToRuntimeKey(FString Value)
    {
        Value = Value.TrimStartAndEnd().ToLower();
        Value.ReplaceInline(TEXT("\""), TEXT(""));
        Value.ReplaceInline(TEXT("'"), TEXT(""));
        Value.ReplaceInline(TEXT("_"), TEXT(""));
        Value.ReplaceInline(TEXT("-"), TEXT(""));

        if (Value == TEXT("none") || Value == TEXT("noop") || Value == TEXT("noemotion")) return TEXT("neutral");
        if (Value == TEXT("happy") || Value == TEXT("happiness") || Value == TEXT("joyful") || Value == TEXT("friendly") || Value == TEXT("amused") || Value == TEXT("amusement") || Value == TEXT("upbeat")) return TEXT("joy");
        if (Value == TEXT("angry") || Value == TEXT("mad") || Value == TEXT("rage") || Value == TEXT("furious") || Value == TEXT("irritated") || Value == TEXT("annoyed")) return TEXT("anger");
        if (Value == TEXT("sad") || Value == TEXT("sorrow") || Value == TEXT("unhappy") || Value == TEXT("sympathy") || Value == TEXT("regret")) return TEXT("sadness");
        if (Value == TEXT("fearful") || Value == TEXT("afraid") || Value == TEXT("scared") || Value == TEXT("anxious") || Value == TEXT("intimidated")) return TEXT("fear");
        if (Value == TEXT("surprised") || Value == TEXT("shock") || Value == TEXT("shocked") || Value == TEXT("unexpected")) return TEXT("surprise");
        if (Value == TEXT("disgusted") || Value == TEXT("grossedout") || Value == TEXT("gross") || Value == TEXT("revulsion") || Value == TEXT("repulsion")) return TEXT("disgust");
        return Value;
    }

    FName NormalizeEmotionNameToRuntimeKey(FName Emotion)
    {
        return FName(*NormalizeEmotionLabelToRuntimeKey(Emotion.ToString()));
    }

    TArray<FName> DefaultRuntimePerformanceEmotions()
    {
        return { TEXT("neutral"), TEXT("joy"), TEXT("anger"), TEXT("sadness"), TEXT("fear"), TEXT("surprise"), TEXT("disgust") };
    }

    void AddNormalizedEmotionNameUnique(TArray<FName>& OutNames, FName InName, bool bIncludeNeutral)
    {
        const FName Normalized = NormalizeEmotionNameToRuntimeKey(InName);
        if (Normalized == NAME_None)
        {
            return;
        }
        if (!bIncludeNeutral && Normalized == TEXT("neutral"))
        {
            return;
        }
        OutNames.AddUnique(Normalized);
    }

}

void UOffgridAIConversationManager::InitializeConversation(
    UOffgridAIOrchestrator* InOrchestrator,
    const TArray<FName>& InPlayerIDs,
    const TArray<FName>& InNPCIDs,
    UOffgridAIConversationPromptDataAsset* InConversationPromptAsset)
{
    Orchestrator = InOrchestrator;
    ConversationID = FGuid::NewGuid();
    PlayerIDs = InPlayerIDs;
    NPCIDs = InNPCIDs;
    ConversationPromptAsset = InConversationPromptAsset;
    PendingNPCLineRequests.Empty();
    ActiveNPCLineIndex = INDEX_NONE;
    CanonicalConversationRecord.Empty();
    InitializeEmotionTransitionState();
    bHasAutoStartedGreeting = false;
    ConversationLineSerial = 0;

    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] InitializeConversation id=%s players=%d npcs=%d"),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        PlayerIDs.Num(),
        NPCIDs.Num());

    SetConversationState(EOffgridAIConversationState::Inactive);
}

void UOffgridAIConversationManager::MarkPrimedAndAwaitingInput()
{
    const bool bShouldAutoStartGreeting =
        ConversationPromptAsset &&
        ConversationPromptAsset->bAutoStartNPCGreeting &&
        !bHasAutoStartedGreeting;

    if (bShouldAutoStartGreeting)
    {
        bHasAutoStartedGreeting = true;

        UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] Conversation primed; auto-starting hidden NPC greeting conversation=%s"),
            *ConversationID.ToString(EGuidFormats::DigitsWithHyphens));

        SubmitHiddenPlayerTurnText(ConversationPromptAsset->AutoStartHiddenPlayerTurn);
        return;
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] Conversation primed; entering AwaitingInput conversation=%s"),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens));
    SetConversationState(EOffgridAIConversationState::AwaitingInput);
}

void UOffgridAIConversationManager::SubmitHiddenPlayerTurnText(const FText& HiddenPlayerText)
{
    const FString TrimmedString = HiddenPlayerText.ToString().TrimStartAndEnd();
    if (TrimmedString.IsEmpty())
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] Hidden player turn was empty; entering AwaitingInput conversation=%s"),
            *ConversationID.ToString(EGuidFormats::DigitsWithHyphens));
        SetConversationState(EOffgridAIConversationState::AwaitingInput);
        return;
    }

    const FName HiddenPlayerID = PlayerIDs.Num() > 0 ? PlayerIDs[0] : FName(TEXT("Player"));

    UE_LOG(LogOffgridAI, Log,
        TEXT("[ConversationManager] Hidden player turn requested through normal player timing path conversation=%s player=%s text=\"%s\""),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        *HiddenPlayerID.ToString(),
        *TrimmedString);

    if (Orchestrator)
    {
        Orchestrator->SubmitHiddenPlayerTurnThroughNormalPath(ConversationID, HiddenPlayerID, FText::FromString(TrimmedString));
    }
}

void UOffgridAIConversationManager::SubmitHiddenPlayerTurnTextAfterSyntheticASR(const FText& HiddenPlayerText)
{
    SubmitPlayerTurnTextInternal(
        PlayerIDs.Num() > 0 ? PlayerIDs[0] : FName(TEXT("Player")),
        HiddenPlayerText,
        false,
        TEXT("HiddenPlayerTranscriptCommitted"),
        TEXT("SubmitHiddenPlayerTurnText"));
}

void UOffgridAIConversationManager::BeginRecording()
{
    SetConversationState(EOffgridAIConversationState::Recording);
}

void UOffgridAIConversationManager::BeginProcessingASR()
{
    SetConversationState(EOffgridAIConversationState::ProcessingASR);
}

void UOffgridAIConversationManager::SubmitPlayerTurnText(FName PlayerID, const FText& PlayerText)
{
    SubmitPlayerTurnTextInternal(
        PlayerID,
        PlayerText,
        true,
        TEXT("PlayerTranscriptCommitted"),
        TEXT("SubmitPlayerTurnText"));
}

void UOffgridAIConversationManager::SubmitPlayerTurnText(const FText& PlayerText)
{
    SubmitPlayerTurnText(PlayerIDs.Num() > 0 ? PlayerIDs[0] : FName(TEXT("Player")), PlayerText);
}

void UOffgridAIConversationManager::SubmitPlayerTurnTextInternal(
    FName PlayerID,
    const FText& PlayerText,
    bool bBroadcastTranscript,
    const TCHAR* LatencyEventName,
    const TCHAR* LogLabel)
{
    const FString NormalizedString = PlayerText.ToString().TrimStartAndEnd().ToLower();
    const FText NormalizedPlayerText = FText::FromString(NormalizedString);

    FOffgridAIConversationRecordLine RecordLine;
    RecordLine.bIsPlayer = true;
    if (!PlayerIDs.Contains(PlayerID))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] Player turn supplied unknown player=%s; using first registered player."), *PlayerID.ToString());
        PlayerID = PlayerIDs.Num() > 0 ? PlayerIDs[0] : FName(TEXT("Player"));
    }
    RecordLine.SpeakerID = PlayerID;
    RecordLine.Message = NormalizedPlayerText;
    RecordLine.ConversationID = ConversationID;

    AppendToCanonicalRecord(RecordLine);

    if (Orchestrator)
    {
        Orchestrator->NoteTurnLatencyEvent(
            LatencyEventName,
            FString::Printf(TEXT("player=%s chars=%d"), *RecordLine.SpeakerID.ToString(), NormalizedString.Len()));
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] %s conversation=%s player=%s text=\"%s\""),
        LogLabel,
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        *RecordLine.SpeakerID.ToString(),
        *NormalizedString);

    if (bBroadcastTranscript)
    {
        FOffgridAITranscriptLine TranscriptLine;
        TranscriptLine.bIsPlayer = RecordLine.bIsPlayer;
        TranscriptLine.SpeakerID = RecordLine.SpeakerID;
        TranscriptLine.Dialogue = RecordLine.Message;
        TranscriptLine.Message = RecordLine.Message;

        if (Orchestrator)
        {
            Orchestrator->PlayerTextTranscriptReady(ConversationID, TranscriptLine);
        }
    }

    SetConversationState(EOffgridAIConversationState::ProcessingLLM);

    if (Orchestrator)
    {
        Orchestrator->SubmitConversationTurnToLLM(ConversationID, NormalizedPlayerText);
    }
}


bool UOffgridAIConversationManager::ApplyPlayerImpulseClassifierResult(const FString& RawClassifierResponse, const FText& PlayerText)
{
    bool bParsedAny = false;
    TMap<FName, FName> ParsedEmotionByNPC;
    TMap<FName, bool> bValidParsedEmotionByNPC;

    const FString TrimmedRaw = RawClassifierResponse.TrimStartAndEnd();
    if (NPCIDs.Num() == 1)
    {
        const FString ParsedToken = OffgridExtractStrictBaseEmotionToken(TrimmedRaw);
        const bool bValid = !ParsedToken.IsEmpty();
        ParsedEmotionByNPC.Add(NPCIDs[0], bValid ? FName(*ParsedToken) : FName(TEXT("neutral")));
        bValidParsedEmotionByNPC.Add(NPCIDs[0], bValid);
        bParsedAny = true;
    }
    else
    {
        TArray<FString> Lines;
        TrimmedRaw.ParseIntoArrayLines(Lines, true);
        for (int32 NPCIndex = 0; NPCIndex < NPCIDs.Num(); ++NPCIndex)
        {
            const FName NPCID = NPCIDs[NPCIndex];
            FName ParsedEmotion = NAME_None;
            bool bValid = false;

            for (const FString& RawLine : Lines)
            {
                const FString Line = RawLine.TrimStartAndEnd();
                if (Line.IsEmpty())
                {
                    continue;
                }

                const FString NPCName = NPCID.ToString();
                const FString IndexName = FString::Printf(TEXT("N%d"), NPCIndex);
                const bool bMatchesNPC = Line.StartsWith(NPCName + TEXT("="), ESearchCase::IgnoreCase)
                    || Line.StartsWith(NPCName + TEXT(":"), ESearchCase::IgnoreCase)
                    || Line.StartsWith(IndexName + TEXT("="), ESearchCase::IgnoreCase)
                    || Line.StartsWith(IndexName + TEXT(":"), ESearchCase::IgnoreCase);
                if (bMatchesNPC)
                {
                    const FString ParsedToken = OffgridExtractStrictBaseEmotionToken(Line);
                    if (!ParsedToken.IsEmpty())
                    {
                        ParsedEmotion = FName(*ParsedToken);
                        bValid = true;
                    }
                    break;
                }
            }

            if (ParsedEmotion == NAME_None)
            {
                ParsedEmotion = FName(TEXT("neutral"));
            }

            ParsedEmotionByNPC.Add(NPCID, ParsedEmotion);
            bValidParsedEmotionByNPC.Add(NPCID, bValid);
            bParsedAny = true;
        }
    }

    for (const FName& NPCID : NPCIDs)
    {
        const FName* ParsedEmotion = ParsedEmotionByNPC.Find(NPCID);
        const FName Emotion = ParsedEmotion ? NormalizeEmotionNameToRuntimeKey(*ParsedEmotion) : FName(TEXT("neutral"));
        const bool* bValid = bValidParsedEmotionByNPC.Find(NPCID);
        const bool bWasValid = bValid && *bValid;

        UE_LOG(LogOffgridAI, Log,
            TEXT("[ConversationManager][PlayerImpulse] npc=%s player=\"%s\" raw=\"%s\" parsed=%s valid=%s"),
            *NPCID.ToString(),
            *PlayerText.ToString(),
            *RawClassifierResponse,
            *Emotion.ToString(),
            bWasValid ? TEXT("true") : TEXT("false"));

        MovePADSTowardLineEmotionTarget(NPCID, Emotion);
    }

    RefreshEmotionTargetsFromPADS();
    return bParsedAny;
}

void UOffgridAIConversationManager::SubmitNPCTurnJSON(const FString& JSONPayload)
{
    PendingNPCLineRequests.Empty();
    ActiveNPCLineIndex = INDEX_NONE;

    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager][LineEmotionClassifier][RawDialoguePayload] conversation=%s raw_json=%s"),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        *JSONPayload);

    TArray<FOffgridAILinePerformanceRequest> ParsedLineRequests;
    if (!TryBuildLineRequestsFromJSON(JSONPayload, ParsedLineRequests))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[ConversationManager] LLM service contract violation conversation=%s payload=%s"),
            *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
            *JSONPayload);

        if (Orchestrator)
        {
            Orchestrator->NoteTurnLatencyEvent(
                TEXT("LLMContractViolation"),
                FString::Printf(TEXT("chars=%d"), JSONPayload.Len()));
        }

        SetConversationState(EOffgridAIConversationState::Error);
        return;
    }

    PendingNPCLineRequests = ParsedLineRequests;
    ActiveNPCLineIndex = 0;

    // Runtime emotion was already updated from the latest player-line impulse before
    // dialogue generation. Do not re-classify/compound from the NPC line JSON emotion;
    // that field is legacy/debug only and can otherwise bias polite NPC replies toward neutral.

    if (Orchestrator)
    {
        Orchestrator->NoteTurnLatencyEvent(
            TEXT("LLMJSONValidated"),
            FString::Printf(TEXT("line_count=%d"), PendingNPCLineRequests.Num()));
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] Parsed %d NPC line request(s) conversation=%s"),
        PendingNPCLineRequests.Num(),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens));

    for (FOffgridAILinePerformanceRequest& LineRequest : PendingNPCLineRequests)
    {
        // Runtime PADS was moved by the latest player impulse before dialogue generation.
        // The resolved state now drives face family/magnitude, TTS label, and chat label.
        if (const FEmotionTransitionState* EmotionState = EmotionTransitionStateByNPC.Find(LineRequest.NPCID))
        {
            LineRequest.Emotion = EmotionState->TargetEmotion;
            LineRequest.EmotionMagnitude = EmotionState->TargetMagnitude;
            LineRequest.TTSEmotionInstruction = EmotionState->TargetTTS.IsEmpty() ? EmotionState->TargetLabel : EmotionState->TargetTTS;
        }
        else
        {
            LineRequest.EmotionMagnitude = 0.0f;
            LineRequest.TTSEmotionInstruction = TEXT("neutral");
        }

        UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager][LineEmotion] line=%s npc=%s face=%s:%.2f chat=%s tts=\"%s\" text=\"%s\""),
            *LineRequest.LineID.ToString(),
            *LineRequest.NPCID.ToString(),
            *LineRequest.Emotion.ToString(),
            LineRequest.EmotionMagnitude,
            *LineRequest.TTSEmotionInstruction.ToUpper(),
            *LineRequest.TTSEmotionInstruction,
            *LineRequest.Dialogue.ToString());

        const FName ResolvedChatEmotion = LineRequest.TTSEmotionInstruction.IsEmpty()
            ? LineRequest.Emotion
            : FName(*LineRequest.TTSEmotionInstruction);

        FOffgridAIConversationRecordLine RecordLine;
        RecordLine.bIsPlayer = false;
        RecordLine.SpeakerID = LineRequest.NPCID;
        RecordLine.VoiceID = LineRequest.VoiceID;
        RecordLine.Emotion = ResolvedChatEmotion;
        RecordLine.Message = LineRequest.Dialogue;
        RecordLine.ConversationID = LineRequest.ConversationID;
        RecordLine.LineID = LineRequest.LineID;

        AppendToCanonicalRecord(RecordLine);

        if (Orchestrator)
        {
            Orchestrator->NoteTurnLatencyEvent(
                TEXT("NPCTranscriptCommitted"),
                FString::Printf(TEXT("npc=%s line=%s chars=%d"), *LineRequest.NPCID.ToString(), *LineRequest.LineID.ToString(), LineRequest.Dialogue.ToString().Len()));
        }

        FOffgridAITranscriptLine TranscriptLine;
        TranscriptLine.bIsPlayer = false;
        TranscriptLine.SpeakerID = LineRequest.NPCID;
        TranscriptLine.Emotion = ResolvedChatEmotion;
        TranscriptLine.Dialogue = LineRequest.Dialogue;

        FString SpeakerTag = LineRequest.NPCID.ToString().ToUpper();
        FString EmotionTag = ResolvedChatEmotion.IsNone()
            ? TEXT("NEUTRAL")
            : ResolvedChatEmotion.ToString().ToUpper();
        TranscriptLine.Message = FText::FromString(FString::Printf(
            TEXT("%s [%s] %s"),
            *SpeakerTag,
            *EmotionTag,
            *LineRequest.Dialogue.ToString()));

        if (Orchestrator)
        {
            Orchestrator->NPCTextTranscriptReady(ConversationID, TranscriptLine);
        }
    }


    SetConversationState(EOffgridAIConversationState::Speaking);

    if (Orchestrator && PendingNPCLineRequests.IsValidIndex(ActiveNPCLineIndex))
    {
        UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] Starting NPC line sequence conversation=%s line_count=%d first_line=%s first_npc=%s"),
            *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
            PendingNPCLineRequests.Num(),
            *PendingNPCLineRequests[ActiveNPCLineIndex].LineID.ToString(),
            *PendingNPCLineRequests[ActiveNPCLineIndex].NPCID.ToString());

        Orchestrator->BeginNPCLineSequence(ConversationID, PendingNPCLineRequests);
    }
}

void UOffgridAIConversationManager::NPCLinePerformanceComplete(FName LineID)
{
    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] NPCLinePerformanceComplete conversation=%s line_id=%s active_index=%d"),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
        *LineID.ToString(),
        ActiveNPCLineIndex);

    if (ActiveNPCLineIndex == INDEX_NONE || !PendingNPCLineRequests.IsValidIndex(ActiveNPCLineIndex))
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[ConversationManager] NPCLinePerformanceComplete invalid active index conversation=%s"),
            *ConversationID.ToString(EGuidFormats::DigitsWithHyphens));
        SetConversationState(EOffgridAIConversationState::Error);
        return;
    }

    if (PendingNPCLineRequests[ActiveNPCLineIndex].LineID != LineID)
    {
        UE_LOG(LogOffgridAI, Error, TEXT("[ConversationManager] NPCLinePerformanceComplete mismatched line. expected=%s actual=%s"),
            *PendingNPCLineRequests[ActiveNPCLineIndex].LineID.ToString(),
            *LineID.ToString());
        SetConversationState(EOffgridAIConversationState::Error);
        return;
    }

    ++ActiveNPCLineIndex;

    if (PendingNPCLineRequests.IsValidIndex(ActiveNPCLineIndex))
    {
        UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] NPC line complete; orchestrator pipeline owns next playback conversation=%s next_line=%s next_npc=%s"),
            *ConversationID.ToString(EGuidFormats::DigitsWithHyphens),
            *PendingNPCLineRequests[ActiveNPCLineIndex].LineID.ToString(),
            *PendingNPCLineRequests[ActiveNPCLineIndex].NPCID.ToString());
    }
    else
    {
        UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] NPC turn sequence complete conversation=%s returning to AwaitingInput"),
            *ConversationID.ToString(EGuidFormats::DigitsWithHyphens));

        PendingNPCLineRequests.Empty();
        ActiveNPCLineIndex = INDEX_NONE;
        RelaxEmotionTargetsForActiveNPCs();
        SetConversationState(EOffgridAIConversationState::AwaitingInput);

    }
}

void UOffgridAIConversationManager::CancelCurrentTurn()
{
    if (Orchestrator)
    {
        Orchestrator->NoteTurnLatencyEvent(TEXT("TurnCancelled"));
    }

    PendingNPCLineRequests.Empty();
    ActiveNPCLineIndex = INDEX_NONE;
    SetConversationState(EOffgridAIConversationState::AwaitingInput);
}

void UOffgridAIConversationManager::SetConversationState(EOffgridAIConversationState NewState)
{
    if (CurrentConversationState == NewState)
    {
        return;
    }

    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager] State %s -> %s conversation=%s"),
        *UEnum::GetValueAsString(CurrentConversationState),
        *UEnum::GetValueAsString(NewState),
        *ConversationID.ToString(EGuidFormats::DigitsWithHyphens));

    CurrentConversationState = NewState;

    if (Orchestrator)
    {
        Orchestrator->HandleConversationStateChanged(ConversationID, CurrentConversationState);
    }
}

void UOffgridAIConversationManager::AppendToCanonicalRecord(const FOffgridAIConversationRecordLine& RecordLine)
{
    CanonicalConversationRecord.Add(RecordLine);
}

void UOffgridAIConversationManager::InitializeEmotionTransitionState()
{
    PADSStateByNPC.Empty();
    EmotionTransitionStateByNPC.Empty();
    PADEmotionMappings.Reset();

    // First-pass built-in mapping. This will be superseded by Content/Face/MetaHumanPADEmotionMapping.json
    // loading once the exact thresholds have been validated in-game.
    auto AddMapping = [this](const TCHAR* Label, const TCHAR* Family, float P, float A, float D, float S, float Radius, float Magnitude)
    {
        FPADSEmotionMapping Mapping;
        Mapping.Label = Label;
        Mapping.Family = FName(Family);
        Mapping.TTS = Label;
        Mapping.Center = FVector4f(P, A, D, S);
        Mapping.Radius = Radius;
        Mapping.Magnitude = Magnitude;
        PADEmotionMappings.Add(Mapping);
    };

    // Built-in fallback mapping in scholarly PAD space: P/A/D are [-1,+1], neutral is (0,0,0).
    // Stability is OffgridAI-specific [0,1]. Content is low-stage joy; stronger joy
    // should require meaningfully positive Pleasure rather than catching neutral states.
    AddMapping(TEXT("neutral"), TEXT("neutral"), 0.00f, 0.00f, 0.00f, 0.80f, 0.75f, 0.00f);
    AddMapping(TEXT("content"), TEXT("joy"), 0.20f, -0.10f, 0.10f, 0.75f, 0.45f, 0.20f);
    AddMapping(TEXT("pleased"), TEXT("joy"), 0.45f, 0.20f, 0.25f, 0.70f, 0.45f, 0.35f);
    AddMapping(TEXT("happy"), TEXT("joy"), 0.81f, 0.51f, 0.46f, 0.65f, 0.55f, 0.70f);
    AddMapping(TEXT("annoyed"), TEXT("anger"), -0.28f, 0.38f, 0.16f, 0.48f, 0.45f, 0.45f);
    AddMapping(TEXT("angry"), TEXT("anger"), -0.51f, 0.59f, 0.25f, 0.35f, 0.55f, 0.75f);
    AddMapping(TEXT("enraged"), TEXT("anger"), -0.78f, 0.86f, 0.48f, 0.18f, 0.50f, 1.00f);
    AddMapping(TEXT("sad"), TEXT("sadness"), -0.63f, -0.27f, -0.33f, 0.40f, 0.55f, 0.65f);
    AddMapping(TEXT("afraid"), TEXT("fear"), -0.64f, 0.60f, -0.43f, 0.25f, 0.55f, 0.80f);
    AddMapping(TEXT("surprised"), TEXT("surprise"), 0.00f, 0.85f, -0.05f, 0.30f, 0.55f, 0.65f);
    AddMapping(TEXT("disgusted"), TEXT("disgust"), -0.60f, 0.35f, 0.11f, 0.42f, 0.55f, 0.75f);

    if (NPCIDs.Num() > 0)
    {
        for (const FName& NPCID : NPCIDs)
        {
            InitializeEmotionTransitionStateForNPC(NPCID);
        }
    }
    else
    {
        InitializeEmotionTransitionStateForNPC(FName(TEXT("NPC")));
    }
}

void UOffgridAIConversationManager::InitializeEmotionTransitionStateForNPC(FName NPCID)
{
    InitializePADSStateForNPC(NPCID);

    FEmotionTransitionState& State = EmotionTransitionStateByNPC.FindOrAdd(NPCID);
    State.CurrentEmotion = TEXT("neutral");
    State.TargetEmotion = TEXT("neutral");
    State.CurrentLabel = TEXT("neutral");
    State.TargetLabel = TEXT("neutral");
    State.TargetTTS = TEXT("neutral");
    State.CurrentMagnitude = 0.0f;
    State.TargetMagnitude = 0.0f;
    State.TransitionElapsedSeconds = 0.0f;
    State.bHasAssignedTarget = true;
    State.bIsRelaxing = false;
}

void UOffgridAIConversationManager::InitializePADSStateForNPC(FName NPCID)
{
    FOffgridAIPADSState StartingState;
    if (Orchestrator)
    {
        Orchestrator->GetNPCStartingPADSState(NPCID, StartingState);
    }
    StartingState.Clamp01();
    PADSStateByNPC.FindOrAdd(NPCID) = StartingState;

    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager][PADS] init npc=%s P=%.2f A=%.2f D=%.2f S=%.2f"),
        *NPCID.ToString(), StartingState.Pleasure, StartingState.Activation, StartingState.Dominance, StartingState.Stability);
}

float UOffgridAIConversationManager::SmoothStep01(float Alpha)
{
    Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
    return Alpha * Alpha * (3.0f - 2.0f * Alpha);
}

float UOffgridAIConversationManager::EasedEmotionTransitionSpeed(float ElapsedSeconds, float TargetSpeedPerSecond)
{
    const float EaseAlpha = EmotionTransitionSpeedEaseInSeconds <= KINDA_SMALL_NUMBER
        ? 1.0f
        : ElapsedSeconds / EmotionTransitionSpeedEaseInSeconds;

    return FMath::Max(0.0f, TargetSpeedPerSecond) * SmoothStep01(EaseAlpha);
}

FOffgridAIPADSState UOffgridAIConversationManager::GetTargetPADSForLineEmotion(FName LineEmotion) const
{
    const FName Emotion = NormalizeEmotionNameToRuntimeKey(LineEmotion);

    FOffgridAIPADSState Target;
    Target.Pleasure = 0.0f;
    Target.Activation = 0.0f;
    Target.Dominance = 0.0f;
    Target.Stability = 0.80f;

    // Scholarly PAD centers. P/A/D are [-1,+1]. Stability is an OffgridAI damping
    // dimension, not part of canonical PAD; values here describe how destabilizing
    // each emotional tone should be.
    if (Emotion == TEXT("joy"))
    {
        Target.Pleasure = 0.81f;
        Target.Activation = 0.51f;
        Target.Dominance = 0.46f;
        Target.Stability = 0.72f;
    }
    else if (Emotion == TEXT("anger"))
    {
        Target.Pleasure = -0.51f;
        Target.Activation = 0.59f;
        Target.Dominance = 0.25f;
        Target.Stability = 0.35f;
    }
    else if (Emotion == TEXT("sadness"))
    {
        Target.Pleasure = -0.63f;
        Target.Activation = -0.27f;
        Target.Dominance = -0.33f;
        Target.Stability = 0.40f;
    }
    else if (Emotion == TEXT("fear"))
    {
        Target.Pleasure = -0.64f;
        Target.Activation = 0.60f;
        Target.Dominance = -0.43f;
        Target.Stability = 0.25f;
    }
    else if (Emotion == TEXT("surprise"))
    {
        Target.Pleasure = 0.0f;
        Target.Activation = 0.85f;
        Target.Dominance = -0.05f;
        Target.Stability = 0.30f;
    }
    else if (Emotion == TEXT("disgust"))
    {
        Target.Pleasure = -0.60f;
        Target.Activation = 0.35f;
        Target.Dominance = 0.11f;
        Target.Stability = 0.42f;
    }

    Target.Clamp01();
    return Target;
}

void UOffgridAIConversationManager::MovePADSTowardLineEmotionTarget(FName NPCID, FName LineEmotion)
{
    const FName Emotion = NormalizeEmotionNameToRuntimeKey(LineEmotion);
    FOffgridAIPADSState& State = PADSStateByNPC.FindOrAdd(NPCID);
    const FOffgridAIPADSState Previous = State;
    const FOffgridAIPADSState Target = GetTargetPADSForLineEmotion(Emotion);

    const float Stability = FMath::Clamp(Previous.Stability, 0.0f, 1.0f);
    const float ImpulseWeight = FMath::Clamp(1.0f - Stability, 0.05f, 1.0f);

    State.Pleasure = FMath::Lerp(Previous.Pleasure, Target.Pleasure, ImpulseWeight);
    State.Activation = FMath::Lerp(Previous.Activation, Target.Activation, ImpulseWeight);
    State.Dominance = FMath::Lerp(Previous.Dominance, Target.Dominance, ImpulseWeight);
    State.Stability = FMath::Lerp(Previous.Stability, Target.Stability, ImpulseWeight);
    State.Clamp01();

    UE_LOG(LogOffgridAI, Log,
        TEXT("[ConversationManager][PADS] npc=%s player_impulse=%s old=(%.2f %.2f %.2f %.2f) target=(%.2f %.2f %.2f %.2f) new=(%.2f %.2f %.2f %.2f) stability=%.2f impulse_weight=%.2f"),
        *NPCID.ToString(),
        *Emotion.ToString(),
        Previous.Pleasure, Previous.Activation, Previous.Dominance, Previous.Stability,
        Target.Pleasure, Target.Activation, Target.Dominance, Target.Stability,
        State.Pleasure, State.Activation, State.Dominance, State.Stability,
        Stability,
        ImpulseWeight);
}

void UOffgridAIConversationManager::RefreshEmotionTargetsFromPADS()
{
    TArray<FName> ActiveNPCIDs = NPCIDs;
    if (ActiveNPCIDs.Num() == 0)
    {
        ActiveNPCIDs.Add(FName(TEXT("NPC")));
    }

    for (const FName& NPCID : ActiveNPCIDs)
    {
        const FOffgridAIPADSState* PADS = PADSStateByNPC.Find(NPCID);
        if (!PADS)
        {
            continue;
        }

        const FPADSEmotionMapping* Best = nullptr;
        float BestScore = TNumericLimits<float>::Max();
        const FVector4f Point(PADS->Pleasure, PADS->Activation, PADS->Dominance, PADS->Stability);
        for (const FPADSEmotionMapping& Mapping : PADEmotionMappings)
        {
            const FVector4f Delta = Point - Mapping.Center;
            const float Score = Delta.SizeSquared() / FMath::Max(0.01f, Mapping.Radius * Mapping.Radius);
            if (Score < BestScore)
            {
                BestScore = Score;
                Best = &Mapping;
            }
        }

        FEmotionTransitionState& EmotionState = EmotionTransitionStateByNPC.FindOrAdd(NPCID);
        if (Best)
        {
            EmotionState.TargetEmotion = Best->Family;
            EmotionState.TargetMagnitude = FMath::Clamp(Best->Magnitude, 0.0f, 1.0f);
            EmotionState.TargetLabel = Best->Label;
            EmotionState.TargetTTS = Best->TTS;
        }
        else
        {
            EmotionState.TargetEmotion = TEXT("neutral");
            EmotionState.TargetMagnitude = 0.0f;
            EmotionState.TargetLabel = TEXT("neutral");
            EmotionState.TargetTTS = TEXT("neutral");
        }
        EmotionState.TransitionElapsedSeconds = 0.0f;
        EmotionState.bHasAssignedTarget = true;
        EmotionState.bIsRelaxing = false;

        UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager][ResolvedEmotion] npc=%s pads=(%.2f %.2f %.2f %.2f) family=%s label=%s magnitude=%.2f score=%.2f"),
            *NPCID.ToString(), PADS->Pleasure, PADS->Activation, PADS->Dominance, PADS->Stability,
            *EmotionState.TargetEmotion.ToString(), *EmotionState.TargetLabel, EmotionState.TargetMagnitude, BestScore);
    }
}

void UOffgridAIConversationManager::RelaxEmotionTargetsForActiveNPCs()
{
    TArray<FName> ActiveNPCIDs = NPCIDs;
    if (ActiveNPCIDs.Num() == 0)
    {
        ActiveNPCIDs.Add(FName(TEXT("NPC")));
    }

    for (const FName& NPCID : ActiveNPCIDs)
    {
        FEmotionTransitionState& State = EmotionTransitionStateByNPC.FindOrAdd(NPCID);
        const FName EmotionToRelax = State.TargetEmotion != NAME_None && State.TargetEmotion != TEXT("neutral")
            ? State.TargetEmotion
            : State.CurrentEmotion;

        State.TargetEmotion = EmotionToRelax == NAME_None ? FName(TEXT("neutral")) : EmotionToRelax;
        State.TargetMagnitude = EmotionRelaxTargetMagnitude;
        State.TransitionElapsedSeconds = 0.0f;
        State.bHasAssignedTarget = true;
        State.bIsRelaxing = true;

        UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager][VisualRelax] npc=%s family=%s from=%.2f to_floor=%.2f"),
            *NPCID.ToString(),
            *State.TargetEmotion.ToString(),
            State.CurrentMagnitude,
            State.TargetMagnitude);
    }
}

void UOffgridAIConversationManager::TickEmotionTransitions(float DeltaTimeSeconds)
{
    if (!Orchestrator)
    {
        return;
    }

    const float SafeDelta = FMath::Max(0.0f, DeltaTimeSeconds);
    TArray<FName> ActiveNPCIDs = NPCIDs;
    if (ActiveNPCIDs.Num() == 0)
    {
        ActiveNPCIDs.Add(FName(TEXT("NPC")));
    }

    for (const FName& NPCID : ActiveNPCIDs)
    {
        // ConversationManager remains the sole owner/driver of NPC emotional state.
        // This tick eases current state toward the assigned target and continuously
        // pushes the live sample through Orchestrator -> LineCoach -> FaceDriver.
        FEmotionTransitionState& State = EmotionTransitionStateByNPC.FindOrAdd(NPCID);

        if (!State.bHasAssignedTarget)
        {
            Orchestrator->DriveNPCEmotionExpression(NPCID, TEXT("neutral"), 0.0f);
            continue;
        }

        State.TransitionElapsedSeconds += SafeDelta;
        const float DesiredSpeed = State.bIsRelaxing ? EmotionRelaxSpeedPerSecond : EmotionApproachSpeedPerSecond;
        const float Speed = EasedEmotionTransitionSpeed(State.TransitionElapsedSeconds, DesiredSpeed);

        if (State.CurrentEmotion != State.TargetEmotion)
        {
            // Switch the driven family immediately, but seed it from the previous
            // magnitude and ease into full transition speed. The expression resolver
            // handles the visual family blend; CM owns only the target state.
            State.CurrentEmotion = State.TargetEmotion;
        }

        State.CurrentMagnitude = FMath::FInterpConstantTo(
            State.CurrentMagnitude,
            State.TargetMagnitude,
            SafeDelta,
            Speed);

        const float Magnitude = FMath::Clamp(State.CurrentMagnitude, 0.0f, 1.0f);
        const FName EmotionToDrive = Magnitude <= EmotionMagnitudeEpsilon
            ? FName(TEXT("neutral"))
            : State.CurrentEmotion;

        Orchestrator->DriveNPCEmotionExpression(NPCID, EmotionToDrive, Magnitude);
    }
}

TArray<FName> UOffgridAIConversationManager::GetSupportedPerformanceEmotionNamesForNPC(FName NPCID, bool bIncludeNeutral) const
{
    TArray<FName> Result;

    TArray<FName> ConfiguredNames;
    if (Orchestrator && Orchestrator->GetNPCSupportedEmotionNames(NPCID, ConfiguredNames) && ConfiguredNames.Num() > 0)
    {
        for (const FName& EmotionName : ConfiguredNames)
        {
            AddNormalizedEmotionNameUnique(Result, EmotionName, bIncludeNeutral);
        }
    }

    if (Result.Num() == 0)
    {
        for (const FName& EmotionName : DefaultRuntimePerformanceEmotions())
        {
            AddNormalizedEmotionNameUnique(Result, EmotionName, bIncludeNeutral);
        }
    }

    return Result;
}

FString UOffgridAIConversationManager::EmotionAdjectiveForPrompt(FName Emotion) const
{
    const FString EmotionKey = Emotion.ToString().ToLower();
    if (EmotionKey == TEXT("joy")) return TEXT("happy");
    if (EmotionKey == TEXT("anger")) return TEXT("angry");
    if (EmotionKey == TEXT("sadness")) return TEXT("sad");
    if (EmotionKey == TEXT("fear")) return TEXT("afraid");
    if (EmotionKey == TEXT("surprise")) return TEXT("surprised");
    if (EmotionKey == TEXT("disgust")) return TEXT("disgusted");
    if (EmotionKey == TEXT("neutral")) return TEXT("neutral");
    return EmotionKey;
}

FString UOffgridAIConversationManager::TTSEmotionInstructionPhrase(FName Emotion, float Magnitude) const
{
    const FName NormalizedEmotion = NormalizeEmotionNameToRuntimeKey(Emotion);
    if (NormalizedEmotion == NAME_None || NormalizedEmotion == TEXT("neutral"))
    {
        return TEXT("neutral");
    }

    const float ClampedMagnitude = FMath::Clamp(Magnitude, 0.0f, 1.0f);
    const FString EmotionWord = EmotionAdjectiveForPrompt(NormalizedEmotion);

    // Designed for a 0.1 default escalation step: the ninth consecutive hit reaches
    // the rare top tier and is allowed to sound extreme.
    const TCHAR* Descriptor = TEXT("slightly");
    if (ClampedMagnitude >= 0.90f)
    {
        Descriptor = TEXT("extremely");
    }
    else if (ClampedMagnitude >= 0.70f)
    {
        Descriptor = TEXT("very");
    }
    else if (ClampedMagnitude >= 0.45f)
    {
        Descriptor = TEXT("moderately");
    }
    else if (ClampedMagnitude >= 0.25f)
    {
        Descriptor = TEXT("mildly");
    }

    return FString::Printf(TEXT("%s %s"), Descriptor, *EmotionWord);
}


FString UOffgridAIConversationManager::GetSignificantPersistentEmotionStatePrompt() const
{
    FString Prompt;
    for (const FName& NPCID : NPCIDs)
    {
        const FOffgridAIPADSState* PADS = PADSStateByNPC.Find(NPCID);
        const FEmotionTransitionState* Emotion = EmotionTransitionStateByNPC.Find(NPCID);
        if (!PADS || !Emotion)
        {
            continue;
        }

        Prompt += FString::Printf(TEXT("- %s: %s (mood %.2f, intensity %.2f, confidence %.2f, composure %.2f).\n"),
            *NPCID.ToString(),
            *Emotion->TargetLabel,
            PADS->Pleasure,
            PADS->Activation,
            PADS->Dominance,
            PADS->Stability);
    }
    return Prompt;
}

FName UOffgridAIConversationManager::MakeUniqueLineID(int32 LineIndex)
{
    const int32 Serial = ConversationLineSerial++;
    return FName(*FString::Printf(TEXT("Line_%s_%06d_%02d"),
        *ConversationID.ToString(EGuidFormats::Digits),
        Serial,
        LineIndex));
}


namespace
{
    FString OffgridExtractBaseEmotionToken(const FString& RawEmotion)
    {
        FString Value = RawEmotion.TrimStartAndEnd().ToLower();
        Value.ReplaceInline(TEXT("\""), TEXT(""));
        Value.ReplaceInline(TEXT("'"), TEXT(""));

        int32 EqIndex = INDEX_NONE;
        if (Value.FindChar(TEXT('='), EqIndex))
        {
            Value = Value.RightChop(EqIndex + 1).TrimStartAndEnd();
        }

        int32 SepIndex = INDEX_NONE;
        const TCHAR* Separators = TEXT(",;|/");
        for (const TCHAR* It = Separators; *It; ++It)
        {
            int32 Candidate = INDEX_NONE;
            if (Value.FindChar(*It, Candidate) && (SepIndex == INDEX_NONE || Candidate < SepIndex))
            {
                SepIndex = Candidate;
            }
        }
        if (SepIndex != INDEX_NONE)
        {
            Value = Value.Left(SepIndex).TrimStartAndEnd();
        }

        int32 SpaceIndex = INDEX_NONE;
        if (Value.FindChar(TEXT(' '), SpaceIndex))
        {
            Value = Value.Left(SpaceIndex).TrimStartAndEnd();
        }

        Value.ReplaceInline(TEXT("."), TEXT(""));
        Value.ReplaceInline(TEXT(":"), TEXT(""));

        return NormalizeEmotionLabelToRuntimeKey(Value);
    }


    bool OffgridIsCanonicalV1Emotion(const FString& BaseEmotion)
    {
        return BaseEmotion.Equals(TEXT("neutral"), ESearchCase::IgnoreCase)
            || BaseEmotion.Equals(TEXT("joy"), ESearchCase::IgnoreCase)
            || BaseEmotion.Equals(TEXT("anger"), ESearchCase::IgnoreCase)
            || BaseEmotion.Equals(TEXT("sadness"), ESearchCase::IgnoreCase)
            || BaseEmotion.Equals(TEXT("fear"), ESearchCase::IgnoreCase)
            || BaseEmotion.Equals(TEXT("surprise"), ESearchCase::IgnoreCase)
            || BaseEmotion.Equals(TEXT("disgust"), ESearchCase::IgnoreCase);
    }

    FString OffgridExtractStrictBaseEmotionToken(const FString& RawEmotion)
    {
        const FString Candidate = OffgridExtractBaseEmotionToken(RawEmotion);
        return OffgridIsCanonicalV1Emotion(Candidate) ? Candidate : FString();
    }

    bool OffgridIsSentenceBoundary(TCHAR Ch)
    {
        return Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?');
    }

    FString OffgridApplySentenceStyleCapitalization(FString Dialogue)
    {
        Dialogue.TrimStartAndEndInline();

        bool bCapitalizeNextAlpha = true;
        for (int32 Index = 0; Index < Dialogue.Len(); ++Index)
        {
            const TCHAR Ch = Dialogue[Index];

            if (FChar::IsAlpha(Ch))
            {
                const bool bStandaloneI = (Ch == TEXT('i'))
                    && (Index == 0 || !FChar::IsAlpha(Dialogue[Index - 1]))
                    && (Index + 1 >= Dialogue.Len() || !FChar::IsAlpha(Dialogue[Index + 1]));

                const bool bIContraction = (Ch == TEXT('i'))
                    && (Index == 0 || !FChar::IsAlpha(Dialogue[Index - 1]))
                    && (Index + 1 < Dialogue.Len() && Dialogue[Index + 1] == TEXT('\''));

                if (bCapitalizeNextAlpha || bStandaloneI || bIContraction)
                {
                    Dialogue[Index] = FChar::ToUpper(Ch);
                }

                bCapitalizeNextAlpha = false;
                continue;
            }

            if (OffgridIsSentenceBoundary(Ch))
            {
                bCapitalizeNextAlpha = true;
                continue;
            }

            if (!FChar::IsWhitespace(Ch) && Ch != TCHAR('"') && Ch != TCHAR('\'') && Ch != TCHAR(')') && Ch != TCHAR(']'))
            {
                if (bCapitalizeNextAlpha && Ch != TEXT('(') && Ch != TEXT('['))
                {
                    bCapitalizeNextAlpha = false;
                }
            }
        }

        return Dialogue;
    }


    FString OffgridSanitizeNPCDialogueString(FString Dialogue, FName NPCID)
    {
        Dialogue.TrimStartAndEndInline();

        auto StripLeadingSeparatorRun = [&Dialogue]()
        {
            bool bChanged = true;
            while (bChanged && Dialogue.Len() > 0)
            {
                bChanged = false;
                Dialogue.TrimStartInline();
                while (Dialogue.StartsWith(TEXT(":")) || Dialogue.StartsWith(TEXT("-")) || Dialogue.StartsWith(TEXT("=")))
                {
                    Dialogue.RightChopInline(1);
                    Dialogue.TrimStartInline();
                    bChanged = true;
                }
            }
        };

        StripLeadingSeparatorRun();

        // Defensive cleanup for LLMs that obey the prose prompt literally inside
        // the JSON dialogue field, e.g. "Alfie: hello" or repeated repair artifacts
        // like ": hello". Speaker identity already lives in npc_id.
        TArray<FString> SpeakerPrefixes;
        if (NPCID != NAME_None)
        {
            SpeakerPrefixes.Add(NPCID.ToString());
        }
        SpeakerPrefixes.Add(TEXT("Alfie"));
        SpeakerPrefixes.Add(TEXT("NPC"));
        SpeakerPrefixes.Add(TEXT("Assistant"));
        SpeakerPrefixes.Add(TEXT("Dialogue"));

        bool bStrippedSpeaker = true;
        while (bStrippedSpeaker)
        {
            bStrippedSpeaker = false;
            Dialogue.TrimStartInline();
            for (const FString& Prefix : SpeakerPrefixes)
            {
                if (Prefix.IsEmpty())
                {
                    continue;
                }

                const bool bHasColonPrefix = Dialogue.StartsWith(Prefix + TEXT(":"), ESearchCase::IgnoreCase);
                const bool bHasEqualsPrefix = Dialogue.StartsWith(Prefix + TEXT("="), ESearchCase::IgnoreCase);
                if (bHasColonPrefix || bHasEqualsPrefix)
                {
                    Dialogue.RightChopInline(Prefix.Len() + 1);
                    StripLeadingSeparatorRun();
                    bStrippedSpeaker = true;
                    break;
                }
            }
        }

        return OffgridApplySentenceStyleCapitalization(Dialogue.TrimStartAndEnd());
    }

    bool OffgridEmotionAllowedByBaseToken(const FString& RawEmotion, const TArray<FString>& AllowedEmotions)
    {
        const FString Base = OffgridExtractBaseEmotionToken(RawEmotion);

        if (AllowedEmotions.Num() > 0)
        {
            for (const FString& Allowed : AllowedEmotions)
            {
                if (OffgridExtractBaseEmotionToken(Allowed).Equals(Base, ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
            return false;
        }

        // No configured asset/schema found: use the neutral-inclusive runtime default.
        return OffgridIsCanonicalV1Emotion(Base);
    }

}

bool UOffgridAIConversationManager::TryBuildSingleLineRequestFromObject(
    const TSharedPtr<FJsonObject>& EntryObject,
    int32 LineIndex,
    TArray<FOffgridAILinePerformanceRequest>& OutLineRequests)
{
    if (!EntryObject.IsValid())
    {
        return false;
    }

    FString NPCIDString;
    FString DialogueString;
    FString EmotionString;

    EntryObject->TryGetStringField(TEXT("npc_id"), NPCIDString);
    if (NPCIDString.IsEmpty())
    {
        if (NPCIDs.Num() == 1)
        {
            NPCIDString = NPCIDs[0].ToString();
        }
        else
        {
            UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON entry %d missing npc_id and conversation has %d NPCs"), LineIndex, NPCIDs.Num());
            return false;
        }
    }

    if (!EntryObject->TryGetStringField(TEXT("dialogue"), DialogueString))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON entry %d missing dialogue"), LineIndex);
        return false;
    }

    if (!EntryObject->TryGetStringField(TEXT("emotion"), EmotionString))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON entry %d missing emotion"), LineIndex);
        return false;
    }

    const FName NPCID(*NPCIDString);

    TArray<FString> AllowedEmotions;
    for (const FName& AllowedEmotion : GetSupportedPerformanceEmotionNamesForNPC(NPCID, true))
    {
        AllowedEmotions.AddUnique(AllowedEmotion.ToString());
    }
    if (AllowedEmotions.Num() == 0)
    {
        ExtractAllowedEmotionsFromSchema(ConversationPromptAsset, AllowedEmotions);
    }

    if (!OffgridEmotionAllowedByBaseToken(EmotionString, AllowedEmotions))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON entry %d invalid emotion=%s base=%s allowed=[%s]"),
            LineIndex,
            *EmotionString,
            *OffgridExtractBaseEmotionToken(EmotionString),
            *FString::Join(AllowedEmotions, TEXT(", ")));
        return false;
    }

    const FString PreservedEmotionString = EmotionString.TrimStartAndEnd();
    const FString BaseEmotionString = OffgridExtractBaseEmotionToken(PreservedEmotionString);
    const FName Emotion(*BaseEmotionString);

    UE_LOG(LogOffgridAI, Log, TEXT("[ConversationManager][Emotion] line=%d raw=%s base=%s preserved=%s"),
        LineIndex,
        *EmotionString,
        *BaseEmotionString,
        *PreservedEmotionString);

    FName ResolvedVoiceID = NAME_None;
    if (Orchestrator && !Orchestrator->TryGetVoiceIDForNPC(NPCID, ResolvedVoiceID))
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON entry %d unknown npc_id=%s or missing voice mapping"), LineIndex, *NPCIDString);
        return false;
    }

    FOffgridAILinePerformanceRequest LineRequest;
    LineRequest.ConversationID = ConversationID;
    LineRequest.LineID = MakeUniqueLineID(LineIndex);
    LineRequest.NPCID = NPCID;
    LineRequest.VoiceID = ResolvedVoiceID;
    LineRequest.Dialogue = FText::FromString(OffgridSanitizeNPCDialogueString(DialogueString, NPCID));
    LineRequest.Emotion = Emotion;
    LineRequest.EmotionMagnitude = 0.0f;

    OutLineRequests.Add(LineRequest);
    return true;
}


bool UOffgridAIConversationManager::TryBuildLineRequestsFromJSON(
    const FString& JSONPayload,
    TArray<FOffgridAILinePerformanceRequest>& OutLineRequests)
{
    OutLineRequests.Empty();

    TSharedPtr<FJsonValue> RootValue;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JSONPayload);
    if (!FJsonSerializer::Deserialize(Reader, RootValue) || !RootValue.IsValid())
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON deserialize failed payload=%s"), *JSONPayload);
        return false;
    }

    if (RootValue->Type == EJson::Object)
    {
        return TryBuildSingleLineRequestFromObject(RootValue->AsObject(), 0, OutLineRequests);
    }

    if (RootValue->Type != EJson::Array)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON root was neither array nor object"));
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>& RootArray = RootValue->AsArray();
    if (RootArray.Num() == 0)
    {
        UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON root array was empty"));
        return false;
    }

    int32 LineIndex = 0;
    for (const TSharedPtr<FJsonValue>& EntryValue : RootArray)
    {
        if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object)
        {
            UE_LOG(LogOffgridAI, Warning, TEXT("[ConversationManager] JSON entry %d was not an object"), LineIndex);
            return false;
        }

        if (!TryBuildSingleLineRequestFromObject(EntryValue->AsObject(), LineIndex, OutLineRequests))
        {
            return false;
        }
        ++LineIndex;
    }

    return OutLineRequests.Num() > 0;
}
