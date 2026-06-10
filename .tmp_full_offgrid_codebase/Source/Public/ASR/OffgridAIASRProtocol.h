#pragma once

#include "CoreMinimal.h"

#include "ASR/OffgridAIASRPipeProtocol.h"

namespace OffgridAIASRProtocol
{
    OFFGRIDAI_API FString OpToString(EOffgridAIASROp Op);
    OFFGRIDAI_API bool StringToOp(const FString& Value, EOffgridAIASROp& OutOp);

    OFFGRIDAI_API bool SerializeRequest(const FOffgridAIASRRequest& Request, TArray<uint8>& OutBytes);
    OFFGRIDAI_API bool DeserializeRequest(const TArray<uint8>& Bytes, FOffgridAIASRRequest& OutRequest);

    OFFGRIDAI_API bool SerializeResponse(const FOffgridAIASRResponse& Response, TArray<uint8>& OutBytes);
    OFFGRIDAI_API bool DeserializeResponse(const TArray<uint8>& Bytes, FOffgridAIASRResponse& OutResponse);
}
