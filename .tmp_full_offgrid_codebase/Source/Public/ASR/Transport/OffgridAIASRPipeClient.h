#pragma once

#include "CoreMinimal.h"

class OFFGRIDAI_API FOffgridAIASRPipeClient
{
public:
    FOffgridAIASRPipeClient();
    ~FOffgridAIASRPipeClient();

    bool Connect(const FString& PipeName);
    void Disconnect();
    bool IsConnected() const;

    bool SendBytes(const TArray<uint8>& Bytes);
    bool ReceiveBytes(TArray<uint8>& OutBytes);

private:
    void* PipeHandle;
};
