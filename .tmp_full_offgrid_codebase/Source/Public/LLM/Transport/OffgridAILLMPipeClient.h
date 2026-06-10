#pragma once

#include "CoreMinimal.h"

class FOffgridAILLMPipeClient
{
public:
    FOffgridAILLMPipeClient();
    ~FOffgridAILLMPipeClient();

    bool Connect(const FString& PipeName);
    void Disconnect();
    bool IsConnected() const;
    bool SendBytes(const TArray<uint8>& Bytes);
    bool ReceiveBytes(TArray<uint8>& OutBytes);

private:
    void* PipeHandle;
};
