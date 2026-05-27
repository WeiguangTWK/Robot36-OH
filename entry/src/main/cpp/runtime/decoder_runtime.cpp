#include "runtime/decoder_runtime.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace robot36::runtime {

namespace {
constexpr int kReadsPerSecond = 50;
constexpr int kWaveformPoints = 256;
constexpr int kWaterfallBins = 96;
constexpr int kWaterfallRows = 180;
constexpr double kPi = 3.14159265358979323846;
}

DecoderRuntime &DecoderRuntime::GetInstance()
{
    static DecoderRuntime runtime;
    return runtime;
}

DecoderRuntime::~DecoderRuntime()
{
    Stop();
}

bool DecoderRuntime::Start(int sample_rate, int channel_count)
{
    Stop();

    if (sample_rate <= 0 || channel_count <= 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = "Invalid decoder runtime configuration";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    sample_rate_ = sample_rate;
    channel_count_ = channel_count;
    chunk_samples_ = static_cast<size_t>((sample_rate_ / kReadsPerSecond) * channel_count_);
    max_queued_samples_ = static_cast<size_t>(sample_rate_ * channel_count_ * 4);
    dropped_samples_ = 0;
    processed_chunks_ = 0;
    completed_images_ = 0;
    sync_pulses_ = 0;
    headers_ = 0;
    image_line_ = 0;
    input_peak_ = 0.0f;
    input_rms_ = 0.0f;
    current_mode_ = "Auto Mode";
    requested_mode_ = "AUTO";
    last_error_.clear();
    stop_requested_ = false;
    running_ = true;
    pcm_queue_.clear();
    has_latest_image_ = false;
    latest_image_ = robot36::core::PixelBuffer(1, 1);
    InitializeVisualStateLocked();
    decoder_ = std::make_unique<robot36::core::Decoder>(robot36::core::PixelBuffer(640, 2 * 1280),
        robot36::core::PixelBuffer(800, 616), "Raw Mode", sample_rate_);
    decoder_->SetMode(requested_mode_);
    decode_thread_ = std::thread(&DecoderRuntime::DecodeLoop, this);
    return true;
}

void DecoderRuntime::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !decode_thread_.joinable()) {
            ResetLocked();
            return;
        }
        stop_requested_ = true;
    }
    condition_.notify_all();
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ResetLocked();
}

void DecoderRuntime::SetMode(const std::string &mode_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    requested_mode_ = mode_name;
    if (decoder_ != nullptr) {
        decoder_->SetMode(requested_mode_);
        current_mode_ = decoder_->GetCurrentModeName();
    }
}

bool DecoderRuntime::PushPcm16(const int16_t *samples, size_t sample_count)
{
    if (samples == nullptr || sample_count == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return false;
    }
    if (pcm_queue_.size() + sample_count > max_queued_samples_) {
        size_t overflow = pcm_queue_.size() + sample_count - max_queued_samples_;
        overflow = std::min(overflow, pcm_queue_.size());
        for (size_t i = 0; i < overflow; ++i) {
            pcm_queue_.pop_front();
        }
        dropped_samples_ += overflow;
    }
    for (size_t i = 0; i < sample_count; ++i) {
        pcm_queue_.push_back(samples[i]);
    }
    condition_.notify_one();
    return true;
}

DecoderRuntime::Snapshot DecoderRuntime::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snapshot;
    snapshot.running = running_;
    snapshot.sample_rate = sample_rate_;
    snapshot.channel_count = channel_count_;
    snapshot.queued_samples = pcm_queue_.size();
    snapshot.dropped_samples = dropped_samples_;
    snapshot.processed_chunks = processed_chunks_;
    snapshot.completed_images = completed_images_;
    snapshot.sync_pulses = sync_pulses_;
    snapshot.headers = headers_;
    snapshot.image_line = image_line_;
    snapshot.input_peak = input_peak_;
    snapshot.input_rms = input_rms_;
    snapshot.current_mode = current_mode_;
    snapshot.last_error = last_error_;
    return snapshot;
}

bool DecoderRuntime::GetLatestImage(robot36::core::PixelBuffer &out_image) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_latest_image_) {
        return false;
    }
    out_image = latest_image_;
    return true;
}

bool DecoderRuntime::GetLatestScope(robot36::core::PixelBuffer &out_scope) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_latest_scope_) {
        return false;
    }
    out_scope = latest_scope_;
    return true;
}

bool DecoderRuntime::GetLatestWaterfall(robot36::core::PixelBuffer &out_waterfall) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_latest_waterfall_) {
        return false;
    }
    out_waterfall = latest_waterfall_;
    return true;
}

bool DecoderRuntime::GetLatestWaveform(std::vector<float> &out_waveform) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_latest_waveform_) {
        return false;
    }
    out_waveform = latest_waveform_;
    return true;
}

void DecoderRuntime::InitializeVisualStateLocked()
{
    latest_waveform_.assign(kWaveformPoints, 0.0f);
    latest_waterfall_ = robot36::core::PixelBuffer(kWaterfallBins, kWaterfallRows);
    std::fill(latest_waterfall_.pixels.begin(), latest_waterfall_.pixels.end(), 0xFF081018u);
    latest_waterfall_.line = 0;
    waterfall_row_ = 0;
    waterfall_floor_db_ = -72.0f;
    waterfall_ceiling_db_ = -18.0f;
    has_latest_waveform_ = true;
    has_latest_waterfall_ = true;
}

void DecoderRuntime::ResetLocked()
{
    stop_requested_ = false;
    running_ = false;
    sample_rate_ = 0;
    channel_count_ = 0;
    chunk_samples_ = 0;
    max_queued_samples_ = 0;
    dropped_samples_ = 0;
    processed_chunks_ = 0;
    completed_images_ = 0;
    sync_pulses_ = 0;
    headers_ = 0;
    image_line_ = 0;
    input_peak_ = 0.0f;
    input_rms_ = 0.0f;
    current_mode_ = "Stopped";
    pcm_queue_.clear();
    decoder_.reset();
    has_latest_image_ = false;
    has_latest_scope_ = false;
    has_latest_waveform_ = false;
    has_latest_waterfall_ = false;
    latest_waveform_.clear();
}

void DecoderRuntime::DecodeLoop()
{
    for (;;) {
        std::vector<int16_t> pcm_chunk;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() {
                return stop_requested_ || pcm_queue_.size() >= chunk_samples_ || (!pcm_queue_.empty() && stop_requested_);
            });
            if (stop_requested_ && pcm_queue_.empty()) {
                break;
            }
            size_t take = std::min(chunk_samples_, pcm_queue_.size());
            if (take == 0) {
                break;
            }
            pcm_chunk.reserve(take);
            for (size_t i = 0; i < take; ++i) {
                pcm_chunk.push_back(pcm_queue_.front());
                pcm_queue_.pop_front();
            }
        }

        std::vector<float> float_chunk(pcm_chunk.size());
        float peak = 0.0f;
        double power_sum = 0.0;
        for (size_t i = 0; i < pcm_chunk.size(); ++i) {
            float_chunk[i] = static_cast<float>(pcm_chunk[i]) / 32768.0f;
            float abs_sample = std::fabs(float_chunk[i]);
            peak = std::max(peak, abs_sample);
            power_sum += static_cast<double>(float_chunk[i]) * static_cast<double>(float_chunk[i]);
        }
        float rms = pcm_chunk.empty() ? 0.0f : static_cast<float>(std::sqrt(power_sum / static_cast<double>(pcm_chunk.size())));

        robot36::core::PixelBuffer completed_image(1, 1);
        if (decoder_ != nullptr) {
            decoder_->Process(float_chunk, channel_count_ == 1 ? 0 : 1);
            while (decoder_->ConsumeCompletedImage(completed_image)) {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_image_ = completed_image;
                has_latest_image_ = true;
                ++completed_images_;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            latest_scope_ = decoder_->GetScopeBuffer();
            has_latest_scope_ = true;
            UpdateWaveformLocked(float_chunk);
            UpdateWaterfallLocked(float_chunk);
            ++processed_chunks_;
            current_mode_ = decoder_->GetCurrentModeName();
            sync_pulses_ = decoder_->GetSyncPulseDetectionCount();
            headers_ = decoder_->GetHeaderDetectionCount();
            image_line_ = decoder_->GetImageLine();
            input_peak_ = peak;
            input_rms_ = rms;
        }
    }
}

void DecoderRuntime::UpdateWaveformLocked(const std::vector<float> &float_chunk)
{
    if (float_chunk.empty()) {
        return;
    }
    if (latest_waveform_.size() != kWaveformPoints) {
        latest_waveform_.assign(kWaveformPoints, 0.0f);
    }
    for (int i = 0; i < kWaveformPoints; ++i) {
        size_t begin = (static_cast<size_t>(i) * float_chunk.size()) / kWaveformPoints;
        size_t end = (static_cast<size_t>(i + 1) * float_chunk.size()) / kWaveformPoints;
        if (end <= begin) {
            end = std::min(begin + 1, float_chunk.size());
        }
        float peak = 0.0f;
        for (size_t j = begin; j < end; ++j) {
            peak = std::max(peak, std::fabs(float_chunk[j]));
        }
        latest_waveform_[static_cast<size_t>(i)] = peak;
    }
}

void DecoderRuntime::UpdateWaterfallLocked(const std::vector<float> &float_chunk)
{
    if (float_chunk.size() < 8 || latest_waterfall_.width <= 0 || latest_waterfall_.height <= 0) {
        return;
    }

    std::vector<float> spectrum(static_cast<size_t>(latest_waterfall_.width), 0.0f);
    const size_t sample_count = float_chunk.size();
    const size_t nyquist_bin = std::max<size_t>(2, sample_count / 2);
    float row_min_db = 0.0f;
    float row_max_db = -120.0f;
    for (int bin = 0; bin < latest_waterfall_.width; ++bin) {
        size_t fft_bin = 1 + (static_cast<size_t>(bin) * (nyquist_bin - 1)) / static_cast<size_t>(latest_waterfall_.width);
        double real = 0.0;
        double imag = 0.0;
        for (size_t n = 0; n < sample_count; ++n) {
            double window = 0.5 - 0.5 * std::cos((2.0 * kPi * static_cast<double>(n)) / static_cast<double>(sample_count - 1));
            double angle = (2.0 * kPi * static_cast<double>(fft_bin) * static_cast<double>(n)) / static_cast<double>(sample_count);
            double sample = static_cast<double>(float_chunk[n]) * window;
            real += sample * std::cos(angle);
            imag -= sample * std::sin(angle);
        }
        double magnitude = std::sqrt(real * real + imag * imag) / static_cast<double>(sample_count);
        float magnitude_db = static_cast<float>(20.0 * std::log10(magnitude + 1.0e-6));
        row_min_db = std::min(row_min_db, magnitude_db);
        row_max_db = std::max(row_max_db, magnitude_db);
        spectrum[static_cast<size_t>(bin)] = magnitude_db;
    }

    waterfall_floor_db_ = waterfall_floor_db_ * 0.96f + row_min_db * 0.04f;
    waterfall_ceiling_db_ = waterfall_ceiling_db_ * 0.90f + row_max_db * 0.10f;
    if (waterfall_ceiling_db_ < waterfall_floor_db_ + 18.0f) {
        waterfall_ceiling_db_ = waterfall_floor_db_ + 18.0f;
    }
    float dynamic_range = std::max(24.0f, waterfall_ceiling_db_ - waterfall_floor_db_);

    const int row = waterfall_row_;
    for (int x = 0; x < latest_waterfall_.width; ++x) {
        float normalized = (spectrum[static_cast<size_t>(x)] - waterfall_floor_db_) / dynamic_range;
        latest_waterfall_.pixels[static_cast<size_t>(row * latest_waterfall_.width + x)] =
            HeatMapColor(std::min(std::max(normalized, 0.0f), 1.0f));
    }
    waterfall_row_ = (waterfall_row_ + 1) % latest_waterfall_.height;
    latest_waterfall_.line = waterfall_row_;
}

uint32_t DecoderRuntime::HeatMapColor(float level)
{
    level = std::min(std::max(level, 0.0f), 1.0f);
    float red = std::min(1.0f, level * 2.2f);
    float green = std::min(1.0f, std::max(0.0f, (level - 0.2f) * 1.8f));
    float blue = std::min(1.0f, std::max(0.0f, 0.35f + (1.0f - level) * 0.65f));
    uint32_t r = static_cast<uint32_t>(red * 255.0f);
    uint32_t g = static_cast<uint32_t>(green * 255.0f);
    uint32_t b = static_cast<uint32_t>(blue * 255.0f);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

}  // namespace robot36::runtime
