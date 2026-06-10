#pragma once

#include "Protocol/TTSProtocol.h"

class ITTSBackend
{
public:
    virtual ~ITTSBackend() = default;

    virtual bool Startup(const TTSRequest& Request, TTSResponse& OutResponse) = 0;
    virtual bool BeginSynthesis(const TTSRequest& Request, TTSResponse& OutResponse) = 0;
    virtual bool Cancel(const TTSRequest& Request, TTSResponse& OutResponse) = 0;
    virtual bool PollEvent(const TTSRequest& Request, TTSResponse& OutResponse) = 0;
    virtual bool Health(const TTSRequest& Request, TTSResponse& OutResponse) = 0;
    virtual bool Shutdown(const TTSRequest& Request, TTSResponse& OutResponse) = 0;
};
