#include <iostream>
#include <string>

#include "../entry/src/main/cpp/core/sstv_core.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "Usage: stage1_wav_driver <wav-path> <output-dir>\n";
        return 2;
    }

    robot36::core::OfflineDecodeResult result = robot36::core::DecodeWavFile(argv[1], argv[2]);
    std::cout << result.summary << "\n";
    for (const std::string &output : result.output_files) {
        std::cout << output << "\n";
    }
    return result.success ? 0 : 1;
}
