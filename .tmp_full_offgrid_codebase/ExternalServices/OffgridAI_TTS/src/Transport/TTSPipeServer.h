#pragma once

class ITTSBackend;

class TTSPipeServer
{
public:
    explicit TTSPipeServer(ITTSBackend& InBackend);
    bool Run();

private:
    ITTSBackend& Backend;
};
