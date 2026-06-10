#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Core/OffgridAITypes.h"
#include "OffgridAITTSServiceSettingsDataAsset.generated.h"

UENUM(BlueprintType)
enum class EOffgridAITTSBackendKind : uint8
{
    None,
    Qwen3
};

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAITTSServiceSettingsDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    /*
     * Implementation
     *
     * Selects the TTS backend and, for local-process backends, describes how to launch it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Implementation")
    EOffgridAITTSBackendKind BackendKind = EOffgridAITTSBackendKind::None;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Implementation")
    FString ServiceExecutablePath;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Implementation")
    FString ServiceWorkingDirectory;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Implementation")
    TArray<FString> LaunchArguments;

    /*
     * Model
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Model")
    FString ModelDirectory;

    /*
     * Legacy identifier used by older service paths or as a fallback display/model key.
     * Prefer ModelDirectory for current local model routing.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Model")
    FString LegacyModelIdentifier = TEXT("Base");

    /*
     * Voice
     *
     * VoiceEmbeddingPath may point to either:
     * - a directory containing per-NPC voice embeddings, or
     * - a legacy single speaker embedding file.
     *
     * Prefer the directory form for multi-NPC projects.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Voice")
    EOffgridAITTSVoiceMode DefaultVoiceMode = EOffgridAITTSVoiceMode::CustomVoice;

    // Used only for VoiceDesign startup/prewarm requests before a LineCoach-specific
    // identity is available. Runtime line requests override this with LineCoach data.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Voice", meta = (MultiLine = true))
    FString DefaultVoiceDesignInstruction = TEXT("Voice identity:\nnatural adult conversational voice\n\nDelivery:\nnatural, conversational, emotionally balanced");

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Voice")
    FString VoiceEmbeddingPath;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Voice")
    FString ReferenceAudioPath;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Voice", meta = (MultiLine = true))
    FString ReferenceText;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Voice")
    FString Language = TEXT("English");

    /*
     * Runtime
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Runtime")
    bool bUseGPU = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Runtime")
    bool bPrewarmStreaming = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Runtime")
    bool bForwardEmotionToInstruction = false;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Runtime")
    bool bAsyncStreamingDecode = true;

    /*
     * Streaming Decode
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode", meta = (ClampMin = "1"))
    int32 FirstTailWindowFrames = 3;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode", meta = (ClampMin = "1"))
    int32 SteadyTailWindowFrames = 8;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode", meta = (ClampMin = "0"))
    int32 ContextFrames = 4;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode", meta = (ClampMin = "0"))
    int32 FinalContextFrames = 4;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode", meta = (ClampMin = "0"))
    int32 PrewarmFrames = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode", meta = (ClampMin = "0"))
    int32 PrewarmRepeats = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode")
    bool bPrewarmFirstDecode = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode")
    bool bPrewarmSteadyDecode = true;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Streaming Decode")
    bool bPrewarmFinalDecode = true;

    /*
     * Generation
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Generation", meta = (ClampMin = "1"))
    int32 MaxAudioTokens = 4096;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Generation", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float Temperature = 0.9f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Generation", meta = (ClampMin = "0"))
    int32 TopK = 75;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Generation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TopP = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Generation", meta = (ClampMin = "0.0", ClampMax = "5.0"))
    float RepetitionPenalty = 1.05f;

    /*
     * Output
     *
     * TTS currently outputs pcm16 audio.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Output", meta = (ClampMin = "8000", ClampMax = "192000"))
    int32 OutputSampleRateHz = 24000;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Output", meta = (ClampMin = "1", ClampMax = "2"))
    int32 NumChannels = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Output", meta = (ClampMin = "5", ClampMax = "1000"))
    int32 PreferredChunkMs = 40;

    /*
     * Transport
     *
     * Named pipe is the currently implemented TTS service boundary.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Transport")
    FString PipeName = TEXT("\\\\.\\pipe\\OffgridAI_TTS");

    /*
     * Debug
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Debug")
    bool bDumpFirstFrameProfile = false;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TTS|Debug")
    bool bVerboseLogging = false;
};
