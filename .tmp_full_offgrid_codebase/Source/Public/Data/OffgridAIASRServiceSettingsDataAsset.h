#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "OffgridAIASRServiceSettingsDataAsset.generated.h"

UENUM(BlueprintType)
enum class EOffgridAIASRBackendKind : uint8
{
    None,
    SherpaOnnx
};

// Shared PCM format enum used by service settings that exchange raw audio.
// ASR currently submits pcm16 at runtime because BoomOperator provides pcm16 data.
UENUM(BlueprintType)
enum class EOffgridAIAudioSampleFormat : uint8
{
    PCM16
};

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAIASRServiceSettingsDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    /*
     * Implementation
     *
     * Selects the ASR backend and, for local-process backends, describes how to launch it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|Implementation")
    EOffgridAIASRBackendKind BackendKind = EOffgridAIASRBackendKind::None;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|Implementation")
    FString ServiceExecutablePath;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|Implementation")
    FString ServiceWorkingDirectory;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|Implementation")
    TArray<FString> LaunchArguments;

    /*
     * Transport
     *
     * ASR currently uses a named-pipe service boundary.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|Transport")
    FString PipeName = TEXT(R"(\\.\pipe\OffgridAI_ASR)");

    /*
     * Sherpa-ONNX Model
     *
     * If ActiveModelDirectory is empty, the service may fall back to ServiceWorkingDirectory.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|SherpaOnnx|Model")
    FString ActiveModelDirectory;

    /*
     * Sherpa-ONNX Runtime
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|SherpaOnnx|Runtime")
    FString Provider = TEXT("cpu");

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|SherpaOnnx|Runtime", meta = (ClampMin = "1"))
    int32 NumThreads = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|SherpaOnnx|Runtime")
    FString DecodingMethod = TEXT("modified_beam_search");

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|SherpaOnnx|Runtime", meta = (ClampMin = "1"))
    int32 MaxActivePaths = 4;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|SherpaOnnx|Runtime", meta = (ClampMin = "1"))
    int32 ModelSampleRate = 16000;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|SherpaOnnx|Runtime", meta = (ClampMin = "1"))
    int32 FeatureDim = 80;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|SherpaOnnx|Runtime")
    bool bModelDebug = false;

    /*
     * Finalization
     *
     * These values are sent to the ASR service when the utterance is finalized.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|Finalization", meta = (ClampMin = "0", ClampMax = "2000"))
    int32 FinalizeSilencePaddingMs = 300;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ASR|Finalization", meta = (ClampMin = "0", ClampMax = "1000"))
    int32 FinalizeSettleDelayMs = 30;
};
