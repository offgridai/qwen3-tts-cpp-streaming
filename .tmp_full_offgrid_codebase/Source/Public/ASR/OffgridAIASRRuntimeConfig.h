#pragma once

#include "CoreMinimal.h"

struct FOffgridAIASRRuntimeConfig
{
    FString ActiveModelDirectory;
    FString Provider = TEXT("cpu");
    FString DecodingMethod = TEXT("modified_beam_search");
    int32 NumThreads = 1;
    int32 MaxActivePaths = 4;
    int32 ExpectedSampleRate = 16000;
    int32 FeatureDim = 80;
    bool bModelDebug = false;
    int32 FinalizeSilencePaddingMs = 300;
    int32 FinalizeSettleDelayMs = 30;

    bool IsValid(FString& OutError) const
    {
        if (ActiveModelDirectory.IsEmpty())
        {
            OutError = TEXT("ASR ActiveModelDirectory is empty.");
            return false;
        }

        if (ExpectedSampleRate <= 0)
        {
            OutError = TEXT("ASR ExpectedSampleRate must be > 0.");
            return false;
        }

        if (FeatureDim <= 0)
        {
            OutError = TEXT("ASR FeatureDim must be > 0.");
            return false;
        }

        if (NumThreads <= 0)
        {
            OutError = TEXT("ASR NumThreads must be > 0.");
            return false;
        }

        if (MaxActivePaths <= 0)
        {
            OutError = TEXT("ASR MaxActivePaths must be > 0.");
            return false;
        }

        return true;
    }
};
