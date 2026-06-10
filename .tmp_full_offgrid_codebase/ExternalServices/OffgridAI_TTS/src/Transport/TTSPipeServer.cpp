#include "Transport/TTSPipeServer.h"

#include "Backend/ITTSBackend.h"
#include "Protocol/TTSProtocol.h"

#include <windows.h>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
    constexpr wchar_t PipeName[] = LR"(\\.\pipe\OffgridAI_TTS)";

    bool ReadExact(HANDLE Pipe, void* Buffer, DWORD BytesToRead)
    {
        uint8_t* Out = static_cast<uint8_t*>(Buffer);
        DWORD TotalRead = 0;
        while (TotalRead < BytesToRead)
        {
            DWORD ChunkRead = 0;
            if (!ReadFile(Pipe, Out + TotalRead, BytesToRead - TotalRead, &ChunkRead, nullptr) || ChunkRead == 0)
            {
                return false;
            }
            TotalRead += ChunkRead;
        }
        return true;
    }

    bool WriteExact(HANDLE Pipe, const void* Buffer, DWORD BytesToWrite)
    {
        const uint8_t* In = static_cast<const uint8_t*>(Buffer);
        DWORD TotalWritten = 0;
        while (TotalWritten < BytesToWrite)
        {
            DWORD ChunkWritten = 0;
            if (!WriteFile(Pipe, In + TotalWritten, BytesToWrite - TotalWritten, &ChunkWritten, nullptr) || ChunkWritten == 0)
            {
                return false;
            }
            TotalWritten += ChunkWritten;
        }
        return true;
    }

    bool ReceiveMessage(HANDLE Pipe, std::vector<uint8_t>& OutBytes)
    {
        uint32_t PayloadSize = 0;
        if (!ReadExact(Pipe, &PayloadSize, sizeof(PayloadSize)))
        {
            return false;
        }
        OutBytes.resize(PayloadSize);
        return PayloadSize == 0 ? true : ReadExact(Pipe, OutBytes.data(), PayloadSize);
    }

    bool SendMessage(HANDLE Pipe, const std::vector<uint8_t>& Bytes)
    {
        const uint32_t PayloadSize = static_cast<uint32_t>(Bytes.size());
        if (!WriteExact(Pipe, &PayloadSize, sizeof(PayloadSize)))
        {
            return false;
        }
        return PayloadSize == 0 ? true : WriteExact(Pipe, Bytes.data(), PayloadSize);
    }
}

TTSPipeServer::TTSPipeServer(ITTSBackend& InBackend)
    : Backend(InBackend)
{
}

bool TTSPipeServer::Run()
{
    bool bShouldShutdown = false;
    while (!bShouldShutdown)
    {
        HANDLE Pipe = CreateNamedPipeW(PipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (Pipe == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const BOOL Connected = ConnectNamedPipe(Pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!Connected)
        {
            CloseHandle(Pipe);
            continue;
        }

        while (!bShouldShutdown)
        {
            std::vector<uint8_t> RequestBytes;
            if (!ReceiveMessage(Pipe, RequestBytes))
            {
                break;
            }

            TTSRequest Request;
            TTSResponse Response;
            if (!TTSProtocol::DeserializeRequest(RequestBytes, Request))
            {
                Response.Ok = false;
                Response.ErrorMessage = "Failed to parse request.";
            }
            else
            {
                Response.RequestId = Request.RequestId;
                bool bHandled = true;
                switch (Request.Op)
                {
                case ETTSOp::Startup: bHandled = Backend.Startup(Request, Response); break;
                case ETTSOp::BeginSynthesis: bHandled = Backend.BeginSynthesis(Request, Response); break;
                case ETTSOp::Cancel: bHandled = Backend.Cancel(Request, Response); break;
                case ETTSOp::PollEvent: bHandled = Backend.PollEvent(Request, Response); break;
                case ETTSOp::Health: bHandled = Backend.Health(Request, Response); break;
                case ETTSOp::Shutdown: bHandled = Backend.Shutdown(Request, Response); bShouldShutdown = Response.Ok; break;
                default: Response.Ok = false; Response.ErrorMessage = "Unknown request op."; break;
                }
                if (!bHandled && Response.ErrorMessage.empty())
                {
                    Response.Ok = false;
                    Response.ErrorMessage = "Backend handler returned failure.";
                }
            }

            std::vector<uint8_t> ResponseBytes;
            if (!TTSProtocol::SerializeResponse(Response, ResponseBytes) || !SendMessage(Pipe, ResponseBytes))
            {
                break;
            }
        }

        FlushFileBuffers(Pipe);
        DisconnectNamedPipe(Pipe);
        CloseHandle(Pipe);
    }

    return true;
}
