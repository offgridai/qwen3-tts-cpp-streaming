#pragma once

#include "Backend/ITTSBackend.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace qwen3_tts { class Qwen3TTS; }

class Qwen3TTSBackend final : public ITTSBackend
{
public:
    Qwen3TTSBackend();
    ~Qwen3TTSBackend() override;

    bool Startup(const TTSRequest& Request, TTSResponse& OutResponse) override;
    bool BeginSynthesis(const TTSRequest& Request, TTSResponse& OutResponse) override;
    bool Cancel(const TTSRequest& Request, TTSResponse& OutResponse) override;
    bool PollEvent(const TTSRequest& Request, TTSResponse& OutResponse) override;
    bool Health(const TTSRequest& Request, TTSResponse& OutResponse) override;
    bool Shutdown(const TTSRequest& Request, TTSResponse& OutResponse) override;

private:
    void StopActiveSynthesis();
    void SynthesisThreadMain(TTSRequest Request, uint64_t Generation);
    void EnqueueEvent(TTSEvent&& Event);
    void EnqueueCompleted(TTSEvent&& Event);
    void EnqueueError(const TTSRequest& Request, const std::string& ErrorMessage);
    bool LoadSpeakerEmbedding(const std::string& Path, std::string& OutError);
    bool RunStartupPrewarmLocked(const TTSRequest& Request, std::string& OutError);
    static bool LoadSpeakerEmbeddingFile(const std::string& Path, std::vector<float>& OutEmbedding, std::string& OutError);
    static std::string ResolveSpeakerEmbeddingPathForRequest(const TTSRequest& Request);
    static std::string ResolvePath(const std::string& Path, const std::string& WorkingDirectory);
    static std::vector<uint8_t> FloatMonoToPCM16(const float* Samples, size_t NumSamples);

private:
    std::unique_ptr<qwen3_tts::Qwen3TTS> Runtime;
    std::vector<float> SpeakerEmbedding;
    std::string LoadedModelDirectory;
    std::string LoadedModelIdentifier;
    std::string LoadedSpeakerEmbeddingPath;
    std::string LastError;
    bool bStartupPrewarmed = false;

    std::mutex RuntimeMutex;
    std::mutex EventMutex;
    std::queue<TTSEvent> PendingEvents;
    bool bHasDeferredCompletedEvent = false;
    TTSEvent DeferredCompletedEvent;

    std::thread ActiveThread;
    std::atomic<bool> bCancelRequested{false};
    std::atomic<bool> bLoaded{false};
    std::atomic<bool> bShuttingDown{false};
    std::atomic<uint64_t> ActiveGeneration{0};
};
