#include "core/sstv_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace robot36::core {

namespace {

constexpr double kPi = 3.14159265358979323846;

int ClampByte(int value)
{
    return std::min(std::max(value, 0), 255);
}

float ClampUnit(float value)
{
    return std::min(std::max(value, 0.0f), 1.0f);
}

int FloatToByte(float level)
{
    return ClampByte(static_cast<int>(std::lround(255.0f * level)));
}

int Compress(float level)
{
    return FloatToByte(std::sqrt(ClampUnit(level)));
}

uint32_t YuvToRgbBytes(int y, int u, int v)
{
    y -= 16;
    u -= 128;
    v -= 128;
    int red = ClampByte((298 * y + 409 * v + 128) >> 8);
    int green = ClampByte((298 * y - 100 * u - 208 * v + 128) >> 8);
    int blue = ClampByte((298 * y + 516 * u + 128) >> 8);
    return 0xFF000000u | (static_cast<uint32_t>(red) << 16) | (static_cast<uint32_t>(green) << 8) |
        static_cast<uint32_t>(blue);
}

uint32_t Gray(float level)
{
    return 0xFF000000u | static_cast<uint32_t>(0x00010101 * Compress(level));
}

uint32_t Rgb(float red, float green, float blue)
{
    return 0xFF000000u | (static_cast<uint32_t>(FloatToByte(red)) << 16) |
        (static_cast<uint32_t>(FloatToByte(green)) << 8) | static_cast<uint32_t>(FloatToByte(blue));
}

uint32_t YuvToRgb(float y, float u, float v)
{
    return YuvToRgbBytes(FloatToByte(y), FloatToByte(u), FloatToByte(v));
}

uint32_t YuvPackedToRgb(uint32_t yuv)
{
    return YuvToRgbBytes(static_cast<int>((yuv & 0x00FF0000u) >> 16), static_cast<int>((yuv & 0x0000FF00u) >> 8),
        static_cast<int>(yuv & 0x000000FFu));
}

float Luminance(uint32_t argb)
{
    float red = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    float green = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    float blue = static_cast<float>(argb & 0xFFu) / 255.0f;
    return 0.2126f * red + 0.7152f * green + 0.0722f * blue;
}

double Sinc(double value)
{
    if (value == 0.0) {
        return 1.0;
    }
    value *= kPi;
    return std::sin(value) / value;
}

double LowPass(double cutoff, double rate, int n, int count)
{
    double f = 2.0 * cutoff / rate;
    double x = n - (count - 1) / 2.0;
    return f * Sinc(f * x);
}

double HannWindow(int n, int count)
{
    return 0.5 * (1.0 - std::cos((2.0 * kPi * n) / (count - 1)));
}

class Kaiser {
public:
    Kaiser() : summands_(35, 0.0) {}

    double Window(double a, int n, int count)
    {
        return I0(kPi * a * std::sqrt(1.0 - Square((2.0 * n) / (count - 1) - 1.0))) / I0(kPi * a);
    }

private:
    static double Square(double value)
    {
        return value * value;
    }

    double I0(double x)
    {
        summands_[0] = 1.0;
        double value = 1.0;
        for (size_t i = 1; i < summands_.size(); ++i) {
            value *= x / (2.0 * static_cast<double>(i));
            summands_[i] = Square(value);
        }
        std::sort(summands_.begin(), summands_.end());
        double sum = 0.0;
        for (auto it = summands_.rbegin(); it != summands_.rend(); ++it) {
            sum += *it;
        }
        return sum;
    }

    std::vector<double> summands_;
};

struct Complex {
    float real = 0.0f;
    float imag = 0.0f;

    Complex() = default;
    Complex(float real, float imag) : real(real), imag(imag) {}

    Complex &Set(const Complex &other)
    {
        real = other.real;
        imag = other.imag;
        return *this;
    }

    Complex &Set(float value_real, float value_imag)
    {
        real = value_real;
        imag = value_imag;
        return *this;
    }

    Complex &Set(float value_real)
    {
        return Set(value_real, 0.0f);
    }

    float Norm() const
    {
        return real * real + imag * imag;
    }

    float Abs() const
    {
        return std::sqrt(Norm());
    }

    float Arg() const
    {
        return std::atan2(imag, real);
    }

    Complex &Conj()
    {
        imag = -imag;
        return *this;
    }

    Complex &Add(const Complex &other)
    {
        real += other.real;
        imag += other.imag;
        return *this;
    }

    Complex &Sub(const Complex &other)
    {
        real -= other.real;
        imag -= other.imag;
        return *this;
    }

    Complex &Mul(float value)
    {
        real *= value;
        imag *= value;
        return *this;
    }

    Complex &Mul(const Complex &other)
    {
        float temp = real * other.real - imag * other.imag;
        imag = real * other.imag + imag * other.real;
        real = temp;
        return *this;
    }

    Complex &Div(float value)
    {
        real /= value;
        imag /= value;
        return *this;
    }
};

class SimpleMovingSum {
public:
    explicit SimpleMovingSum(int length) : tree_(2 * length, 0.0f), leaf_(length), length_(length) {}

    void Add(float input)
    {
        tree_[leaf_] = input;
        for (int child = leaf_, parent = leaf_ / 2; parent > 0; child = parent, parent /= 2) {
            tree_[parent] = tree_[child] + tree_[child ^ 1];
        }
        if (++leaf_ >= static_cast<int>(tree_.size())) {
            leaf_ = length_;
        }
    }

    float Sum() const
    {
        return tree_[1];
    }

    float Sum(float input)
    {
        Add(input);
        return Sum();
    }

    int Length() const
    {
        return length_;
    }

private:
    std::vector<float> tree_;
    int leaf_;
    int length_;
};

class SimpleMovingAverage : public SimpleMovingSum {
public:
    explicit SimpleMovingAverage(int length) : SimpleMovingSum(length) {}

    float Avg(float input)
    {
        return Sum(input) / static_cast<float>(Length());
    }
};

class ExponentialMovingAverage {
public:
    float Avg(float input)
    {
        prev_ = prev_ * (1.0f - alpha_) + alpha_ * input;
        return prev_;
    }

    void Alpha(double alpha)
    {
        alpha_ = static_cast<float>(alpha);
    }

    void Alpha(double alpha, int order)
    {
        Alpha(std::pow(alpha, 1.0 / order));
    }

    void Cutoff(double frequency, double rate, int order)
    {
        double x = std::cos(2.0 * kPi * frequency / rate);
        Alpha(x - 1.0 + std::sqrt(x * (x - 4.0) + 3.0), order);
    }

    void Reset()
    {
        prev_ = 0.0f;
    }

private:
    float alpha_ = 1.0f;
    float prev_ = 0.0f;
};

class Delay {
public:
    explicit Delay(int length) : buffer_(length, 0.0f), length_(length) {}

    float Push(float input)
    {
        float temp = buffer_[position_];
        buffer_[position_] = input;
        if (++position_ >= length_) {
            position_ = 0;
        }
        return temp;
    }

private:
    std::vector<float> buffer_;
    int length_;
    int position_ = 0;
};

class SchmittTrigger {
public:
    SchmittTrigger(float low, float high) : low_(low), high_(high) {}

    bool Latch(float input)
    {
        if (previous_) {
            if (input < low_) {
                previous_ = false;
            }
        } else if (input > high_) {
            previous_ = true;
        }
        return previous_;
    }

private:
    float low_;
    float high_;
    bool previous_ = false;
};

class ComplexConvolution {
public:
    explicit ComplexConvolution(int length)
        : length_(length), taps_(length, 0.0f), real_(length, 0.0f), imag_(length, 0.0f)
    {
    }

    Complex Push(const Complex &input)
    {
        real_[position_] = input.real;
        imag_[position_] = input.imag;
        if (++position_ >= length_) {
            position_ = 0;
        }
        sum_.real = 0.0f;
        sum_.imag = 0.0f;
        for (float tap : taps_) {
            sum_.real += tap * real_[position_];
            sum_.imag += tap * imag_[position_];
            if (++position_ >= length_) {
                position_ = 0;
            }
        }
        return sum_;
    }

    int length_;
    std::vector<float> taps_;

private:
    std::vector<float> real_;
    std::vector<float> imag_;
    Complex sum_;
    int position_ = 0;
};

class FrequencyModulation {
public:
    FrequencyModulation(double bandwidth, double sample_rate)
        : pi_(static_cast<float>(kPi)), two_pi_(2.0f * pi_),
          scale_(static_cast<float>(sample_rate / (bandwidth * kPi)))
    {
    }

    float Demod(const Complex &input)
    {
        float phase = input.Arg();
        float delta = Wrap(phase - previous_);
        previous_ = phase;
        return scale_ * delta;
    }

private:
    float Wrap(float value) const
    {
        if (value < -pi_) {
            return value + two_pi_;
        }
        if (value > pi_) {
            return value - two_pi_;
        }
        return value;
    }

    float previous_ = 0.0f;
    float pi_;
    float two_pi_;
    float scale_;
};

class Phasor {
public:
    Phasor(double frequency, double rate)
    {
        value_.Set(1.0f, 0.0f);
        double omega = 2.0 * kPi * frequency / rate;
        delta_.Set(static_cast<float>(std::cos(omega)), static_cast<float>(std::sin(omega)));
    }

    Complex Rotate()
    {
        value_.Mul(delta_);
        float magnitude = value_.Abs();
        if (magnitude != 0.0f) {
            value_.Div(magnitude);
        }
        return value_;
    }

private:
    Complex value_;
    Complex delta_;
};

class Demodulator {
public:
    enum class SyncPulseWidth { FiveMilliSeconds, NineMilliSeconds, TwentyMilliSeconds };

    explicit Demodulator(int sample_rate)
        : frequency_modulation_(white_frequency - black_frequency, sample_rate),
          sync_pulse_value_delay_(CalculateDelayLength(sample_rate)),
          base_band_low_pass_(CalculateBaseBandLowPassSamples(sample_rate)),
          base_band_(),
          base_band_oscillator_(-center_frequency, sample_rate),
          sync_pulse_filter_(CalculateDelayLength(sample_rate)),
          sync_pulse_trigger_(0.0f, 0.0f)
    {
        scan_line_bandwidth_ = white_frequency - black_frequency;
        sync_pulse_filter_delay_ = (sync_pulse_filter_.Length() - 1) / 2;
        sync_pulse_5ms_min_samples_ = static_cast<int>(std::lround((0.005 / 2.0) * sample_rate));
        sync_pulse_5ms_max_samples_ = static_cast<int>(std::lround(((0.005 + 0.009) / 2.0) * sample_rate));
        sync_pulse_9ms_max_samples_ = static_cast<int>(std::lround(((0.009 + 0.020) / 2.0) * sample_rate));
        sync_pulse_20ms_max_samples_ = static_cast<int>(std::lround((0.020 + 0.005) * sample_rate));

        Kaiser kaiser;
        int taps_length = base_band_low_pass_.length_;
        double cutoff_frequency = (2800.0 - 1000.0) / 2.0;
        for (int i = 0; i < taps_length; ++i) {
            base_band_low_pass_.taps_[i] = static_cast<float>(
                kaiser.Window(2.0, i, taps_length) * LowPass(cutoff_frequency, sample_rate, i, taps_length));
        }

        sync_pulse_frequency_value_ = static_cast<float>(NormalizeFrequency(sync_pulse_frequency));
        sync_pulse_frequency_tolerance_ = static_cast<float>(50.0 * 2.0 / scan_line_bandwidth_);
        float sync_high_frequency = static_cast<float>((sync_pulse_frequency + 1500.0) / 2.0);
        float sync_low_frequency = static_cast<float>((sync_pulse_frequency + sync_high_frequency) / 2.0);
        sync_pulse_trigger_ = SchmittTrigger(static_cast<float>(NormalizeFrequency(sync_low_frequency)),
            static_cast<float>(NormalizeFrequency(sync_high_frequency)));
    }

    bool Process(std::vector<float> &buffer, int channel_select)
    {
        bool sync_pulse_detected = false;
        int channels = channel_select > 0 ? 2 : 1;
        for (int i = 0; i < static_cast<int>(buffer.size()) / channels; ++i) {
            switch (channel_select) {
                case 1:
                    base_band_.Set(buffer[2 * i]);
                    break;
                case 2:
                    base_band_.Set(buffer[2 * i + 1]);
                    break;
                case 3:
                    base_band_.Set(buffer[2 * i] + buffer[2 * i + 1]);
                    break;
                case 4:
                    base_band_.Set(buffer[2 * i], buffer[2 * i + 1]);
                    break;
                default:
                    base_band_.Set(buffer[i]);
                    break;
            }
            Complex mixed = base_band_;
            mixed.Mul(base_band_oscillator_.Rotate());
            base_band_ = base_band_low_pass_.Push(mixed);
            float frequency_value = frequency_modulation_.Demod(base_band_);
            float sync_pulse_value = sync_pulse_filter_.Avg(frequency_value);
            float sync_pulse_delayed_value = sync_pulse_value_delay_.Push(sync_pulse_value);
            buffer[i] = frequency_value;
            if (!sync_pulse_trigger_.Latch(sync_pulse_value)) {
                ++sync_pulse_counter_;
            } else if (sync_pulse_counter_ < sync_pulse_5ms_min_samples_ ||
                sync_pulse_counter_ > sync_pulse_20ms_max_samples_ ||
                std::fabs(sync_pulse_delayed_value - sync_pulse_frequency_value_) > sync_pulse_frequency_tolerance_) {
                sync_pulse_counter_ = 0;
            } else {
                if (sync_pulse_counter_ < sync_pulse_5ms_max_samples_) {
                    sync_pulse_width = SyncPulseWidth::FiveMilliSeconds;
                } else if (sync_pulse_counter_ < sync_pulse_9ms_max_samples_) {
                    sync_pulse_width = SyncPulseWidth::NineMilliSeconds;
                } else {
                    sync_pulse_width = SyncPulseWidth::TwentyMilliSeconds;
                }
                sync_pulse_offset = i - sync_pulse_filter_delay_;
                frequency_offset = sync_pulse_delayed_value - sync_pulse_frequency_value_;
                sync_pulse_detected = true;
                sync_pulse_counter_ = 0;
            }
        }
        return sync_pulse_detected;
    }

    SyncPulseWidth sync_pulse_width = SyncPulseWidth::NineMilliSeconds;
    int sync_pulse_offset = 0;
    float frequency_offset = 0.0f;

    static constexpr double sync_pulse_frequency = 1200.0;
    static constexpr double black_frequency = 1500.0;
    static constexpr double white_frequency = 2300.0;
    static constexpr double center_frequency = 1900.0;

private:
    static int CalculateDelayLength(int sample_rate)
    {
        int samples = static_cast<int>(std::lround((0.005 / 2.0) * sample_rate)) | 1;
        return samples;
    }

    static int CalculateBaseBandLowPassSamples(int sample_rate)
    {
        return static_cast<int>(std::lround(0.002 * sample_rate)) | 1;
    }

    double NormalizeFrequency(double frequency) const
    {
        return (frequency - center_frequency) * 2.0 / scan_line_bandwidth_;
    }

    SimpleMovingAverage sync_pulse_filter_;
    ComplexConvolution base_band_low_pass_;
    FrequencyModulation frequency_modulation_;
    SchmittTrigger sync_pulse_trigger_;
    Phasor base_band_oscillator_;
    Delay sync_pulse_value_delay_;
    double scan_line_bandwidth_ = 0.0;
    float sync_pulse_frequency_value_ = 0.0f;
    float sync_pulse_frequency_tolerance_ = 0.0f;
    int sync_pulse_5ms_min_samples_ = 0;
    int sync_pulse_5ms_max_samples_ = 0;
    int sync_pulse_9ms_max_samples_ = 0;
    int sync_pulse_20ms_max_samples_ = 0;
    int sync_pulse_filter_delay_ = 0;
    int sync_pulse_counter_ = 0;
    Complex base_band_;
};

class BaseMode : public Mode {
public:
    void ResetState() override {}
};

class RawDecoder : public BaseMode {
public:
    RawDecoder(std::string name, int sample_rate)
        : name_(std::move(name)),
          small_picture_max_samples_(static_cast<int>(std::lround(0.125 * sample_rate))),
          medium_picture_max_samples_(static_cast<int>(std::lround(0.175 * sample_rate)))
    {
    }

    std::string GetName() const override { return name_; }
    int GetVISCode() const override { return -1; }
    int GetWidth() const override { return -1; }
    int GetHeight() const override { return -1; }
    int GetFirstPixelSampleIndex() const override { return 0; }
    int GetFirstSyncPulseIndex() const override { return -1; }
    int GetScanLineSamples() const override { return -1; }

    bool DecodeScanLine(PixelBuffer &pixel_buffer, std::vector<float> &scratch_buffer,
        const std::vector<float> &scan_line_buffer, int scope_buffer_width, int sync_pulse_index,
        int scan_line_samples, float frequency_offset) override
    {
        if (sync_pulse_index < 0 || sync_pulse_index + scan_line_samples > static_cast<int>(scan_line_buffer.size())) {
            return false;
        }
        int horizontal_pixels = scope_buffer_width;
        if (scan_line_samples < small_picture_max_samples_) {
            horizontal_pixels /= 2;
        }
        if (scan_line_samples < medium_picture_max_samples_) {
            horizontal_pixels /= 2;
        }
        low_pass_filter_.Cutoff(horizontal_pixels, 2.0 * scan_line_samples, 2);
        low_pass_filter_.Reset();
        for (int i = 0; i < scan_line_samples; ++i) {
            scratch_buffer[i] = low_pass_filter_.Avg(scan_line_buffer[sync_pulse_index + i]);
        }
        low_pass_filter_.Reset();
        for (int i = scan_line_samples - 1; i >= 0; --i) {
            scratch_buffer[i] = FreqToLevel(low_pass_filter_.Avg(scratch_buffer[i]), frequency_offset);
        }
        if (static_cast<int>(pixel_buffer.pixels.size()) < horizontal_pixels) {
            pixel_buffer.pixels.resize(horizontal_pixels);
        }
        for (int i = 0; i < horizontal_pixels; ++i) {
            int position = (i * scan_line_samples) / horizontal_pixels;
            pixel_buffer.pixels[i] = Gray(scratch_buffer[position]);
        }
        pixel_buffer.width = horizontal_pixels;
        pixel_buffer.height = 1;
        return true;
    }

private:
    static float FreqToLevel(float frequency, float offset)
    {
        return 0.5f * (frequency - offset + 1.0f);
    }

    ExponentialMovingAverage low_pass_filter_;
    int small_picture_max_samples_;
    int medium_picture_max_samples_;
    std::string name_;
};

class RGBDecoder : public BaseMode {
public:
    RGBDecoder(std::string name, int code, int horizontal_pixels, int vertical_pixels,
        double first_sync_pulse_seconds, double scan_line_seconds, double begin_seconds,
        double red_begin_seconds, double red_end_seconds, double green_begin_seconds,
        double green_end_seconds, double blue_begin_seconds, double blue_end_seconds,
        double end_seconds, int sample_rate)
        : horizontal_pixels_(horizontal_pixels),
          vertical_pixels_(vertical_pixels),
          first_sync_pulse_index_(static_cast<int>(std::lround(first_sync_pulse_seconds * sample_rate))),
          scan_line_samples_(static_cast<int>(std::lround(scan_line_seconds * sample_rate))),
          begin_samples_(static_cast<int>(std::lround(begin_seconds * sample_rate))),
          red_begin_samples_(static_cast<int>(std::lround(red_begin_seconds * sample_rate)) - begin_samples_),
          red_samples_(static_cast<int>(std::lround((red_end_seconds - red_begin_seconds) * sample_rate))),
          green_begin_samples_(static_cast<int>(std::lround(green_begin_seconds * sample_rate)) - begin_samples_),
          green_samples_(static_cast<int>(std::lround((green_end_seconds - green_begin_seconds) * sample_rate))),
          blue_begin_samples_(static_cast<int>(std::lround(blue_begin_seconds * sample_rate)) - begin_samples_),
          blue_samples_(static_cast<int>(std::lround((blue_end_seconds - blue_begin_seconds) * sample_rate))),
          end_samples_(static_cast<int>(std::lround(end_seconds * sample_rate))),
          name_(std::move(name)),
          code_(code)
    {
    }

    std::string GetName() const override { return name_; }
    int GetVISCode() const override { return code_; }
    int GetWidth() const override { return horizontal_pixels_; }
    int GetHeight() const override { return vertical_pixels_; }
    int GetFirstPixelSampleIndex() const override { return begin_samples_; }
    int GetFirstSyncPulseIndex() const override { return first_sync_pulse_index_; }
    int GetScanLineSamples() const override { return scan_line_samples_; }

    bool DecodeScanLine(PixelBuffer &pixel_buffer, std::vector<float> &scratch_buffer,
        const std::vector<float> &scan_line_buffer, int scope_buffer_width, int sync_pulse_index,
        int scan_line_samples, float frequency_offset) override
    {
        (void)scope_buffer_width;
        (void)scan_line_samples;
        if (sync_pulse_index + begin_samples_ < 0 ||
            sync_pulse_index + end_samples_ > static_cast<int>(scan_line_buffer.size())) {
            return false;
        }
        low_pass_filter_.Cutoff(horizontal_pixels_, 2.0 * green_samples_, 2);
        low_pass_filter_.Reset();
        for (int i = 0; i < end_samples_ - begin_samples_; ++i) {
            scratch_buffer[i] = low_pass_filter_.Avg(scan_line_buffer[sync_pulse_index + begin_samples_ + i]);
        }
        low_pass_filter_.Reset();
        for (int i = end_samples_ - begin_samples_ - 1; i >= 0; --i) {
            scratch_buffer[i] = FreqToLevel(low_pass_filter_.Avg(scratch_buffer[i]), frequency_offset);
        }
        if (static_cast<int>(pixel_buffer.pixels.size()) < horizontal_pixels_) {
            pixel_buffer.pixels.resize(horizontal_pixels_);
        }
        for (int i = 0; i < horizontal_pixels_; ++i) {
            int red_pos = red_begin_samples_ + (i * red_samples_) / horizontal_pixels_;
            int green_pos = green_begin_samples_ + (i * green_samples_) / horizontal_pixels_;
            int blue_pos = blue_begin_samples_ + (i * blue_samples_) / horizontal_pixels_;
            pixel_buffer.pixels[i] = Rgb(scratch_buffer[red_pos], scratch_buffer[green_pos], scratch_buffer[blue_pos]);
        }
        pixel_buffer.width = horizontal_pixels_;
        pixel_buffer.height = 1;
        return true;
    }

private:
    static float FreqToLevel(float frequency, float offset)
    {
        return 0.5f * (frequency - offset + 1.0f);
    }

    ExponentialMovingAverage low_pass_filter_;
    int horizontal_pixels_;
    int vertical_pixels_;
    int first_sync_pulse_index_;
    int scan_line_samples_;
    int begin_samples_;
    int red_begin_samples_;
    int red_samples_;
    int green_begin_samples_;
    int green_samples_;
    int blue_begin_samples_;
    int blue_samples_;
    int end_samples_;
    std::string name_;
    int code_;
};

class PaulDon : public BaseMode {
public:
    PaulDon(std::string name, int code, int horizontal_pixels, int vertical_pixels, double channel_seconds,
        int sample_rate)
        : horizontal_pixels_(horizontal_pixels),
          vertical_pixels_(vertical_pixels),
          scan_line_samples_(static_cast<int>(std::lround((0.02 + 0.00208 + 4.0 * channel_seconds) * sample_rate))),
          channel_samples_(static_cast<int>(std::lround(channel_seconds * sample_rate))),
          y_even_begin_samples_(static_cast<int>(std::lround(0.00208 * sample_rate))),
          begin_samples_(y_even_begin_samples_),
          v_avg_begin_samples_(static_cast<int>(std::lround((0.00208 + channel_seconds) * sample_rate))),
          u_avg_begin_samples_(static_cast<int>(std::lround((0.00208 + 2.0 * channel_seconds) * sample_rate))),
          y_odd_begin_samples_(static_cast<int>(std::lround((0.00208 + 3.0 * channel_seconds) * sample_rate))),
          end_samples_(static_cast<int>(std::lround((0.00208 + 4.0 * channel_seconds) * sample_rate))),
          name_("PD " + name),
          code_(code)
    {
    }

    std::string GetName() const override { return name_; }
    int GetVISCode() const override { return code_; }
    int GetWidth() const override { return horizontal_pixels_; }
    int GetHeight() const override { return vertical_pixels_; }
    int GetFirstPixelSampleIndex() const override { return begin_samples_; }
    int GetFirstSyncPulseIndex() const override { return 0; }
    int GetScanLineSamples() const override { return scan_line_samples_; }

    bool DecodeScanLine(PixelBuffer &pixel_buffer, std::vector<float> &scratch_buffer,
        const std::vector<float> &scan_line_buffer, int scope_buffer_width, int sync_pulse_index,
        int scan_line_samples, float frequency_offset) override
    {
        (void)scope_buffer_width;
        (void)scan_line_samples;
        if (sync_pulse_index + begin_samples_ < 0 ||
            sync_pulse_index + end_samples_ > static_cast<int>(scan_line_buffer.size())) {
            return false;
        }
        low_pass_filter_.Cutoff(horizontal_pixels_, 2.0 * channel_samples_, 2);
        low_pass_filter_.Reset();
        for (int i = begin_samples_; i < end_samples_; ++i) {
            scratch_buffer[i] = low_pass_filter_.Avg(scan_line_buffer[sync_pulse_index + i]);
        }
        low_pass_filter_.Reset();
        for (int i = end_samples_ - 1; i >= begin_samples_; --i) {
            scratch_buffer[i] = FreqToLevel(low_pass_filter_.Avg(scratch_buffer[i]), frequency_offset);
        }
        if (static_cast<int>(pixel_buffer.pixels.size()) < 2 * horizontal_pixels_) {
            pixel_buffer.pixels.resize(2 * horizontal_pixels_);
        }
        for (int i = 0; i < horizontal_pixels_; ++i) {
            int position = (i * channel_samples_) / horizontal_pixels_;
            int y_even_pos = position + y_even_begin_samples_;
            int v_avg_pos = position + v_avg_begin_samples_;
            int u_avg_pos = position + u_avg_begin_samples_;
            int y_odd_pos = position + y_odd_begin_samples_;
            pixel_buffer.pixels[i] = YuvToRgb(scratch_buffer[y_even_pos], scratch_buffer[u_avg_pos], scratch_buffer[v_avg_pos]);
            pixel_buffer.pixels[i + horizontal_pixels_] =
                YuvToRgb(scratch_buffer[y_odd_pos], scratch_buffer[u_avg_pos], scratch_buffer[v_avg_pos]);
        }
        pixel_buffer.width = horizontal_pixels_;
        pixel_buffer.height = 2;
        return true;
    }

private:
    static float FreqToLevel(float frequency, float offset)
    {
        return 0.5f * (frequency - offset + 1.0f);
    }

    ExponentialMovingAverage low_pass_filter_;
    int horizontal_pixels_;
    int vertical_pixels_;
    int scan_line_samples_;
    int channel_samples_;
    int y_even_begin_samples_;
    int begin_samples_;
    int v_avg_begin_samples_;
    int u_avg_begin_samples_;
    int y_odd_begin_samples_;
    int end_samples_;
    std::string name_;
    int code_;
};

class Robot36Color : public BaseMode {
public:
    explicit Robot36Color(int sample_rate)
        : scan_line_samples_(static_cast<int>(std::lround((0.009 + 0.003 + 0.088 + 0.0045 + 0.0015 + 0.044) * sample_rate))),
          luminance_samples_(static_cast<int>(std::lround(0.088 * sample_rate))),
          separator_samples_(static_cast<int>(std::lround(0.0045 * sample_rate))),
          chrominance_samples_(static_cast<int>(std::lround(0.044 * sample_rate))),
          luminance_begin_samples_(static_cast<int>(std::lround(0.003 * sample_rate))),
          begin_samples_(luminance_begin_samples_),
          separator_begin_samples_(static_cast<int>(std::lround((0.003 + 0.088) * sample_rate))),
          chrominance_begin_samples_(static_cast<int>(std::lround((0.003 + 0.088 + 0.0045 + 0.0015) * sample_rate))),
          end_samples_(static_cast<int>(std::lround((0.003 + 0.088 + 0.0045 + 0.0015 + 0.044) * sample_rate)))
    {
    }

    std::string GetName() const override { return "Robot 36 Color"; }
    int GetVISCode() const override { return 8; }
    int GetWidth() const override { return 320; }
    int GetHeight() const override { return 240; }
    int GetFirstPixelSampleIndex() const override { return begin_samples_; }
    int GetFirstSyncPulseIndex() const override { return 0; }
    int GetScanLineSamples() const override { return scan_line_samples_; }

    void ResetState() override
    {
        last_even_ = false;
    }

    bool DecodeScanLine(PixelBuffer &pixel_buffer, std::vector<float> &scratch_buffer,
        const std::vector<float> &scan_line_buffer, int scope_buffer_width, int sync_pulse_index,
        int scan_line_samples, float frequency_offset) override
    {
        (void)scope_buffer_width;
        (void)scan_line_samples;
        if (sync_pulse_index + begin_samples_ < 0 ||
            sync_pulse_index + end_samples_ > static_cast<int>(scan_line_buffer.size())) {
            return false;
        }
        float separator = 0.0f;
        for (int i = 0; i < separator_samples_; ++i) {
            separator += scan_line_buffer[sync_pulse_index + separator_begin_samples_ + i];
        }
        separator /= separator_samples_;
        separator -= frequency_offset;
        bool even = separator < 0.0f;
        if (separator < -1.1f || (separator > -0.9f && separator < 0.9f) || separator > 1.1f) {
            even = !last_even_;
        }
        last_even_ = even;

        low_pass_filter_.Cutoff(GetWidth(), 2.0 * luminance_samples_, 2);
        low_pass_filter_.Reset();
        for (int i = begin_samples_; i < end_samples_; ++i) {
            scratch_buffer[i] = low_pass_filter_.Avg(scan_line_buffer[sync_pulse_index + i]);
        }
        low_pass_filter_.Reset();
        for (int i = end_samples_ - 1; i >= begin_samples_; --i) {
            scratch_buffer[i] = FreqToLevel(low_pass_filter_.Avg(scratch_buffer[i]), frequency_offset);
        }
        if (static_cast<int>(pixel_buffer.pixels.size()) < 2 * GetWidth()) {
            pixel_buffer.pixels.resize(2 * GetWidth());
        }
        for (int i = 0; i < GetWidth(); ++i) {
            int luminance_pos = luminance_begin_samples_ + (i * luminance_samples_) / GetWidth();
            int chrominance_pos = chrominance_begin_samples_ + (i * chrominance_samples_) / GetWidth();
            if (even) {
                pixel_buffer.pixels[i] = Rgb(scratch_buffer[luminance_pos], 0.0f, scratch_buffer[chrominance_pos]);
            } else {
                uint32_t even_yuv = pixel_buffer.pixels[i];
                uint32_t odd_yuv = Rgb(scratch_buffer[luminance_pos], scratch_buffer[chrominance_pos], 0.0f);
                pixel_buffer.pixels[i] = YuvPackedToRgb((even_yuv & 0x00FF00FFu) | (odd_yuv & 0x0000FF00u));
                pixel_buffer.pixels[i + GetWidth()] =
                    YuvPackedToRgb((odd_yuv & 0x00FFFF00u) | (even_yuv & 0x000000FFu));
            }
        }
        pixel_buffer.width = GetWidth();
        pixel_buffer.height = 2;
        return !even;
    }

private:
    static float FreqToLevel(float frequency, float offset)
    {
        return 0.5f * (frequency - offset + 1.0f);
    }

    ExponentialMovingAverage low_pass_filter_;
    int scan_line_samples_;
    int luminance_samples_;
    int separator_samples_;
    int chrominance_samples_;
    int luminance_begin_samples_;
    int begin_samples_;
    int separator_begin_samples_;
    int chrominance_begin_samples_;
    int end_samples_;
    bool last_even_ = false;
};

class Robot72Color : public BaseMode {
public:
    explicit Robot72Color(int sample_rate)
        : scan_line_samples_(static_cast<int>(std::lround((0.009 + 0.003 + 0.138 + 2.0 * (0.0045 + 0.0015 + 0.069)) * sample_rate))),
          luminance_samples_(static_cast<int>(std::lround(0.138 * sample_rate))),
          chrominance_samples_(static_cast<int>(std::lround(0.069 * sample_rate))),
          y_begin_samples_(static_cast<int>(std::lround(0.003 * sample_rate))),
          begin_samples_(y_begin_samples_),
          v_begin_samples_(static_cast<int>(std::lround((0.003 + 0.138 + 0.0045 + 0.0015) * sample_rate))),
          u_begin_samples_(static_cast<int>(std::lround((0.003 + 0.138 + 0.0045 + 0.0015 + 0.069 + 0.0045 + 0.0015) * sample_rate))),
          end_samples_(static_cast<int>(std::lround((0.003 + 0.138 + 0.0045 + 0.0015 + 0.069 + 0.0045 + 0.0015 + 0.069) * sample_rate)))
    {
    }

    std::string GetName() const override { return "Robot 72 Color"; }
    int GetVISCode() const override { return 12; }
    int GetWidth() const override { return 320; }
    int GetHeight() const override { return 240; }
    int GetFirstPixelSampleIndex() const override { return begin_samples_; }
    int GetFirstSyncPulseIndex() const override { return 0; }
    int GetScanLineSamples() const override { return scan_line_samples_; }

    bool DecodeScanLine(PixelBuffer &pixel_buffer, std::vector<float> &scratch_buffer,
        const std::vector<float> &scan_line_buffer, int scope_buffer_width, int sync_pulse_index,
        int scan_line_samples, float frequency_offset) override
    {
        (void)scope_buffer_width;
        (void)scan_line_samples;
        if (sync_pulse_index + begin_samples_ < 0 ||
            sync_pulse_index + end_samples_ > static_cast<int>(scan_line_buffer.size())) {
            return false;
        }
        low_pass_filter_.Cutoff(GetWidth(), 2.0 * luminance_samples_, 2);
        low_pass_filter_.Reset();
        for (int i = begin_samples_; i < end_samples_; ++i) {
            scratch_buffer[i] = low_pass_filter_.Avg(scan_line_buffer[sync_pulse_index + i]);
        }
        low_pass_filter_.Reset();
        for (int i = end_samples_ - 1; i >= begin_samples_; --i) {
            scratch_buffer[i] = FreqToLevel(low_pass_filter_.Avg(scratch_buffer[i]), frequency_offset);
        }
        if (static_cast<int>(pixel_buffer.pixels.size()) < GetWidth()) {
            pixel_buffer.pixels.resize(GetWidth());
        }
        for (int i = 0; i < GetWidth(); ++i) {
            int y_pos = y_begin_samples_ + (i * luminance_samples_) / GetWidth();
            int u_pos = u_begin_samples_ + (i * chrominance_samples_) / GetWidth();
            int v_pos = v_begin_samples_ + (i * chrominance_samples_) / GetWidth();
            pixel_buffer.pixels[i] = YuvToRgb(scratch_buffer[y_pos], scratch_buffer[u_pos], scratch_buffer[v_pos]);
        }
        pixel_buffer.width = GetWidth();
        pixel_buffer.height = 1;
        return true;
    }

private:
    static float FreqToLevel(float frequency, float offset)
    {
        return 0.5f * (frequency - offset + 1.0f);
    }

    ExponentialMovingAverage low_pass_filter_;
    int scan_line_samples_;
    int luminance_samples_;
    int chrominance_samples_;
    int y_begin_samples_;
    int begin_samples_;
    int v_begin_samples_;
    int u_begin_samples_;
    int end_samples_;
};

class HFFax : public BaseMode {
public:
    explicit HFFax(int sample_rate) : sample_rate_(sample_rate), cumulated_(GetWidth(), 0.0f) {}

    std::string GetName() const override { return "HF Fax"; }
    int GetVISCode() const override { return -1; }
    int GetWidth() const override { return 640; }
    int GetHeight() const override { return 1200; }
    int GetFirstPixelSampleIndex() const override { return 0; }
    int GetFirstSyncPulseIndex() const override { return -1; }
    int GetScanLineSamples() const override { return sample_rate_ / 2; }

    bool DecodeScanLine(PixelBuffer &pixel_buffer, std::vector<float> &scratch_buffer,
        const std::vector<float> &scan_line_buffer, int scope_buffer_width, int sync_pulse_index,
        int scan_line_samples, float frequency_offset) override
    {
        (void)scope_buffer_width;
        if (sync_pulse_index < 0 || sync_pulse_index + scan_line_samples > static_cast<int>(scan_line_buffer.size())) {
            return false;
        }
        low_pass_filter_.Cutoff(GetWidth(), 2.0 * scan_line_samples, 2);
        low_pass_filter_.Reset();
        for (int i = 0; i < scan_line_samples; ++i) {
            scratch_buffer[i] = low_pass_filter_.Avg(scan_line_buffer[sync_pulse_index + i]);
        }
        low_pass_filter_.Reset();
        for (int i = scan_line_samples - 1; i >= 0; --i) {
            scratch_buffer[i] = FreqToLevel(low_pass_filter_.Avg(scratch_buffer[i]), frequency_offset);
        }
        if (static_cast<int>(pixel_buffer.pixels.size()) < GetWidth()) {
            pixel_buffer.pixels.resize(GetWidth());
        }
        for (int i = 0; i < GetWidth(); ++i) {
            int position = (i * scan_line_samples) / GetWidth();
            uint32_t color = Gray(scratch_buffer[position]);
            pixel_buffer.pixels[i] = color;
            constexpr float decay = 0.99f;
            cumulated_[i] = cumulated_[i] * decay + Luminance(color) * (1.0f - decay);
        }
        int best_index = 0;
        float best_value = 0.0f;
        for (int x = 0; x < GetWidth(); ++x) {
            if (cumulated_[x] > best_value) {
                best_value = cumulated_[x];
                best_index = x;
            }
        }
        horizontal_shift_ = best_index;
        pixel_buffer.width = GetWidth();
        pixel_buffer.height = 1;
        return true;
    }

private:
    static float FreqToLevel(float frequency, float offset)
    {
        return 0.5f * (frequency - offset + 1.0f);
    }

    ExponentialMovingAverage low_pass_filter_;
    int sample_rate_;
    std::vector<float> cumulated_;
    int horizontal_shift_ = 0;
};

std::unique_ptr<RGBDecoder> MakeMartin(const std::string &name, int code, double channel_seconds, int sample_rate)
{
    double sync_pulse_seconds = 0.004862;
    double separator_seconds = 0.000572;
    double scan_line_seconds = sync_pulse_seconds + separator_seconds + 3.0 * (channel_seconds + separator_seconds);
    double green_begin_seconds = separator_seconds;
    double green_end_seconds = green_begin_seconds + channel_seconds;
    double blue_begin_seconds = green_end_seconds + separator_seconds;
    double blue_end_seconds = blue_begin_seconds + channel_seconds;
    double red_begin_seconds = blue_end_seconds + separator_seconds;
    double red_end_seconds = red_begin_seconds + channel_seconds;
    return std::make_unique<RGBDecoder>("Martin " + name, code, 320, 256, 0.0, scan_line_seconds, green_begin_seconds,
        red_begin_seconds, red_end_seconds, green_begin_seconds, green_end_seconds, blue_begin_seconds,
        blue_end_seconds, red_end_seconds, sample_rate);
}

std::unique_ptr<RGBDecoder> MakeScottie(const std::string &name, int code, double channel_seconds, int sample_rate)
{
    double sync_pulse_seconds = 0.009;
    double separator_seconds = 0.0015;
    double first_sync_pulse_seconds = sync_pulse_seconds + 2.0 * (separator_seconds + channel_seconds);
    double scan_line_seconds = sync_pulse_seconds + 3.0 * (channel_seconds + separator_seconds);
    double blue_end_seconds = -sync_pulse_seconds;
    double blue_begin_seconds = blue_end_seconds - channel_seconds;
    double green_end_seconds = blue_begin_seconds - separator_seconds;
    double green_begin_seconds = green_end_seconds - channel_seconds;
    double red_begin_seconds = separator_seconds;
    double red_end_seconds = red_begin_seconds + channel_seconds;
    return std::make_unique<RGBDecoder>("Scottie " + name, code, 320, 256, first_sync_pulse_seconds,
        scan_line_seconds, green_begin_seconds, red_begin_seconds, red_end_seconds, green_begin_seconds,
        green_end_seconds, blue_begin_seconds, blue_end_seconds, red_end_seconds, sample_rate);
}

std::unique_ptr<RGBDecoder> MakeWraaseSc2180(int sample_rate)
{
    double sync_pulse_seconds = 0.0055225;
    double sync_porch_seconds = 0.0005;
    double channel_seconds = 0.235;
    double scan_line_seconds = sync_pulse_seconds + sync_porch_seconds + 3.0 * channel_seconds;
    double red_begin_seconds = sync_porch_seconds;
    double red_end_seconds = red_begin_seconds + channel_seconds;
    double green_begin_seconds = red_end_seconds;
    double green_end_seconds = green_begin_seconds + channel_seconds;
    double blue_begin_seconds = green_end_seconds;
    double blue_end_seconds = blue_begin_seconds + channel_seconds;
    return std::make_unique<RGBDecoder>("Wraase SC2-180", 55, 320, 256, 0.0, scan_line_seconds, red_begin_seconds,
        red_begin_seconds, red_end_seconds, green_begin_seconds, green_end_seconds, blue_begin_seconds,
        blue_end_seconds, blue_end_seconds, sample_rate);
}

class DecoderImpl {
public:
    DecoderImpl(PixelBuffer scope_buffer, PixelBuffer image_buffer, std::string raw_name, int sample_rate)
        : scope_buffer_(std::move(scope_buffer)),
          image_buffer_(std::move(image_buffer)),
          pixel_buffer_(800, 2),
          demodulator_(sample_rate),
          pulse_filter_(static_cast<int>(std::lround(0.0025 * sample_rate)) | 1),
          pulse_filter_delay_((pulse_filter_.Length() - 1) / 2),
          scan_line_buffer_(static_cast<size_t>(std::lround(7.0 * sample_rate)), 0.0f),
          scratch_buffer_(static_cast<size_t>(std::lround(1.1 * sample_rate)), 0.0f),
          leader_tone_samples_(static_cast<int>(std::lround(0.3 * sample_rate))),
          leader_tone_tolerance_samples_(static_cast<int>(std::lround(0.3 * 0.2 * sample_rate))),
          transition_samples_(static_cast<int>(std::lround(0.0005 * sample_rate))),
          vis_code_bit_samples_(static_cast<int>(std::lround(0.03 * sample_rate))),
          vis_code_samples_(static_cast<int>(std::lround(0.3 * sample_rate))),
          vis_code_bit_frequencies_(10, 0.0f),
          last_5ms_sync_pulses_(5, 0),
          last_9ms_sync_pulses_(5, 0),
          last_20ms_sync_pulses_(5, 0),
          last_5ms_scan_lines_(4, 0),
          last_9ms_scan_lines_(4, 0),
          last_20ms_scan_lines_(4, 0),
          last_5ms_frequency_offsets_(5, 0.0f),
          last_9ms_frequency_offsets_(5, 0.0f),
          last_20ms_frequency_offsets_(5, 0.0f),
          scan_line_min_samples_(static_cast<int>(std::lround(0.05 * sample_rate))),
          sync_pulse_tolerance_samples_(static_cast<int>(std::lround(0.03 * sample_rate))),
          scan_line_tolerance_samples_(static_cast<int>(std::lround(0.001 * sample_rate)))
    {
        image_buffer_.line = -1;
        raw_mode_ = AddMode(std::make_unique<RawDecoder>(std::move(raw_name), sample_rate));
        hf_fax_mode_ = AddMode(std::make_unique<HFFax>(sample_rate));
        Mode *robot36 = AddMode(std::make_unique<Robot36Color>(sample_rate));
        current_mode_ = robot36;
        current_scan_line_samples_ = robot36->GetScanLineSamples();

        sync_pulse_5ms_modes_.push_back(AddMode(MakeWraaseSc2180(sample_rate)));
        sync_pulse_5ms_modes_.push_back(AddMode(MakeMartin("1", 44, 0.146432, sample_rate)));
        sync_pulse_5ms_modes_.push_back(AddMode(MakeMartin("2", 40, 0.073216, sample_rate)));

        sync_pulse_9ms_modes_.push_back(robot36);
        sync_pulse_9ms_modes_.push_back(AddMode(std::make_unique<Robot72Color>(sample_rate)));
        sync_pulse_9ms_modes_.push_back(AddMode(MakeScottie("1", 60, 0.138240, sample_rate)));
        sync_pulse_9ms_modes_.push_back(AddMode(MakeScottie("2", 56, 0.088064, sample_rate)));
        sync_pulse_9ms_modes_.push_back(AddMode(MakeScottie("DX", 76, 0.3456, sample_rate)));

        sync_pulse_20ms_modes_.push_back(AddMode(std::make_unique<PaulDon>("50", 93, 320, 256, 0.09152, sample_rate)));
        sync_pulse_20ms_modes_.push_back(AddMode(std::make_unique<PaulDon>("90", 99, 320, 256, 0.17024, sample_rate)));
        sync_pulse_20ms_modes_.push_back(AddMode(std::make_unique<PaulDon>("120", 95, 640, 496, 0.1216, sample_rate)));
        sync_pulse_20ms_modes_.push_back(AddMode(std::make_unique<PaulDon>("160", 98, 512, 400, 0.195584, sample_rate)));
        sync_pulse_20ms_modes_.push_back(AddMode(std::make_unique<PaulDon>("180", 96, 640, 496, 0.18304, sample_rate)));
        sync_pulse_20ms_modes_.push_back(AddMode(std::make_unique<PaulDon>("240", 97, 640, 496, 0.24448, sample_rate)));
        sync_pulse_20ms_modes_.push_back(AddMode(std::make_unique<PaulDon>("290", 94, 800, 616, 0.2288, sample_rate)));
    }

    bool Process(std::vector<float> &record_buffer, int channel_select)
    {
        bool new_lines_present = false;
        bool sync_pulse_detected = demodulator_.Process(record_buffer, channel_select);
        if (sync_pulse_detected) {
            ++sync_pulse_detection_count_;
        }
        int sync_pulse_index = current_sample_ + demodulator_.sync_pulse_offset;
        int channels = channel_select > 0 ? 2 : 1;
        for (int j = 0; j < static_cast<int>(record_buffer.size()) / channels; ++j) {
            scan_line_buffer_[current_sample_++] = record_buffer[j];
            if (current_sample_ >= static_cast<int>(scan_line_buffer_.size())) {
                ShiftSamples(current_scan_line_samples_);
                sync_pulse_index -= current_scan_line_samples_;
            }
        }
        if (sync_pulse_detected) {
            switch (demodulator_.sync_pulse_width) {
                case Demodulator::SyncPulseWidth::FiveMilliSeconds:
                    new_lines_present = ProcessSyncPulse(sync_pulse_5ms_modes_, last_5ms_frequency_offsets_,
                        last_5ms_sync_pulses_, last_5ms_scan_lines_, sync_pulse_index);
                    break;
                case Demodulator::SyncPulseWidth::NineMilliSeconds:
                    leader_break_index_ = sync_pulse_index;
                    new_lines_present = ProcessSyncPulse(sync_pulse_9ms_modes_, last_9ms_frequency_offsets_,
                        last_9ms_sync_pulses_, last_9ms_scan_lines_, sync_pulse_index);
                    break;
                case Demodulator::SyncPulseWidth::TwentyMilliSeconds:
                    leader_break_index_ = sync_pulse_index;
                    new_lines_present = ProcessSyncPulse(sync_pulse_20ms_modes_, last_20ms_frequency_offsets_,
                        last_20ms_sync_pulses_, last_20ms_scan_lines_, sync_pulse_index);
                    break;
            }
        } else if (HandleHeader()) {
            ++header_detection_count_;
            new_lines_present = true;
        } else if (current_sample_ > last_sync_pulse_index_ + (current_scan_line_samples_ * 5) / 4) {
            CopyLines(current_mode_->DecodeScanLine(pixel_buffer_, scratch_buffer_, scan_line_buffer_, scope_buffer_.width,
                last_sync_pulse_index_, current_scan_line_samples_, last_frequency_offset_));
            last_sync_pulse_index_ += current_scan_line_samples_;
            new_lines_present = true;
        }

        if (image_buffer_.line >= image_buffer_.height && image_buffer_.height > 0) {
            completed_image_ = image_buffer_;
            has_completed_image_ = true;
            ++completed_images_;
            image_buffer_.line = -1;
        }

        return new_lines_present;
    }

    void SetMode(const std::string &name)
    {
        if (raw_mode_->GetName() == name) {
            lock_mode_ = true;
            image_buffer_.line = -1;
            current_mode_ = raw_mode_;
            return;
        }
        Mode *mode = FindMode(sync_pulse_5ms_modes_, name);
        if (mode == nullptr) {
            mode = FindMode(sync_pulse_9ms_modes_, name);
        }
        if (mode == nullptr) {
            mode = FindMode(sync_pulse_20ms_modes_, name);
        }
        if (mode == nullptr && hf_fax_mode_->GetName() == name) {
            mode = hf_fax_mode_;
        }
        if (mode == current_mode_) {
            lock_mode_ = true;
            return;
        }
        if (mode != nullptr) {
            lock_mode_ = true;
            image_buffer_.line = -1;
            current_mode_ = mode;
            current_scan_line_samples_ = mode->GetScanLineSamples();
            return;
        }
        lock_mode_ = false;
    }

    std::string GetCurrentModeName() const
    {
        return current_mode_ == nullptr ? std::string() : current_mode_->GetName();
    }

    bool ConsumeCompletedImage(PixelBuffer &out_image)
    {
        if (!has_completed_image_) {
            return false;
        }
        out_image = completed_image_;
        has_completed_image_ = false;
        return true;
    }

    PixelBuffer &GetImageBuffer() { return image_buffer_; }
    const PixelBuffer &GetImageBuffer() const { return image_buffer_; }
    PixelBuffer &GetScopeBuffer() { return scope_buffer_; }
    const PixelBuffer &GetScopeBuffer() const { return scope_buffer_; }
    int GetCompletedImageCount() const { return completed_images_; }
    int GetSyncPulseDetectionCount() const { return sync_pulse_detection_count_; }
    int GetHeaderDetectionCount() const { return header_detection_count_; }
    int GetImageLine() const { return image_buffer_.line; }

private:
    Mode *AddMode(std::unique_ptr<Mode> mode)
    {
        Mode *raw_ptr = mode.get();
        mode_storage_.push_back(std::move(mode));
        return raw_ptr;
    }

    static double ScanLineMean(const std::vector<int> &lines)
    {
        double mean = 0.0;
        for (int diff : lines) {
            mean += diff;
        }
        return mean / static_cast<double>(lines.size());
    }

    static double ScanLineStdDev(const std::vector<int> &lines, double mean)
    {
        double std_dev = 0.0;
        for (int diff : lines) {
            std_dev += (diff - mean) * (diff - mean);
        }
        return std::sqrt(std_dev / static_cast<double>(lines.size()));
    }

    static double FrequencyOffsetMean(const std::vector<float> &offsets)
    {
        double mean = 0.0;
        for (float offset : offsets) {
            mean += offset;
        }
        return mean / static_cast<double>(offsets.size());
    }

    Mode *DetectMode(const std::vector<Mode *> &modes, int line) const
    {
        Mode *best_mode = raw_mode_;
        int best_distance = std::numeric_limits<int>::max();
        for (Mode *mode : modes) {
            int distance = std::abs(line - mode->GetScanLineSamples());
            if (distance <= scan_line_tolerance_samples_ && distance < best_distance) {
                best_distance = distance;
                best_mode = mode;
            }
        }
        return best_mode;
    }

    static Mode *FindMode(const std::vector<Mode *> &modes, int code)
    {
        for (Mode *mode : modes) {
            if (mode->GetVISCode() == code) {
                return mode;
            }
        }
        return nullptr;
    }

    static Mode *FindMode(const std::vector<Mode *> &modes, const std::string &name)
    {
        for (Mode *mode : modes) {
            if (mode->GetName() == name) {
                return mode;
            }
        }
        return nullptr;
    }

    void CopyUnscaled()
    {
        int width = std::min(scope_buffer_.width, pixel_buffer_.width);
        for (int row = 0; row < pixel_buffer_.height; ++row) {
            int line = scope_buffer_.width * scope_buffer_.line;
            std::copy(pixel_buffer_.pixels.begin() + row * pixel_buffer_.width,
                pixel_buffer_.pixels.begin() + row * pixel_buffer_.width + width, scope_buffer_.pixels.begin() + line);
            std::fill(scope_buffer_.pixels.begin() + line + width, scope_buffer_.pixels.begin() + line + scope_buffer_.width, 0u);
            std::copy(scope_buffer_.pixels.begin() + line, scope_buffer_.pixels.begin() + line + scope_buffer_.width,
                scope_buffer_.pixels.begin() + scope_buffer_.width * (scope_buffer_.line + scope_buffer_.height / 2));
            scope_buffer_.line = (scope_buffer_.line + 1) % (scope_buffer_.height / 2);
        }
    }

    void CopyScaled(int scale)
    {
        for (int row = 0; row < pixel_buffer_.height; ++row) {
            int line = scope_buffer_.width * scope_buffer_.line;
            for (int col = 0; col < pixel_buffer_.width; ++col) {
                for (int i = 0; i < scale; ++i) {
                    scope_buffer_.pixels[line + col * scale + i] = pixel_buffer_.pixels[pixel_buffer_.width * row + col];
                }
            }
            std::fill(scope_buffer_.pixels.begin() + line + pixel_buffer_.width * scale,
                scope_buffer_.pixels.begin() + line + scope_buffer_.width, 0u);
            std::copy(scope_buffer_.pixels.begin() + line, scope_buffer_.pixels.begin() + line + scope_buffer_.width,
                scope_buffer_.pixels.begin() + scope_buffer_.width * (scope_buffer_.line + scope_buffer_.height / 2));
            scope_buffer_.line = (scope_buffer_.line + 1) % (scope_buffer_.height / 2);
            for (int i = 1; i < scale; ++i) {
                int destination_line = scope_buffer_.width * scope_buffer_.line;
                std::copy(scope_buffer_.pixels.begin() + line, scope_buffer_.pixels.begin() + line + scope_buffer_.width,
                    scope_buffer_.pixels.begin() + destination_line);
                std::copy(scope_buffer_.pixels.begin() + line, scope_buffer_.pixels.begin() + line + scope_buffer_.width,
                    scope_buffer_.pixels.begin() + scope_buffer_.width * (scope_buffer_.line + scope_buffer_.height / 2));
                scope_buffer_.line = (scope_buffer_.line + 1) % (scope_buffer_.height / 2);
            }
        }
    }

    void CopyLines(bool okay)
    {
        if (!okay) {
            return;
        }
        bool finish = false;
        if (image_buffer_.line >= 0 && image_buffer_.line < image_buffer_.height &&
            image_buffer_.width == pixel_buffer_.width) {
            int width = image_buffer_.width;
            for (int row = 0; row < pixel_buffer_.height && image_buffer_.line < image_buffer_.height; ++row, ++image_buffer_.line) {
                std::copy(pixel_buffer_.pixels.begin() + row * width, pixel_buffer_.pixels.begin() + row * width + width,
                    image_buffer_.pixels.begin() + image_buffer_.line * width);
            }
            finish = image_buffer_.line == image_buffer_.height;
        }
        int scale = scope_buffer_.width / pixel_buffer_.width;
        if (scale <= 1) {
            CopyUnscaled();
        } else {
            CopyScaled(scale);
        }
        if (finish) {
            DrawLines(0xFF000000u, 10);
        }
    }

    void DrawLines(uint32_t color, int count)
    {
        for (int i = 0; i < count; ++i) {
            std::fill(scope_buffer_.pixels.begin() + scope_buffer_.line * scope_buffer_.width,
                scope_buffer_.pixels.begin() + (scope_buffer_.line + 1) * scope_buffer_.width, color);
            std::fill(scope_buffer_.pixels.begin() + (scope_buffer_.line + scope_buffer_.height / 2) * scope_buffer_.width,
                scope_buffer_.pixels.begin() + (scope_buffer_.line + 1 + scope_buffer_.height / 2) * scope_buffer_.width, color);
            scope_buffer_.line = (scope_buffer_.line + 1) % (scope_buffer_.height / 2);
        }
    }

    static void AdjustSyncPulses(std::vector<int> &pulses, int shift)
    {
        for (int &pulse : pulses) {
            pulse -= shift;
        }
    }

    void ShiftSamples(int shift)
    {
        if (shift <= 0 || shift > current_sample_) {
            return;
        }
        current_sample_ -= shift;
        leader_break_index_ -= shift;
        last_sync_pulse_index_ -= shift;
        AdjustSyncPulses(last_5ms_sync_pulses_, shift);
        AdjustSyncPulses(last_9ms_sync_pulses_, shift);
        AdjustSyncPulses(last_20ms_sync_pulses_, shift);
        std::copy(scan_line_buffer_.begin() + shift, scan_line_buffer_.begin() + shift + current_sample_, scan_line_buffer_.begin());
    }

    bool HandleHeader()
    {
        if (leader_break_index_ < vis_code_bit_samples_ + leader_tone_tolerance_samples_ ||
            current_sample_ < leader_break_index_ + leader_tone_samples_ + leader_tone_tolerance_samples_ +
            vis_code_samples_ + vis_code_bit_samples_) {
            return false;
        }
        int break_pulse_index = leader_break_index_;
        leader_break_index_ = 0;
        float pre_break_freq = 0.0f;
        for (int i = 0; i < leader_tone_tolerance_samples_; ++i) {
            pre_break_freq += scan_line_buffer_[break_pulse_index - vis_code_bit_samples_ - leader_tone_tolerance_samples_ + i];
        }
        constexpr float leader_tone_frequency = 1900.0f;
        constexpr float center_frequency = 1900.0f;
        constexpr float tolerance_frequency = 50.0f;
        constexpr float half_bandwidth = 400.0f;
        pre_break_freq = pre_break_freq * half_bandwidth / leader_tone_tolerance_samples_ + center_frequency;
        if (std::fabs(pre_break_freq - leader_tone_frequency) > tolerance_frequency) {
            return false;
        }
        float leader_freq = 0.0f;
        for (int i = transition_samples_; i < leader_tone_samples_ - leader_tone_tolerance_samples_; ++i) {
            leader_freq += scan_line_buffer_[break_pulse_index + i];
        }
        float leader_freq_offset =
            leader_freq / static_cast<float>(leader_tone_samples_ - transition_samples_ - leader_tone_tolerance_samples_);
        leader_freq = leader_freq_offset * half_bandwidth + center_frequency;
        if (std::fabs(leader_freq - leader_tone_frequency) > tolerance_frequency) {
            return false;
        }
        constexpr float stop_bit_frequency = 1200.0f;
        float pulse_threshold_frequency = (stop_bit_frequency + leader_tone_frequency) / 2.0f;
        float pulse_threshold_value = (pulse_threshold_frequency - center_frequency) / half_bandwidth;
        int vis_begin_index = break_pulse_index + leader_tone_samples_ - leader_tone_tolerance_samples_;
        int vis_end_index = break_pulse_index + leader_tone_samples_ + leader_tone_tolerance_samples_ + vis_code_bit_samples_;
        for (int i = 0; i < pulse_filter_.Length(); ++i) {
            pulse_filter_.Avg(scan_line_buffer_[vis_begin_index++] - leader_freq_offset);
        }
        while (++vis_begin_index < vis_end_index) {
            if (pulse_filter_.Avg(scan_line_buffer_[vis_begin_index] - leader_freq_offset) < pulse_threshold_value) {
                break;
            }
        }
        if (vis_begin_index >= vis_end_index) {
            return false;
        }
        vis_begin_index -= pulse_filter_delay_;
        vis_end_index = vis_begin_index + vis_code_samples_;
        std::fill(vis_code_bit_frequencies_.begin(), vis_code_bit_frequencies_.end(), 0.0f);
        for (int j = 0; j < 10; ++j) {
            for (int i = transition_samples_; i < vis_code_bit_samples_ - transition_samples_; ++i) {
                vis_code_bit_frequencies_[j] += scan_line_buffer_[vis_begin_index + vis_code_bit_samples_ * j + i] - leader_freq_offset;
            }
        }
        for (float &frequency : vis_code_bit_frequencies_) {
            frequency = frequency * half_bandwidth / static_cast<float>(vis_code_bit_samples_ - 2 * transition_samples_) +
                center_frequency;
        }
        if (std::fabs(vis_code_bit_frequencies_[0] - stop_bit_frequency) > tolerance_frequency ||
            std::fabs(vis_code_bit_frequencies_[9] - stop_bit_frequency) > tolerance_frequency) {
            return false;
        }
        constexpr float one_bit_frequency = 1100.0f;
        constexpr float zero_bit_frequency = 1300.0f;
        for (int i = 1; i < 9; ++i) {
            if (std::fabs(vis_code_bit_frequencies_[i] - one_bit_frequency) > tolerance_frequency &&
                std::fabs(vis_code_bit_frequencies_[i] - zero_bit_frequency) > tolerance_frequency) {
                return false;
            }
        }
        int vis_code = 0;
        for (int i = 0; i < 8; ++i) {
            vis_code |= (vis_code_bit_frequencies_[i + 1] < stop_bit_frequency ? 1 : 0) << i;
        }
        bool check = true;
        for (int i = 0; i < 8; ++i) {
            check ^= (vis_code & (1 << i)) != 0;
        }
        vis_code &= 127;
        if (!check) {
            return false;
        }
        constexpr float sync_porch_frequency = 1500.0f;
        constexpr float sync_pulse_frequency = 1200.0f;
        float sync_threshold_frequency = (sync_pulse_frequency + sync_porch_frequency) / 2.0f;
        float sync_threshold_value = (sync_threshold_frequency - center_frequency) / half_bandwidth;
        int sync_pulse_index = vis_end_index - vis_code_bit_samples_;
        int sync_pulse_max_index = vis_end_index + vis_code_bit_samples_;
        for (int i = 0; i < pulse_filter_.Length(); ++i) {
            pulse_filter_.Avg(scan_line_buffer_[sync_pulse_index++] - leader_freq_offset);
        }
        while (++sync_pulse_index < sync_pulse_max_index) {
            if (pulse_filter_.Avg(scan_line_buffer_[sync_pulse_index] - leader_freq_offset) > sync_threshold_value) {
                break;
            }
        }
        if (sync_pulse_index >= sync_pulse_max_index) {
            return false;
        }
        sync_pulse_index -= pulse_filter_delay_;
        Mode *mode = nullptr;
        std::vector<int> *pulses = nullptr;
        std::vector<int> *lines = nullptr;
        if ((mode = FindMode(sync_pulse_5ms_modes_, vis_code)) != nullptr) {
            pulses = &last_5ms_sync_pulses_;
            lines = &last_5ms_scan_lines_;
        } else if ((mode = FindMode(sync_pulse_9ms_modes_, vis_code)) != nullptr) {
            pulses = &last_9ms_sync_pulses_;
            lines = &last_9ms_scan_lines_;
        } else if ((mode = FindMode(sync_pulse_20ms_modes_, vis_code)) != nullptr) {
            pulses = &last_20ms_sync_pulses_;
            lines = &last_20ms_scan_lines_;
        } else {
            if (!lock_mode_) {
                DrawLines(0xFFFF0000u, 8);
            }
            return false;
        }
        if (lock_mode_ && mode != current_mode_) {
            return false;
        }
        mode->ResetState();
        image_buffer_.width = mode->GetWidth();
        image_buffer_.height = mode->GetHeight();
        image_buffer_.pixels.resize(static_cast<size_t>(image_buffer_.width * image_buffer_.height));
        image_buffer_.line = 0;
        current_mode_ = mode;
        last_sync_pulse_index_ = sync_pulse_index + mode->GetFirstSyncPulseIndex();
        current_scan_line_samples_ = mode->GetScanLineSamples();
        last_frequency_offset_ = leader_freq_offset;
        int oldest_sync_pulse_index = last_sync_pulse_index_ - (static_cast<int>(pulses->size()) - 1) * current_scan_line_samples_;
        if (mode->GetFirstSyncPulseIndex() > 0) {
            oldest_sync_pulse_index -= current_scan_line_samples_;
        }
        for (size_t i = 0; i < pulses->size(); ++i) {
            (*pulses)[i] = oldest_sync_pulse_index + static_cast<int>(i) * current_scan_line_samples_;
        }
        std::fill(lines->begin(), lines->end(), current_scan_line_samples_);
        ShiftSamples(last_sync_pulse_index_ + mode->GetFirstPixelSampleIndex());
        DrawLines(0xFF00FF00u, 8);
        DrawLines(0xFF000000u, 10);
        return true;
    }

    bool ProcessSyncPulse(const std::vector<Mode *> &modes, std::vector<float> &frequency_offsets,
        std::vector<int> &sync_indexes, std::vector<int> &line_lengths, int latest_sync_index)
    {
        for (size_t i = 1; i < sync_indexes.size(); ++i) {
            sync_indexes[i - 1] = sync_indexes[i];
        }
        sync_indexes.back() = latest_sync_index;
        for (size_t i = 1; i < line_lengths.size(); ++i) {
            line_lengths[i - 1] = line_lengths[i];
        }
        line_lengths.back() = sync_indexes.back() - sync_indexes[sync_indexes.size() - 2];
        for (size_t i = 1; i < frequency_offsets.size(); ++i) {
            frequency_offsets[i - 1] = frequency_offsets[i];
        }
        frequency_offsets.back() = demodulator_.frequency_offset;
        if (line_lengths.front() == 0) {
            return false;
        }
        double mean = ScanLineMean(line_lengths);
        int scan_line_samples = static_cast<int>(std::lround(mean));
        if (scan_line_samples < scan_line_min_samples_ || scan_line_samples > static_cast<int>(scratch_buffer_.size())) {
            return false;
        }
        if (ScanLineStdDev(line_lengths, mean) > scan_line_tolerance_samples_) {
            return false;
        }
        bool picture_changed = false;
        if (lock_mode_ || (image_buffer_.line >= 0 && image_buffer_.line < image_buffer_.height)) {
            if (current_mode_ != raw_mode_ &&
                std::abs(scan_line_samples - current_mode_->GetScanLineSamples()) > scan_line_tolerance_samples_) {
                return false;
            }
        } else {
            Mode *previous_mode = current_mode_;
            current_mode_ = DetectMode(modes, scan_line_samples);
            picture_changed = current_mode_ != previous_mode ||
                std::abs(current_scan_line_samples_ - scan_line_samples) > scan_line_tolerance_samples_ ||
                std::abs(last_sync_pulse_index_ + scan_line_samples - sync_indexes.back()) > sync_pulse_tolerance_samples_;
        }
        if (picture_changed) {
            DrawLines(0xFF000000u, 10);
            DrawLines(0xFF00FFFFu, 8);
            DrawLines(0xFF000000u, 10);
        }
        float frequency_offset = static_cast<float>(FrequencyOffsetMean(frequency_offsets));
        if (sync_indexes.front() >= scan_line_samples && picture_changed) {
            int end_pulse = sync_indexes.front();
            int extrapolate = end_pulse / scan_line_samples;
            int first_pulse = end_pulse - extrapolate * scan_line_samples;
            for (int pulse_index = first_pulse; pulse_index < end_pulse; pulse_index += scan_line_samples) {
                CopyLines(current_mode_->DecodeScanLine(pixel_buffer_, scratch_buffer_, scan_line_buffer_, scope_buffer_.width,
                    pulse_index, scan_line_samples, frequency_offset));
            }
        }
        for (size_t i = picture_changed ? 0 : line_lengths.size() - 1; i < line_lengths.size(); ++i) {
            CopyLines(current_mode_->DecodeScanLine(pixel_buffer_, scratch_buffer_, scan_line_buffer_, scope_buffer_.width,
                sync_indexes[i], line_lengths[i], frequency_offset));
        }
        last_sync_pulse_index_ = sync_indexes.back();
        current_scan_line_samples_ = scan_line_samples;
        last_frequency_offset_ = frequency_offset;
        ShiftSamples(last_sync_pulse_index_ + current_mode_->GetFirstPixelSampleIndex());
        return true;
    }

    PixelBuffer scope_buffer_;
    PixelBuffer image_buffer_;
    PixelBuffer pixel_buffer_;
    PixelBuffer completed_image_ {1, 1};
    bool has_completed_image_ = false;
    int completed_images_ = 0;

    Demodulator demodulator_;
    SimpleMovingAverage pulse_filter_;
    int pulse_filter_delay_;
    std::vector<float> scan_line_buffer_;
    std::vector<float> scratch_buffer_;
    int leader_tone_samples_;
    int leader_tone_tolerance_samples_;
    int transition_samples_;
    int vis_code_bit_samples_;
    int vis_code_samples_;
    std::vector<float> vis_code_bit_frequencies_;
    std::vector<int> last_5ms_sync_pulses_;
    std::vector<int> last_9ms_sync_pulses_;
    std::vector<int> last_20ms_sync_pulses_;
    std::vector<int> last_5ms_scan_lines_;
    std::vector<int> last_9ms_scan_lines_;
    std::vector<int> last_20ms_scan_lines_;
    std::vector<float> last_5ms_frequency_offsets_;
    std::vector<float> last_9ms_frequency_offsets_;
    std::vector<float> last_20ms_frequency_offsets_;
    int scan_line_min_samples_;
    int sync_pulse_tolerance_samples_;
    int scan_line_tolerance_samples_;

    std::vector<std::unique_ptr<Mode>> mode_storage_;
    Mode *raw_mode_ = nullptr;
    Mode *hf_fax_mode_ = nullptr;
    std::vector<Mode *> sync_pulse_5ms_modes_;
    std::vector<Mode *> sync_pulse_9ms_modes_;
    std::vector<Mode *> sync_pulse_20ms_modes_;

    Mode *current_mode_ = nullptr;
    bool lock_mode_ = false;
    int current_sample_ = 0;
    int leader_break_index_ = 0;
    int last_sync_pulse_index_ = 0;
    int current_scan_line_samples_ = 0;
    float last_frequency_offset_ = 0.0f;
    int sync_pulse_detection_count_ = 0;
    int header_detection_count_ = 0;
};

uint16_t ReadLe16(std::istream &input)
{
    uint8_t bytes[2];
    input.read(reinterpret_cast<char *>(bytes), 2);
    return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

uint32_t ReadLe32(std::istream &input)
{
    uint8_t bytes[4];
    input.read(reinterpret_cast<char *>(bytes), 4);
    return static_cast<uint32_t>(bytes[0] | (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24));
}

std::string BaseNameWithoutExtension(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < start) {
        dot = path.size();
    }
    return path.substr(start, dot - start);
}

}  // namespace

PixelBuffer::PixelBuffer(int width, int height) : pixels(static_cast<size_t>(width * height), 0u), width(width), height(height), line(0)
{
}

class Decoder::Impl : public DecoderImpl {
public:
    using DecoderImpl::DecoderImpl;
};

Decoder::Decoder(PixelBuffer scope_buffer, PixelBuffer image_buffer, std::string raw_name, int sample_rate)
    : impl_(std::make_unique<Impl>(std::move(scope_buffer), std::move(image_buffer), std::move(raw_name), sample_rate))
{
}

Decoder::~Decoder() = default;

bool Decoder::Process(std::vector<float> &record_buffer, int channel_select)
{
    return impl_->Process(record_buffer, channel_select);
}

void Decoder::SetMode(const std::string &name)
{
    impl_->SetMode(name);
}

std::string Decoder::GetCurrentModeName() const
{
    return impl_->GetCurrentModeName();
}

PixelBuffer &Decoder::GetImageBuffer()
{
    return impl_->GetImageBuffer();
}

const PixelBuffer &Decoder::GetImageBuffer() const
{
    return impl_->GetImageBuffer();
}

PixelBuffer &Decoder::GetScopeBuffer()
{
    return impl_->GetScopeBuffer();
}

const PixelBuffer &Decoder::GetScopeBuffer() const
{
    return impl_->GetScopeBuffer();
}

bool Decoder::ConsumeCompletedImage(PixelBuffer &out_image)
{
    return impl_->ConsumeCompletedImage(out_image);
}

int Decoder::GetCompletedImageCount() const
{
    return impl_->GetCompletedImageCount();
}

int Decoder::GetSyncPulseDetectionCount() const
{
    return impl_->GetSyncPulseDetectionCount();
}

int Decoder::GetHeaderDetectionCount() const
{
    return impl_->GetHeaderDetectionCount();
}

int Decoder::GetImageLine() const
{
    return impl_->GetImageLine();
}

bool LoadWavFile(const std::string &path, WavData &out_wav, std::string &error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "Failed to open WAV file";
        return false;
    }

    char riff[4];
    input.read(riff, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0) {
        error = "Missing RIFF header";
        return false;
    }
    (void)ReadLe32(input);
    char wave[4];
    input.read(wave, 4);
    if (std::strncmp(wave, "WAVE", 4) != 0) {
        error = "Missing WAVE header";
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    uint16_t audio_format = 0;
    uint32_t data_size = 0;
    std::streampos data_offset = 0;
    while (input && (!have_fmt || !have_data)) {
        char chunk_id[4];
        input.read(chunk_id, 4);
        if (input.gcount() != 4) {
            break;
        }
        uint32_t chunk_size = ReadLe32(input);
        std::streampos next_chunk = input.tellg();
        next_chunk += static_cast<std::streamoff>(chunk_size + (chunk_size & 1u));
        if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
            audio_format = ReadLe16(input);
            out_wav.channel_count = static_cast<int>(ReadLe16(input));
            out_wav.sample_rate = static_cast<int>(ReadLe32(input));
            (void)ReadLe32(input);
            (void)ReadLe16(input);
            out_wav.bits_per_sample = static_cast<int>(ReadLe16(input));
            have_fmt = true;
        } else if (std::strncmp(chunk_id, "data", 4) == 0) {
            data_offset = input.tellg();
            data_size = chunk_size;
            have_data = true;
        }
        input.seekg(next_chunk);
    }

    if (!have_fmt || !have_data) {
        error = "WAV is missing fmt or data chunk";
        return false;
    }
    if (audio_format != 1) {
        error = "Only PCM WAV files are supported";
        return false;
    }
    if (out_wav.bits_per_sample != 16) {
        error = "Only 16-bit PCM WAV files are supported";
        return false;
    }
    if (out_wav.channel_count <= 0) {
        error = "Invalid channel count";
        return false;
    }

    input.clear();
    input.seekg(data_offset);
    size_t sample_count = data_size / sizeof(int16_t);
    std::vector<int16_t> pcm(sample_count);
    input.read(reinterpret_cast<char *>(pcm.data()), static_cast<std::streamsize>(data_size));
    std::streamsize bytes_read = input.gcount();
    if (bytes_read <= 0) {
        error = "Failed to read PCM payload";
        return false;
    }
    sample_count = static_cast<size_t>(bytes_read) / sizeof(int16_t);
    out_wav.samples.resize(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        out_wav.samples[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    return true;
}

bool SavePixelBufferAsPpm(const PixelBuffer &buffer, const std::string &path, std::string &error)
{
    if (buffer.width <= 0 || buffer.height <= 0 || static_cast<int>(buffer.pixels.size()) < buffer.width * buffer.height) {
        error = "Pixel buffer is empty";
        return false;
    }

    std::ofstream output(path, std::ios::binary);
    if (!output.is_open()) {
        error = "Failed to open output file";
        return false;
    }
    output << "P6\n" << buffer.width << " " << buffer.height << "\n255\n";
    for (int y = 0; y < buffer.height; ++y) {
        for (int x = 0; x < buffer.width; ++x) {
            uint32_t argb = buffer.pixels[static_cast<size_t>(y * buffer.width + x)];
            char rgb[3] = {
                static_cast<char>((argb >> 16) & 0xFFu),
                static_cast<char>((argb >> 8) & 0xFFu),
                static_cast<char>(argb & 0xFFu),
            };
            output.write(rgb, 3);
        }
    }
    if (!output.good()) {
        error = "Failed while writing image";
        return false;
    }
    return true;
}

OfflineDecodeResult DecodeWavFile(const std::string &wav_path, const std::string &output_dir)
{
    OfflineDecodeResult result;
    WavData wav;
    std::string error;
    if (!LoadWavFile(wav_path, wav, error)) {
        result.summary = error;
        return result;
    }

    result.sample_rate = wav.sample_rate;
    result.channel_count = wav.channel_count;

    PixelBuffer scope_buffer(640, 2 * 1280);
    PixelBuffer image_buffer(800, 616);
    Decoder decoder(std::move(scope_buffer), std::move(image_buffer), "Raw Mode", wav.sample_rate);
    decoder.SetMode("AUTO");

    int reads_per_second = 50;
    int frame_count = wav.sample_rate / reads_per_second;
    int chunk_samples = frame_count * wav.channel_count;
    PixelBuffer completed(1, 1);
    int image_index = 0;
    std::string stem = BaseNameWithoutExtension(wav_path);

    for (size_t offset = 0; offset < wav.samples.size(); offset += static_cast<size_t>(chunk_samples)) {
        size_t count = std::min(static_cast<size_t>(chunk_samples), wav.samples.size() - offset);
        std::vector<float> chunk(wav.samples.begin() + static_cast<std::ptrdiff_t>(offset),
            wav.samples.begin() + static_cast<std::ptrdiff_t>(offset + count));
        decoder.Process(chunk, wav.channel_count == 1 ? 0 : 1);
        while (decoder.ConsumeCompletedImage(completed)) {
            std::ostringstream filename;
            filename << output_dir << "/" << stem << "_" << image_index++ << ".ppm";
            std::string save_error;
            if (SavePixelBufferAsPpm(completed, filename.str(), save_error)) {
                result.output_files.push_back(filename.str());
            } else {
                result.output_files.push_back(filename.str() + " (save failed: " + save_error + ")");
            }
        }
    }

    for (int flush = 0; flush < 12; ++flush) {
        std::vector<float> tail(static_cast<size_t>(chunk_samples), 0.0f);
        decoder.Process(tail, wav.channel_count == 1 ? 0 : 1);
        while (decoder.ConsumeCompletedImage(completed)) {
            std::ostringstream filename;
            filename << output_dir << "/" << stem << "_" << image_index++ << ".ppm";
            std::string save_error;
            if (SavePixelBufferAsPpm(completed, filename.str(), save_error)) {
                result.output_files.push_back(filename.str());
            } else {
                result.output_files.push_back(filename.str() + " (save failed: " + save_error + ")");
            }
        }
    }

    result.completed_images = decoder.GetCompletedImageCount();
    result.current_mode = decoder.GetCurrentModeName();
    result.success = result.completed_images > 0;

    std::ostringstream summary;
    summary << "sampleRate=" << result.sample_rate
            << ", channels=" << result.channel_count
            << ", completedImages=" << result.completed_images
            << ", currentMode=" << result.current_mode
            << ", syncPulses=" << decoder.GetSyncPulseDetectionCount()
            << ", headers=" << decoder.GetHeaderDetectionCount()
            << ", imageLine=" << decoder.GetImageLine();
    if (!result.output_files.empty()) {
        summary << ", firstOutput=" << result.output_files.front();
    }
    result.summary = summary.str();
    return result;
}

}  // namespace robot36::core
