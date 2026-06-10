#include "App/TTSServiceApp.h"

#include "Backend/Qwen3TTSBackend.h"
#include "Transport/TTSPipeServer.h"

#include <iostream>

TTSServiceApp::TTSServiceApp()
    : Backend(std::make_unique<Qwen3TTSBackend>())
    , PipeServer(std::make_unique<TTSPipeServer>(*Backend))
{
}

TTSServiceApp::~TTSServiceApp() = default;

int TTSServiceApp::Run()
{
    std::cout << "[OffgridAI_TTS] boot complete, entering pipe server loop." << std::endl;
    return PipeServer->Run() ? 0 : 1;
}
