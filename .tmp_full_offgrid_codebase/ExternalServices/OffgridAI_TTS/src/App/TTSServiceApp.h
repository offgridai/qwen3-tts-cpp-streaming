#pragma once

#include <memory>

class ITTSBackend;
class TTSPipeServer;

class TTSServiceApp
{
public:
    TTSServiceApp();
    ~TTSServiceApp();

    int Run();

private:
    std::unique_ptr<ITTSBackend> Backend;
    std::unique_ptr<TTSPipeServer> PipeServer;
};
