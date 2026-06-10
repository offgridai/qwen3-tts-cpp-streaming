#include "TTS/Transport/OffgridAITTSPipeClient.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

FOffgridAITTSPipeClient::FOffgridAITTSPipeClient()
    : PipeHandle(nullptr)
{
}

FOffgridAITTSPipeClient::~FOffgridAITTSPipeClient()
{
    Disconnect();
}

bool FOffgridAITTSPipeClient::Connect(const FString& PipeName)
{
#if PLATFORM_WINDOWS
    Disconnect();
    HANDLE Handle = CreateFileW(*PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (Handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    PipeHandle = Handle;
    return true;
#else
    return false;
#endif
}

void FOffgridAITTSPipeClient::Disconnect()
{
#if PLATFORM_WINDOWS
    if (PipeHandle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(PipeHandle));
        PipeHandle = nullptr;
    }
#endif
}

bool FOffgridAITTSPipeClient::IsConnected() const
{
    return PipeHandle != nullptr;
}

bool FOffgridAITTSPipeClient::SendBytes(const TArray<uint8>& Bytes)
{
#if PLATFORM_WINDOWS
    if (!PipeHandle)
    {
        return false;
    }

    const uint32 PayloadSize = static_cast<uint32>(Bytes.Num());
    DWORD Written = 0;
    if (!WriteFile(static_cast<HANDLE>(PipeHandle), &PayloadSize, sizeof(PayloadSize), &Written, nullptr) || Written != sizeof(PayloadSize))
    {
        return false;
    }

    if (PayloadSize == 0)
    {
        return true;
    }

    if (!WriteFile(static_cast<HANDLE>(PipeHandle), Bytes.GetData(), PayloadSize, &Written, nullptr) || Written != PayloadSize)
    {
        return false;
    }

    return true;
#else
    return false;
#endif
}

bool FOffgridAITTSPipeClient::ReceiveBytes(TArray<uint8>& OutBytes)
{
#if PLATFORM_WINDOWS
    if (!PipeHandle)
    {
        return false;
    }

    uint32 PayloadSize = 0;
    DWORD Read = 0;
    if (!ReadFile(static_cast<HANDLE>(PipeHandle), &PayloadSize, sizeof(PayloadSize), &Read, nullptr) || Read != sizeof(PayloadSize))
    {
        return false;
    }

    OutBytes.SetNumUninitialized(PayloadSize);
    if (PayloadSize == 0)
    {
        return true;
    }

    uint32 TotalRead = 0;
    while (TotalRead < PayloadSize)
    {
        DWORD ChunkRead = 0;
        if (!ReadFile(static_cast<HANDLE>(PipeHandle), OutBytes.GetData() + TotalRead, PayloadSize - TotalRead, &ChunkRead, nullptr) || ChunkRead == 0)
        {
            return false;
        }

        TotalRead += ChunkRead;
    }

    return true;
#else
    return false;
#endif
}
