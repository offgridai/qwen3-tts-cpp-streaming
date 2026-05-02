#pragma once
#include <string>
#include <vector>
class WavWriter {
public:
    static void Write(const std::string& path, const std::vector<int16_t>& pcm, int sample_rate);
};
