#ifndef ROBOT36_DECODER_RUNTIME_H
#define ROBOT36_DECODER_RUNTIME_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/sstv_core.h"

namespace robot36::runtime {

class DecoderRuntime {
public:
    struct Snapshot {
        bool running = false;
        int sample_rate = 0;
        int channel_count = 0;
        size_t queued_samples = 0;
        size_t dropped_samples = 0;
        int processed_chunks = 0;
        int completed_images = 0;
        int sync_pulses = 0;
        int headers = 0;
        int image_line = 0;
        float input_peak = 0.0f;
        float input_rms = 0.0f;
        std::string current_mode;
        std::string last_error;
    };

    static DecoderRuntime &GetInstance();

    bool Start(int sample_rate, int channel_count);
    void Stop();
    void SetMode(const std::string &mode_name);
    bool PushPcm16(const int16_t *samples, size_t sample_count);
    Snapshot GetSnapshot() const;
    bool GetLatestImage(robot36::core::PixelBuffer &out_image) const;
    bool GetLatestScope(robot36::core::PixelBuffer &out_scope) const;
    bool GetLatestWaterfall(robot36::core::PixelBuffer &out_waterfall) const;
    bool GetLatestWaveform(std::vector<float> &out_waveform) const;

private:
    DecoderRuntime() = default;
    ~DecoderRuntime();
    DecoderRuntime(const DecoderRuntime &) = delete;
    DecoderRuntime &operator=(const DecoderRuntime &) = delete;

    void InitializeVisualStateLocked();
    void ResetLocked();
    void DecodeLoop();
    void UpdateWaveformLocked(const std::vector<float> &float_chunk);
    void UpdateWaterfallLocked(const std::vector<float> &float_chunk);
    static uint32_t HeatMapColor(float level);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_requested_ = false;
    bool running_ = false;
    int sample_rate_ = 0;
    int channel_count_ = 0;
    size_t chunk_samples_ = 0;
    size_t max_queued_samples_ = 0;
    size_t dropped_samples_ = 0;
    int processed_chunks_ = 0;
    int completed_images_ = 0;
    int sync_pulses_ = 0;
    int headers_ = 0;
    int image_line_ = 0;
    float input_peak_ = 0.0f;
    float input_rms_ = 0.0f;
    std::string current_mode_ = "Idle";
    std::string last_error_;
    std::deque<int16_t> pcm_queue_;
    std::thread decode_thread_;
    std::unique_ptr<robot36::core::Decoder> decoder_;
    robot36::core::PixelBuffer latest_image_ {1, 1};
    robot36::core::PixelBuffer latest_scope_ {640, 2 * 1280};
    robot36::core::PixelBuffer latest_waterfall_ {96, 180};
    std::vector<float> latest_waveform_ {};
    bool has_latest_image_ = false;
    bool has_latest_scope_ = false;
    bool has_latest_waterfall_ = false;
    bool has_latest_waveform_ = false;
    int waterfall_row_ = 0;
    float waterfall_floor_db_ = -72.0f;
    float waterfall_ceiling_db_ = -18.0f;
    std::string requested_mode_ = "AUTO";
};

}  // namespace robot36::runtime

#endif
