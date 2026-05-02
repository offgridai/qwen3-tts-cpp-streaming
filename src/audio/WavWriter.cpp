#include "WavWriter.h"
#include <fstream>

void WavWriter::Write(const std::string& path, const std::vector<int16_t>& pcm, int sample_rate) {
    std::ofstream f(path, std::ios::binary);
    int dataSize = pcm.size() * sizeof(int16_t);

    f.write("RIFF", 4);
    int chunkSize = 36 + dataSize;
    f.write((char*)&chunkSize, 4);
    f.write("WAVEfmt ", 8);

    int subchunk1Size = 16;
    short audioFormat = 1;
    short numChannels = 1;
    int byteRate = sample_rate * numChannels * 2;
    short blockAlign = numChannels * 2;
    short bitsPerSample = 16;

    f.write((char*)&subchunk1Size, 4);
    f.write((char*)&audioFormat, 2);
    f.write((char*)&numChannels, 2);
    f.write((char*)&sample_rate, 4);
    f.write((char*)&byteRate, 4);
    f.write((char*)&blockAlign, 2);
    f.write((char*)&bitsPerSample, 2);

    f.write("data", 4);
    f.write((char*)&dataSize, 4);
    f.write((char*)pcm.data(), dataSize);
}
