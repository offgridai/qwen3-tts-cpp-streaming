#include "offgrid_tts/Qwen3StreamingTts.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string Quote(const std::string& s) {
    return "\"" + s + "\"";
}

bool ExistsFile(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

bool ExistsDir(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_directory(p, ec);
}

fs::path Abs(const fs::path& p) {
    std::error_code ec;
    return fs::absolute(p, ec);
}

fs::path FindEngineExecutable() {
    const std::vector<fs::path> candidates = {
        "third_party/qwen3-tts-cpp/build/qwen3-tts-cli.exe",
        "third_party/qwen3-tts-cpp/build/Release/qwen3-tts-cli.exe",
        "third_party/qwen3-tts-cpp/build/Debug/qwen3-tts-cli.exe",
        "third_party/qwen3-tts-cpp/build/bin/qwen3-tts-cli.exe",
        "third_party/qwen3-tts-cpp/build/bin/Release/qwen3-tts-cli.exe",
        "third_party/qwen3-tts-cpp/qwen3-tts-cli.exe",
        "build/qwen3-tts-cli.exe",
        "build/Release/qwen3-tts-cli.exe",
    };

    for (const auto& candidate : candidates) {
        if (ExistsFile(candidate)) {
            return Abs(candidate);
        }
    }

    return {};
}

fs::path EngineRootFromExe(const fs::path& exe) {
    // Handles:
    //   third_party/qwen3-tts-cpp/build/qwen3-tts-cli.exe
    //   third_party/qwen3-tts-cpp/build/Release/qwen3-tts-cli.exe
    fs::path p = exe.parent_path();
    if (p.filename() == "Release" || p.filename() == "Debug") {
        p = p.parent_path();
    }
    if (p.filename() == "build") {
        p = p.parent_path();
    }
    return p;
}

fs::path ResolveDirNearEngineOrCwd(const fs::path& input, const fs::path& engine_root) {
    if (input.is_absolute()) {
        return input;
    }

    const fs::path cwd_candidate = Abs(input);
    if (ExistsDir(cwd_candidate)) {
        return cwd_candidate;
    }

    const fs::path engine_candidate = Abs(engine_root / input);
    if (ExistsDir(engine_candidate)) {
        return engine_candidate;
    }

    return cwd_candidate;
}

fs::path ResolveFileNearEngineOrCwd(const fs::path& input, const fs::path& engine_root) {
    if (input.is_absolute()) {
        return input;
    }

    const fs::path cwd_candidate = Abs(input);
    if (ExistsFile(cwd_candidate)) {
        return cwd_candidate;
    }

    const fs::path engine_candidate = Abs(engine_root / input);
    if (ExistsFile(engine_candidate)) {
        return engine_candidate;
    }

    return cwd_candidate;
}

std::string NormalizeModelName(std::string model_identifier) {
    if (model_identifier.empty()) {
        return {};
    }
    const fs::path p(model_identifier);
    model_identifier = p.filename().string();
    if (model_identifier.size() < 5 || model_identifier.substr(model_identifier.size() - 5) != ".gguf") {
        model_identifier += ".gguf";
    }
    return model_identifier;
}

} // namespace

struct Qwen3StreamingTts::Impl {
    std::string model_dir = "models";
    std::string speaker_embedding = "reference/alfie_speaker.json";
};

Qwen3StreamingTts::Qwen3StreamingTts()
    : impl_(new Impl()) {}

Qwen3StreamingTts::~Qwen3StreamingTts() {
    delete impl_;
}

bool Qwen3StreamingTts::load(const std::string& model_dir) {
    impl_->model_dir = model_dir;
    return true;
}

bool Qwen3StreamingTts::load_speaker_embedding(const std::string& path) {
    impl_->speaker_embedding = path;
    return true;
}

bool Qwen3StreamingTts::synthesize_streaming(
    const std::string& text,
    const TtsStreamOptions& options,
    TtsChunkCallback /*on_chunk*/)
{
    const fs::path engine = FindEngineExecutable();
    if (engine.empty()) {
        std::cerr
            << "Engine executable not found.\n"
            << "Build the vendored engine first:\n"
            << "  cd third_party\\qwen3-tts-cpp\n"
            << "  powershell -ExecutionPolicy Bypass -File .\\build.ps1 -UseNinja -EnableCuda -EnableCudaGraphs -Configuration Release\n";
        return false;
    }

    const fs::path engine_root = EngineRootFromExe(engine);
    const fs::path model_dir = ResolveDirNearEngineOrCwd(impl_->model_dir, engine_root);
    const fs::path speaker_embedding = ResolveFileNearEngineOrCwd(impl_->speaker_embedding, engine_root);
    const fs::path output_wav = Abs(options.output_wav.empty() ? fs::path("examples/bridge_test.wav") : fs::path(options.output_wav));

    if (!ExistsDir(model_dir)) {
        std::cerr << "Model directory not found: " << model_dir.string() << "\n";
        return false;
    }
    if (!ExistsFile(speaker_embedding)) {
        std::cerr << "Speaker embedding not found: " << speaker_embedding.string() << "\n";
        return false;
    }

    std::error_code ec;
    fs::create_directories(output_wav.parent_path(), ec);

    std::ostringstream cmd;

    // Run from the engine root so its relative DLL/config assumptions remain valid,
    // but pass model/speaker/output as absolute paths.
    cmd << "cd /d " << Quote(engine_root.string()) << " && "
        << Quote(engine.string())
        << " -m " << Quote(model_dir.string())
        << " --speaker-embedding " << Quote(speaker_embedding.string());

    const std::string model_name = NormalizeModelName(options.model_identifier);
    if (!model_name.empty()) {
        cmd << " --model-name " << Quote(model_name);
    }

    cmd << " -t " << Quote(text)
        << " --temperature 0.9"
        << " --top-k 75"
        << " --top-p 1.0"
        << " --streaming-generate"
        << " --async-streaming-decode"
        << " --play-streaming"
        << " --prewarm-streaming"
        << " --prewarm-frames 1";

    if (options.live_preroll_ms > 0) {
        cmd << " --live-preroll-ms " << options.live_preroll_ms;
    }

    cmd << " --first-tail-window-frames " << options.first_tail_window_frames
        << " --steady-tail-window-frames " << options.steady_tail_window_frames
        << " --context-frames " << options.context_frames
        << " --final-context-frames " << options.final_context_frames
        << " -o " << Quote(output_wav.string());

    if (options.dump_first_frame_profile) {
        cmd << " --dump-first-frame-profile";
    }
    if (options.dump_streaming_overlap) {
        cmd << " --dump-streaming-overlap";
    }

    std::cout << "Engine root: " << engine_root.string() << "\n";
    std::cout << "Running engine: " << cmd.str() << "\n";

    const int rc = std::system(cmd.str().c_str());
    if (rc != 0) {
        std::cerr << "Engine exited with code: " << rc << "\n";
    }
    return rc == 0;
}
