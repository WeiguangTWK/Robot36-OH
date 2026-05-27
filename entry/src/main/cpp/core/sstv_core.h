#ifndef ROBOT36_SSTV_CORE_H
#define ROBOT36_SSTV_CORE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace robot36::core {

struct PixelBuffer {
    std::vector<uint32_t> pixels;
    int width;
    int height;
    int line;

    PixelBuffer(int width, int height);
};

struct WavData {
    int sample_rate = 0;
    int channel_count = 0;
    int bits_per_sample = 0;
    std::vector<float> samples;
};

struct OfflineDecodeResult {
    bool success = false;
    int sample_rate = 0;
    int channel_count = 0;
    int completed_images = 0;
    std::string current_mode;
    std::string summary;
    std::vector<std::string> output_files;
};

class Mode {
public:
    virtual ~Mode() = default;

    virtual std::string GetName() const = 0;
    virtual int GetVISCode() const = 0;
    virtual int GetWidth() const = 0;
    virtual int GetHeight() const = 0;
    virtual int GetFirstPixelSampleIndex() const = 0;
    virtual int GetFirstSyncPulseIndex() const = 0;
    virtual int GetScanLineSamples() const = 0;
    virtual void ResetState() = 0;
    virtual bool DecodeScanLine(PixelBuffer &pixel_buffer, std::vector<float> &scratch_buffer,
        const std::vector<float> &scan_line_buffer, int scope_buffer_width, int sync_pulse_index,
        int scan_line_samples, float frequency_offset) = 0;
};

class Decoder {
public:
    Decoder(PixelBuffer scope_buffer, PixelBuffer image_buffer, std::string raw_name, int sample_rate);
    ~Decoder();

    bool Process(std::vector<float> &record_buffer, int channel_select);
    void SetMode(const std::string &name);
    std::string GetCurrentModeName() const;

    PixelBuffer &GetImageBuffer();
    const PixelBuffer &GetImageBuffer() const;
    PixelBuffer &GetScopeBuffer();
    const PixelBuffer &GetScopeBuffer() const;
    bool ConsumeCompletedImage(PixelBuffer &out_image);
    int GetCompletedImageCount() const;
    int GetSyncPulseDetectionCount() const;
    int GetHeaderDetectionCount() const;
    int GetImageLine() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

bool LoadWavFile(const std::string &path, WavData &out_wav, std::string &error);
bool SavePixelBufferAsPpm(const PixelBuffer &buffer, const std::string &path, std::string &error);
OfflineDecodeResult DecodeWavFile(const std::string &wav_path, const std::string &output_dir);

}  // namespace robot36::core

#endif
