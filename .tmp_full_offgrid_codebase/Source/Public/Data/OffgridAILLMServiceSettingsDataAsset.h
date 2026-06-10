#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "OffgridAILLMServiceSettingsDataAsset.generated.h"

UENUM(BlueprintType)
enum class EOffgridAILLMBackendKind : uint8
{
    None,
    ExternalProcess
};

UENUM(BlueprintType)
enum class EOffgridAILLMMode : uint8
{
    Passthrough,
    LlamaCpp
};

UCLASS(BlueprintType)
class OFFGRIDAI_API UOffgridAILLMServiceSettingsDataAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    /*
     * Implementation
     *
     * Selects the LLM backend and, for local-process backends, describes how to launch it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Implementation")
    EOffgridAILLMBackendKind BackendKind = EOffgridAILLMBackendKind::None;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Implementation")
    FString ServiceExecutablePath;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Implementation")
    FString ServiceWorkingDirectory;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Implementation")
    TArray<FString> LaunchArguments;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Implementation")
    EOffgridAILLMMode Mode = EOffgridAILLMMode::Passthrough;

    /*
     * Model
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Model")
    FString ModelPath;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Model", meta = (ClampMin = "1"))
    int32 ContextWindowTokens = 4096;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Model", meta = (ClampMin = "1"))
    int32 MaxOutputTokens = 128;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Model", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float Temperature = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Model")
    int32 GPULayers = 999;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Model", meta = (ClampMin = "1"))
    int32 ParallelSlots = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Model", meta = (ClampMin = "1", ClampMax = "8000"))
    int32 MaxDialogueCharacters = 500;

    /*
     * Transport
     *
     * Named pipe is the currently implemented LLM service boundary.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Transport")
    FString PipeName = TEXT("\\\\.\\pipe\\OffgridAI_LLM");

    /*
     * Stub
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Stub", meta = (ClampMin = "0.0", ClampMax = "10.0"))
    float StubCompletionDelaySeconds = 0.02f;

    /*
     * Debug
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LLM|Debug")
    bool bVerboseLogging = false;
};
