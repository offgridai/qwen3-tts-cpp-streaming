#pragma once

#include "CoreMinimal.h"

enum class EOffgridAIASROp : uint8
{
    Startup,
    StartUtterance,
    PushAudio,
    FinalizeUtterance,
    CancelUtterance,
    GetPartial,
    Health,
    Shutdown
};

struct FOffgridAIASRRequest
{
    EOffgridAIASROp Op = EOffgridAIASROp::Health;
    FString RequestId;
    TArray<uint8> Payload;
    int32 SampleRateHz = 0;
    int32 NumChannels = 0;
    FString SampleFormat;

    // Startup config for the real sherpa-onnx backend.
    FString ActiveModelDirectory;
    FString Provider;
    FString DecodingMethod = TEXT("modified_beam_search");
    int32 NumThreads = 1;
    int32 MaxActivePaths = 4;
    int32 ExpectedSampleRate = 16000;
    int32 FeatureDim = 80;
    bool bModelDebug = false;
    int32 FinalizeSilencePaddingMs = 300;
    int32 FinalizeSettleDelayMs = 30;
};

struct FOffgridAIASRResponse
{
    bool bOk = false;
    FString RequestId;
    FString Transcript;
    FString ErrorMessage;
};
