#include "renderer/robot36_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>

#include "common/log_common.h"
#include "plugin/plugin_manager.h"
#include "runtime/decoder_runtime.h"

extern "C" {
struct OH_NativeVSync;
typedef void (*OH_NativeVSync_FrameCallback)(long long timestamp, void *data);
OH_NativeVSync *OH_NativeVSync_Create(const char *name, unsigned int length);
void OH_NativeVSync_Destroy(OH_NativeVSync *nativeVsync);
int OH_NativeVSync_RequestFrame(OH_NativeVSync *nativeVsync, OH_NativeVSync_FrameCallback callback, void *data);
int OH_NativeVSync_GetPeriod(OH_NativeVSync *nativeVsync, long long *period);
}

namespace {
std::unordered_map<std::string, Robot36Renderer *> g_renderers;
constexpr uint32_t kScopeBackground = 0xFF1A222D;
constexpr uint32_t kWaterfallBackground = 0xFF0B1521;
constexpr uint32_t kMeterBackground = 0xFF0B1117;

void FillPixelBlock(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
    uint32_t color)
{
    if (bitmap_pixels == nullptr || right <= left || bottom <= top) {
        return;
    }
    for (uint32_t y = top; y < bottom; ++y) {
        for (uint32_t x = left; x < right; ++x) {
            bitmap_pixels[y * stride_pixels + x] = color;
        }
    }
}

void WriteLe16(std::vector<uint8_t> &buffer, size_t offset, uint16_t value)
{
    buffer[offset] = static_cast<uint8_t>(value & 0xFFu);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void WriteLe32(std::vector<uint8_t> &buffer, size_t offset, uint32_t value)
{
    buffer[offset] = static_cast<uint8_t>(value & 0xFFu);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    buffer[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    buffer[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}
}

Robot36Renderer::Robot36Renderer(std::string id) : id_(std::move(id))
{
}

Robot36Renderer::~Robot36Renderer()
{
    std::lock_guard<std::mutex> lock(mutex_);
    StopRenderLoopLocked();
    nativeWindow_ = nullptr;
}

Robot36Renderer *Robot36Renderer::GetInstance(const std::string &id)
{
    auto iter = g_renderers.find(id);
    if (iter != g_renderers.end()) {
        return iter->second;
    }

    auto *renderer = PluginManager::GetInstance()->GetRenderer(id);
    g_renderers[id] = renderer;
    return renderer;
}

void Robot36Renderer::Release(const std::string &id)
{
    g_renderers.erase(id);
    PluginManager::GetInstance()->ReleaseRenderer(id);
}

void Robot36Renderer::RegisterCallback(OH_NativeXComponent *nativeXComponent)
{
    renderCallback_.OnSurfaceCreated = OnSurfaceCreatedCB;
    renderCallback_.OnSurfaceChanged = OnSurfaceChangedCB;
    renderCallback_.OnSurfaceDestroyed = OnSurfaceDestroyedCB;
    renderCallback_.DispatchTouchEvent = nullptr;
    OH_NativeXComponent_RegisterCallback(nativeXComponent, &renderCallback_);
}

void Robot36Renderer::Export(napi_env env, napi_value exports)
{
    napi_property_descriptor descriptors[] = {
        {"drawTestFrame", nullptr, DrawTestFrameNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"renderDecoderFrame", nullptr, RenderDecoderFrameNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getNativeState", nullptr, GetNativeStateNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDebugLabel", nullptr, SetDebugLabelNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startDecoder", nullptr, StartDecoderNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopDecoder", nullptr, StopDecoderNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pushAudioFrame", nullptr, PushAudioFrameNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDecoderMode", nullptr, SetDecoderModeNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"exportLatestImageBmp", nullptr, ExportLatestImageBmpNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(descriptors) / sizeof(descriptors[0]), descriptors);
}

void Robot36Renderer::SetNativeWindow(OHNativeWindow *nativeWindow)
{
    std::lock_guard<std::mutex> lock(mutex_);
    nativeWindow_ = nativeWindow;
}

void Robot36Renderer::SetSize(uint64_t width, uint64_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    width_ = width;
    height_ = height;
    UpdateStateLocked("Surface ready");
}

void Robot36Renderer::SetDebugLabel(const std::string &label)
{
    std::lock_guard<std::mutex> lock(mutex_);
    debugLabel_ = label;
    UpdateStateLocked("Label synced");
}

std::string Robot36Renderer::GetNativeState()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto snapshot = robot36::runtime::DecoderRuntime::GetInstance().GetSnapshot();
    return state_ + " | id=" + id_ + " | size=" + std::to_string(width_) + "x" + std::to_string(height_) +
        " | label=" + debugLabel_ + " | running=" + (snapshot.running ? "yes" : "no") +
        " | vsync=" + (vsync_enabled_.load() ? "on" : "off") + " | periodNs=" + std::to_string(vsync_period_ns_) +
        " | queued=" + std::to_string(snapshot.queued_samples) + " | dropped=" + std::to_string(snapshot.dropped_samples) +
        " | chunks=" + std::to_string(snapshot.processed_chunks) + " | images=" + std::to_string(snapshot.completed_images) +
        " | sync=" + std::to_string(snapshot.sync_pulses) + " | headers=" + std::to_string(snapshot.headers) +
        " | line=" + std::to_string(snapshot.image_line) + " | peak=" + std::to_string(snapshot.input_peak) +
        " | rms=" + std::to_string(snapshot.input_rms) + " | mode=" + snapshot.current_mode +
        (snapshot.last_error.empty() ? "" : " | error=" + snapshot.last_error);
}

void Robot36Renderer::UpdateStateLocked(const std::string &state)
{
    state_ = state;
}

void Robot36Renderer::StartRenderLoopLocked()
{
    if (nativeWindow_ == nullptr || width_ == 0 || height_ == 0) {
        return;
    }
    if (nativeVsync_ == nullptr) {
        nativeVsync_ = OH_NativeVSync_Create(id_.c_str(), static_cast<unsigned int>(id_.size()));
    }
    if (nativeVsync_ == nullptr) {
        UpdateStateLocked("VSync unavailable");
        return;
    }
    vsync_enabled_.store(true);
    RequestVSyncLocked();
}

void Robot36Renderer::StopRenderLoopLocked()
{
    vsync_enabled_.store(false);
    vsync_callback_pending_ = false;
    if (nativeVsync_ != nullptr) {
        OH_NativeVSync_Destroy(nativeVsync_);
        nativeVsync_ = nullptr;
    }
}

void Robot36Renderer::RequestVSyncLocked()
{
    if (!vsync_enabled_.load() || nativeVsync_ == nullptr || vsync_callback_pending_) {
        return;
    }
    if (OH_NativeVSync_RequestFrame(nativeVsync_, OnVSyncFrame, this) == 0) {
        vsync_callback_pending_ = true;
    } else {
        UpdateStateLocked("VSync request failed");
    }
}

void Robot36Renderer::HandleVSync(long long timestamp)
{
    std::lock_guard<std::mutex> lock(mutex_);
    vsync_callback_pending_ = false;
    if (!vsync_enabled_.load()) {
        return;
    }
    if (nativeVsync_ != nullptr) {
        long long period = 0;
        if (OH_NativeVSync_GetPeriod(nativeVsync_, &period) == 0) {
            vsync_period_ns_ = period;
        }
    }
    DrawFrameLocked();
    RequestVSyncLocked();
    UpdateStateLocked("VSync frame rendered @" + std::to_string(timestamp));
}

void Robot36Renderer::DrawPanel(OH_Drawing_Canvas *canvas, float left, float top, float right, float bottom, uint32_t color)
{
    OH_Drawing_Rect *rect = OH_Drawing_RectCreate(left, top, right, bottom);
    OH_Drawing_Brush *brush = OH_Drawing_BrushCreate();
    OH_Drawing_BrushSetColor(brush, color);
    OH_Drawing_CanvasAttachBrush(canvas, brush);
    OH_Drawing_CanvasDrawRect(canvas, rect);
    OH_Drawing_CanvasDetachBrush(canvas);
    OH_Drawing_BrushDestroy(brush);
    OH_Drawing_RectDestroy(rect);
}

Robot36Renderer::PanelRect Robot36Renderer::GetScopePanelRect(uint32_t width_pixels, uint32_t height_pixels) const
{
    PanelRect rect {};
    rect.left = 24;
    rect.top = 24;
    rect.right = static_cast<uint32_t>(static_cast<float>(width_pixels) * 0.72f);
    rect.bottom = height_pixels > 24 ? height_pixels - 24 : height_pixels;
    if (rect.bottom <= rect.top + 1) {
        rect.bottom = height_pixels > 24 ? height_pixels - 24 : height_pixels;
    }
    return rect;
}

Robot36Renderer::PanelRect Robot36Renderer::GetWaveformPanelRect(uint32_t width_pixels, uint32_t height_pixels) const
{
    PanelRect rect = GetScopePanelRect(width_pixels, height_pixels);
    rect.bottom = std::min(rect.bottom, rect.top + std::max<uint32_t>(80, (rect.bottom - rect.top) / 4));
    return rect;
}

Robot36Renderer::PanelRect Robot36Renderer::GetWaterfallPanelRect(uint32_t width_pixels, uint32_t height_pixels) const
{
    PanelRect rect {};
    rect.left = static_cast<uint32_t>(static_cast<float>(width_pixels) * 0.75f);
    rect.top = 24;
    rect.right = width_pixels > 72 ? width_pixels - 72 : width_pixels;
    rect.bottom = height_pixels > 24 ? height_pixels - 24 : height_pixels;
    if (rect.right <= rect.left) {
        rect.right = width_pixels > 24 ? width_pixels - 24 : width_pixels;
    }
    return rect;
}

Robot36Renderer::PanelRect Robot36Renderer::GetMeterPanelRect(uint32_t width_pixels, uint32_t height_pixels) const
{
    PanelRect rect {};
    rect.left = width_pixels > 56 ? width_pixels - 56 : width_pixels;
    rect.top = 24;
    rect.right = width_pixels > 24 ? width_pixels - 24 : width_pixels;
    rect.bottom = height_pixels > 24 ? height_pixels - 24 : height_pixels;
    return rect;
}

void Robot36Renderer::CopyBitmapToSurface(OH_Drawing_Bitmap *bitmap, BufferHandle *bufferHandle, void *mappedAddr)
{
    auto *bitmapPixels = static_cast<uint32_t *>(OH_Drawing_BitmapGetPixels(bitmap));
    auto *surfacePixels = static_cast<uint32_t *>(mappedAddr);
    if (bitmapPixels == nullptr || surfacePixels == nullptr) {
        return;
    }

    uint32_t stridePixels = static_cast<uint32_t>(bufferHandle->stride / 4);
    uint32_t widthPixels = static_cast<uint32_t>(width_);
    uint32_t heightPixels = static_cast<uint32_t>(height_);
    for (uint32_t y = 0; y < heightPixels; ++y) {
        std::memcpy(surfacePixels + (y * stridePixels), bitmapPixels + (y * stridePixels), widthPixels * sizeof(uint32_t));
    }
}

void Robot36Renderer::DrawPlaceholder(OH_Drawing_Canvas *canvas, float w, float h)
{
    PanelRect scope = GetScopePanelRect(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    PanelRect waterfall = GetWaterfallPanelRect(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    PanelRect meter = GetMeterPanelRect(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    DrawPanel(canvas, static_cast<float>(scope.left), static_cast<float>(scope.top), static_cast<float>(scope.right),
        static_cast<float>(scope.bottom), kScopeBackground);
    DrawPanel(canvas, static_cast<float>(waterfall.left), static_cast<float>(waterfall.top), static_cast<float>(waterfall.right),
        static_cast<float>(waterfall.bottom), kWaterfallBackground);
    DrawPanel(canvas, static_cast<float>(meter.left), static_cast<float>(meter.top), static_cast<float>(meter.right),
        static_cast<float>(meter.bottom), kMeterBackground);
    DrawPanel(canvas, static_cast<float>(scope.left), static_cast<float>(scope.top), static_cast<float>(scope.right),
        static_cast<float>(scope.top + 1), 0xFF31404Fu);
    DrawPanel(canvas, static_cast<float>(waterfall.left), static_cast<float>(waterfall.top), static_cast<float>(waterfall.right),
        static_cast<float>(waterfall.top + 1), 0xFF31404Fu);

    OH_Drawing_Pen *pen = OH_Drawing_PenCreate();
    OH_Drawing_PenSetColor(pen, 0x223A4B5Au);
    OH_Drawing_PenSetWidth(pen, 1.0f);
    OH_Drawing_CanvasAttachPen(canvas, pen);
    for (int i = 1; i < 4; ++i) {
        float y = static_cast<float>(scope.top) + (static_cast<float>(scope.bottom - scope.top) * static_cast<float>(i)) / 4.0f;
        OH_Drawing_CanvasDrawLine(canvas, static_cast<float>(scope.left + 8), y, static_cast<float>(scope.right - 8), y);
    }
    for (int i = 1; i < 5; ++i) {
        float x = static_cast<float>(waterfall.left) + (static_cast<float>(waterfall.right - waterfall.left) * static_cast<float>(i)) / 5.0f;
        OH_Drawing_CanvasDrawLine(canvas, x, static_cast<float>(waterfall.top + 8), x, static_cast<float>(waterfall.bottom - 8));
    }
    OH_Drawing_CanvasDetachPen(canvas);
    OH_Drawing_PenDestroy(pen);
}

void Robot36Renderer::DrawLatestScope(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t width_pixels, uint32_t height_pixels)
{
    robot36::core::PixelBuffer scope(1, 1);
    if (!robot36::runtime::DecoderRuntime::GetInstance().GetLatestScope(scope) ||
        scope.width <= 0 || scope.height <= 1 || scope.line < 0) {
        return;
    }

    PanelRect panel = GetScopePanelRect(width_pixels, height_pixels);
    if (panel.right <= panel.left || panel.bottom <= panel.top) {
        return;
    }

    uint32_t panel_width = panel.right - panel.left;
    uint32_t panel_height = panel.bottom - panel.top;
    uint32_t source_height = static_cast<uint32_t>(scope.height / 2);
    uint32_t source_start = static_cast<uint32_t>(std::max(scope.line, 0));
    for (uint32_t y = 0; y < panel_height; ++y) {
        uint32_t rel_y = std::min(static_cast<uint32_t>((static_cast<uint64_t>(y) * source_height) / panel_height),
            source_height - 1);
        uint32_t src_y = (source_start + rel_y) % source_height;
        for (uint32_t x = 0; x < panel_width; ++x) {
            uint32_t src_x = std::min(static_cast<uint32_t>((static_cast<uint64_t>(x) * scope.width) / panel_width),
                static_cast<uint32_t>(scope.width - 1));
            uint32_t color = scope.pixels[static_cast<size_t>(src_y * scope.width + src_x)];
            if ((color & 0x00FFFFFFu) != 0u || (color >> 24) != 0u) {
                bitmap_pixels[(panel.top + y) * stride_pixels + panel.left + x] = color;
            }
        }
    }
}

void Robot36Renderer::DrawWaveformPanel(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t width_pixels, uint32_t height_pixels)
{
    std::vector<float> waveform;
    if (!robot36::runtime::DecoderRuntime::GetInstance().GetLatestWaveform(waveform) || waveform.size() < 2) {
        return;
    }

    PanelRect panel = GetWaveformPanelRect(width_pixels, height_pixels);
    if (panel.right <= panel.left || panel.bottom <= panel.top) {
        return;
    }

    const uint32_t panel_width = panel.right - panel.left;
    const uint32_t panel_height = panel.bottom - panel.top;
    const uint32_t x_step = panel_width > 360 ? 3u : 2u;
    for (uint32_t x = 0; x < panel_width; x += x_step) {
        size_t sample_index = std::min((static_cast<size_t>(x) * waveform.size()) / panel_width, waveform.size() - 1);
        float magnitude = std::min(std::max(waveform[sample_index], 0.0f), 1.0f);
        uint32_t value_height = std::max<uint32_t>(1, static_cast<uint32_t>(magnitude * static_cast<float>(panel_height)));
        uint32_t start_y = panel.top + (panel_height - value_height);
        uint32_t draw_right = std::min(panel.left + x + x_step, panel.right);
        for (uint32_t y = start_y; y < panel.bottom; ++y) {
            for (uint32_t draw_x = panel.left + x; draw_x < draw_right; ++draw_x) {
                uint32_t &pixel = bitmap_pixels[y * stride_pixels + draw_x];
                pixel = BlendColor(pixel, 0xFF6CE7D5u, 0.65f);
            }
        }
    }
}

void Robot36Renderer::DrawLatestWaterfall(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t width_pixels, uint32_t height_pixels)
{
    robot36::core::PixelBuffer waterfall(1, 1);
    if (!robot36::runtime::DecoderRuntime::GetInstance().GetLatestWaterfall(waterfall) ||
        waterfall.width <= 0 || waterfall.height <= 0) {
        return;
    }

    PanelRect panel = GetWaterfallPanelRect(width_pixels, height_pixels);
    if (panel.right <= panel.left || panel.bottom <= panel.top) {
        return;
    }

    const uint32_t panel_width = panel.right - panel.left;
    const uint32_t panel_height = panel.bottom - panel.top;
    const uint32_t source_start = static_cast<uint32_t>(std::max(waterfall.line, 0));
    const uint32_t x_step = panel_width > 160 ? 3u : 2u;
    const uint32_t y_step = panel_height > 320 ? 3u : 2u;
    for (uint32_t y = 0; y < panel_height; y += y_step) {
        uint32_t rel_y = std::min(static_cast<uint32_t>((static_cast<uint64_t>(y) * waterfall.height) / panel_height),
            static_cast<uint32_t>(waterfall.height - 1));
        uint32_t src_y = (source_start + rel_y) % static_cast<uint32_t>(waterfall.height);
        uint32_t draw_bottom = std::min(panel.top + y + y_step, panel.bottom);
        for (uint32_t x = 0; x < panel_width; x += x_step) {
            uint32_t src_x = std::min(static_cast<uint32_t>((static_cast<uint64_t>(x) * waterfall.width) / panel_width),
                static_cast<uint32_t>(waterfall.width - 1));
            uint32_t color = waterfall.pixels[static_cast<size_t>(src_y * waterfall.width + src_x)];
            uint32_t draw_right = std::min(panel.left + x + x_step, panel.right);
            FillPixelBlock(bitmap_pixels, stride_pixels, panel.left + x, panel.top + y, draw_right, draw_bottom, color);
        }
    }
}

void Robot36Renderer::DrawPeakMeter(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t width_pixels, uint32_t height_pixels)
{
    PanelRect panel = GetMeterPanelRect(width_pixels, height_pixels);
    if (panel.right <= panel.left || panel.bottom <= panel.top) {
        return;
    }

    robot36::runtime::DecoderRuntime::Snapshot snapshot = robot36::runtime::DecoderRuntime::GetInstance().GetSnapshot();
    float peak = std::min(std::max(snapshot.input_peak, 0.0f), 1.0f);
    float rms = std::min(std::max(snapshot.input_rms * 4.0f, 0.0f), 1.0f);
    uint32_t panel_height = panel.bottom - panel.top;
    uint32_t peak_height = static_cast<uint32_t>(peak * static_cast<float>(panel_height));
    uint32_t rms_height = static_cast<uint32_t>(rms * static_cast<float>(panel_height));

    const uint32_t meter_y_step = panel_height > 320 ? 3u : 2u;
    for (uint32_t y = 0; y < panel_height; y += meter_y_step) {
        uint32_t abs_y = panel.top + y;
        uint32_t draw_bottom = std::min(abs_y + meter_y_step, panel.bottom);
        for (uint32_t yy = abs_y; yy < draw_bottom; ++yy) {
            for (uint32_t x = panel.left; x < panel.right; ++x) {
                uint32_t &pixel = bitmap_pixels[yy * stride_pixels + x];
                pixel = BlendColor(pixel, 0xFF151E28u, 0.85f);
            }
        }
    }

    for (uint32_t y = 0; y < rms_height; y += meter_y_step) {
        uint32_t abs_y = panel.bottom - 1 - y;
        uint32_t draw_top = abs_y + 1 > meter_y_step ? abs_y + 1 - meter_y_step : panel.top;
        for (uint32_t yy = draw_top; yy <= abs_y && yy < panel.bottom; ++yy) {
            for (uint32_t x = panel.left + 4; x + 4 < panel.right; ++x) {
                bitmap_pixels[yy * stride_pixels + x] = 0xFF2C8CFFu;
            }
        }
    }
    for (uint32_t y = 0; y < peak_height; y += meter_y_step) {
        uint32_t abs_y = panel.bottom - 1 - y;
        uint32_t draw_top = abs_y + 1 > meter_y_step ? abs_y + 1 - meter_y_step : panel.top;
        uint32_t color = (y > panel_height * 7 / 10) ? 0xFFFF7E3Du : 0xFF63E16Eu;
        for (uint32_t yy = draw_top; yy <= abs_y && yy < panel.bottom; ++yy) {
            for (uint32_t x = panel.left + 10; x + 2 < panel.right; ++x) {
                bitmap_pixels[yy * stride_pixels + x] = color;
            }
        }
    }
}

uint32_t Robot36Renderer::BlendColor(uint32_t background, uint32_t foreground, float alpha)
{
    alpha = std::min(std::max(alpha, 0.0f), 1.0f);
    auto blend_channel = [alpha](uint32_t bg, uint32_t fg) -> uint32_t {
        return static_cast<uint32_t>(static_cast<float>(bg) * (1.0f - alpha) + static_cast<float>(fg) * alpha);
    };
    uint32_t br = (background >> 16) & 0xFFu;
    uint32_t bg = (background >> 8) & 0xFFu;
    uint32_t bb = background & 0xFFu;
    uint32_t fr = (foreground >> 16) & 0xFFu;
    uint32_t fg = (foreground >> 8) & 0xFFu;
    uint32_t fb = foreground & 0xFFu;
    return 0xFF000000u | (blend_channel(br, fr) << 16) | (blend_channel(bg, fg) << 8) | blend_channel(bb, fb);
}

void Robot36Renderer::RenderDecoderFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!vsync_enabled_.load()) {
        DrawFrameLocked();
    }
}

void Robot36Renderer::DrawTestFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    DrawFrameLocked();
}

void Robot36Renderer::DrawFrameLocked()
{
    if (nativeWindow_ == nullptr || width_ == 0 || height_ == 0) {
        UpdateStateLocked("Surface not ready");
        ROBOT36_LOGW("Draw skipped because surface is not ready");
        return;
    }

    NativeWindowBuffer *buffer = nullptr;
    int fenceFd = -1;
    int requestResult = OH_NativeWindow_NativeWindowRequestBuffer(nativeWindow_, &buffer, &fenceFd);
    if (requestResult != 0 || buffer == nullptr) {
        UpdateStateLocked("Request buffer failed");
        ROBOT36_LOGE("Request buffer failed: %{public}d", requestResult);
        return;
    }

    BufferHandle *bufferHandle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    if (bufferHandle == nullptr) {
        UpdateStateLocked("Buffer handle unavailable");
        return;
    }

    void *mappedAddr = mmap(bufferHandle->virAddr, bufferHandle->size, PROT_READ | PROT_WRITE, MAP_SHARED,
        bufferHandle->fd, 0);
    if (mappedAddr == MAP_FAILED) {
        UpdateStateLocked("mmap failed");
        return;
    }

    uint32_t stridePixels = static_cast<uint32_t>(bufferHandle->stride / 4);
    OH_Drawing_Bitmap *bitmap = OH_Drawing_BitmapCreate();
    OH_Drawing_BitmapFormat bitmapFormat { COLOR_FORMAT_RGBA_8888, ALPHA_FORMAT_OPAQUE };
    OH_Drawing_BitmapBuild(bitmap, stridePixels, static_cast<uint32_t>(height_), &bitmapFormat);

    OH_Drawing_Canvas *canvas = OH_Drawing_CanvasCreate();
    OH_Drawing_CanvasBind(canvas, bitmap);
    OH_Drawing_CanvasClear(canvas, 0xFF0E1116);

    float w = static_cast<float>(width_);
    float h = static_cast<float>(height_);
    DrawPlaceholder(canvas, w, h);
    auto *bitmapPixels = static_cast<uint32_t *>(OH_Drawing_BitmapGetPixels(bitmap));
    if (bitmapPixels != nullptr) {
        DrawLatestScope(bitmapPixels, stridePixels, static_cast<uint32_t>(width_), static_cast<uint32_t>(height_));
        DrawWaveformPanel(bitmapPixels, stridePixels, static_cast<uint32_t>(width_), static_cast<uint32_t>(height_));
        DrawLatestWaterfall(bitmapPixels, stridePixels, static_cast<uint32_t>(width_), static_cast<uint32_t>(height_));
        DrawPeakMeter(bitmapPixels, stridePixels, static_cast<uint32_t>(width_), static_cast<uint32_t>(height_));
    }

    CopyBitmapToSurface(bitmap, bufferHandle, mappedAddr);
    Region region { nullptr, 0 };
    OH_NativeWindow_NativeWindowFlushBuffer(nativeWindow_, buffer, fenceFd, region);
    munmap(mappedAddr, bufferHandle->size);

    OH_Drawing_CanvasDestroy(canvas);
    OH_Drawing_BitmapDestroy(bitmap);
    UpdateStateLocked("Realtime frame rendered");
}

Robot36Renderer *Robot36Renderer::FromNapiCallback(napi_env env, napi_callback_info info, size_t argc, napi_value *args)
{
    napi_value thisArg = nullptr;
    size_t actualArgc = argc;
    if (napi_get_cb_info(env, info, &actualArgc, args, &thisArg, nullptr) != napi_ok) {
        return nullptr;
    }

    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, thisArg, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) != napi_ok) {
        return nullptr;
    }

    OH_NativeXComponent *nativeXComponent = nullptr;
    if (napi_unwrap(env, exportInstance, reinterpret_cast<void **>(&nativeXComponent)) != napi_ok) {
        return nullptr;
    }

    char idStr[OH_XCOMPONENT_ID_LEN_MAX + 1] = {'\0'};
    uint64_t idSize = OH_XCOMPONENT_ID_LEN_MAX + 1;
    if (OH_NativeXComponent_GetXComponentId(nativeXComponent, idStr, &idSize) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return nullptr;
    }

    return GetInstance(idStr);
}

napi_value Robot36Renderer::DrawTestFrameNapi(napi_env env, napi_callback_info info)
{
    napi_value args[1];
    auto *renderer = FromNapiCallback(env, info, 0, args);
    if (renderer != nullptr) {
        renderer->DrawTestFrame();
    }
    return nullptr;
}

napi_value Robot36Renderer::RenderDecoderFrameNapi(napi_env env, napi_callback_info info)
{
    napi_value args[1];
    auto *renderer = FromNapiCallback(env, info, 0, args);
    if (renderer != nullptr) {
        renderer->RenderDecoderFrame();
    }
    return nullptr;
}

napi_value Robot36Renderer::GetNativeStateNapi(napi_env env, napi_callback_info info)
{
    napi_value args[1];
    auto *renderer = FromNapiCallback(env, info, 0, args);
    napi_value result = nullptr;
    napi_create_string_utf8(env, renderer == nullptr ? "Renderer unavailable" : renderer->GetNativeState().c_str(),
        NAPI_AUTO_LENGTH, &result);
    return result;
}

napi_value Robot36Renderer::SetDebugLabelNapi(napi_env env, napi_callback_info info)
{
    napi_value args[1];
    auto *renderer = FromNapiCallback(env, info, 1, args);
    if (renderer == nullptr) {
        return nullptr;
    }

    char label[256] = {'\0'};
    napi_get_value_string_utf8(env, args[0], label, sizeof(label), nullptr);
    renderer->SetDebugLabel(label);
    return nullptr;
}

napi_value Robot36Renderer::StartDecoderNapi(napi_env env, napi_callback_info info)
{
    napi_value args[2];
    size_t argc = 2;
    auto *renderer = FromNapiCallback(env, info, argc, args);
    if (renderer == nullptr) {
        return nullptr;
    }

    int32_t sampleRate = 0;
    int32_t channelCount = 0;
    napi_get_value_int32(env, args[0], &sampleRate);
    napi_get_value_int32(env, args[1], &channelCount);
    bool ok = robot36::runtime::DecoderRuntime::GetInstance().Start(sampleRate, channelCount);
    renderer->RenderDecoderFrame();
    napi_value result = nullptr;
    napi_get_boolean(env, ok, &result);
    return result;
}

napi_value Robot36Renderer::StopDecoderNapi(napi_env env, napi_callback_info info)
{
    napi_value args[1];
    auto *renderer = FromNapiCallback(env, info, 0, args);
    robot36::runtime::DecoderRuntime::GetInstance().Stop();
    if (renderer != nullptr) {
        renderer->RenderDecoderFrame();
    }
    return nullptr;
}

napi_value Robot36Renderer::PushAudioFrameNapi(napi_env env, napi_callback_info info)
{
    napi_value args[1];
    auto *renderer = FromNapiCallback(env, info, 1, args);
    if (renderer == nullptr) {
        return nullptr;
    }

    void *data = nullptr;
    size_t byteLength = 0;
    if (napi_get_arraybuffer_info(env, args[0], &data, &byteLength) != napi_ok || data == nullptr || byteLength == 0) {
        return nullptr;
    }
    robot36::runtime::DecoderRuntime::GetInstance().PushPcm16(static_cast<int16_t *>(data), byteLength / sizeof(int16_t));
    return nullptr;
}

napi_value Robot36Renderer::SetDecoderModeNapi(napi_env env, napi_callback_info info)
{
    napi_value args[1];
    auto *renderer = FromNapiCallback(env, info, 1, args);
    if (renderer == nullptr) {
        return nullptr;
    }

    char mode[64] = {'\0'};
    napi_get_value_string_utf8(env, args[0], mode, sizeof(mode), nullptr);
    robot36::runtime::DecoderRuntime::GetInstance().SetMode(mode);
    renderer->RenderDecoderFrame();
    return nullptr;
}

napi_value Robot36Renderer::ExportLatestImageBmpNapi(napi_env env, napi_callback_info info)
{
    napi_value args[1];
    auto *renderer = FromNapiCallback(env, info, 0, args);
    napi_value nullValue = nullptr;
    napi_get_null(env, &nullValue);
    if (renderer == nullptr) {
        return nullValue;
    }

    robot36::core::PixelBuffer image(1, 1);
    if (!robot36::runtime::DecoderRuntime::GetInstance().GetLatestImage(image) || image.width <= 0 || image.height <= 0) {
        return nullValue;
    }

    const uint32_t width = static_cast<uint32_t>(image.width);
    const uint32_t height = static_cast<uint32_t>(image.height);
    const uint32_t row_stride = ((width * 3u) + 3u) & ~3u;
    const uint32_t pixel_bytes = row_stride * height;
    const uint32_t file_size = 54u + pixel_bytes;
    std::vector<uint8_t> bmp(static_cast<size_t>(file_size), 0u);

    bmp[0] = 'B';
    bmp[1] = 'M';
    WriteLe32(bmp, 2, file_size);
    WriteLe32(bmp, 10, 54u);
    WriteLe32(bmp, 14, 40u);
    WriteLe32(bmp, 18, width);
    WriteLe32(bmp, 22, height);
    WriteLe16(bmp, 26, 1u);
    WriteLe16(bmp, 28, 24u);
    WriteLe32(bmp, 34, pixel_bytes);

    for (uint32_t y = 0; y < height; ++y) {
        uint32_t src_y = height - 1u - y;
        size_t row_offset = 54u + static_cast<size_t>(y) * row_stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t argb = image.pixels[static_cast<size_t>(src_y) * width + x];
            bmp[row_offset + static_cast<size_t>(x) * 3u + 0u] = static_cast<uint8_t>(argb & 0xFFu);
            bmp[row_offset + static_cast<size_t>(x) * 3u + 1u] = static_cast<uint8_t>((argb >> 8) & 0xFFu);
            bmp[row_offset + static_cast<size_t>(x) * 3u + 2u] = static_cast<uint8_t>((argb >> 16) & 0xFFu);
        }
    }

    void *data = nullptr;
    napi_value result = nullptr;
    if (napi_create_arraybuffer(env, bmp.size(), &data, &result) != napi_ok || data == nullptr) {
        return nullValue;
    }
    std::memcpy(data, bmp.data(), bmp.size());
    return result;
}

void Robot36Renderer::OnSurfaceCreatedCB(OH_NativeXComponent *component, void *window)
{
    if (component == nullptr || window == nullptr) {
        return;
    }

    char idStr[OH_XCOMPONENT_ID_LEN_MAX + 1] = {'\0'};
    uint64_t idSize = OH_XCOMPONENT_ID_LEN_MAX + 1;
    if (OH_NativeXComponent_GetXComponentId(component, idStr, &idSize) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }

    auto *renderer = GetInstance(idStr);
    renderer->SetNativeWindow(static_cast<OHNativeWindow *>(window));

    uint64_t width = 0;
    uint64_t height = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) ==
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        renderer->SetSize(width, height);
    }
    {
        std::lock_guard<std::mutex> lock(renderer->mutex_);
        renderer->StartRenderLoopLocked();
        renderer->DrawFrameLocked();
    }
}

void Robot36Renderer::OnSurfaceChangedCB(OH_NativeXComponent *component, void *window)
{
    if (component == nullptr || window == nullptr) {
        return;
    }

    char idStr[OH_XCOMPONENT_ID_LEN_MAX + 1] = {'\0'};
    uint64_t idSize = OH_XCOMPONENT_ID_LEN_MAX + 1;
    if (OH_NativeXComponent_GetXComponentId(component, idStr, &idSize) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }

    auto *renderer = GetInstance(idStr);
    renderer->SetNativeWindow(static_cast<OHNativeWindow *>(window));

    uint64_t width = 0;
    uint64_t height = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) ==
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        renderer->SetSize(width, height);
    }
    {
        std::lock_guard<std::mutex> lock(renderer->mutex_);
        renderer->StartRenderLoopLocked();
        renderer->DrawFrameLocked();
    }
}

void Robot36Renderer::OnSurfaceDestroyedCB(OH_NativeXComponent *component, void *window)
{
    (void)window;
    if (component == nullptr) {
        return;
    }

    char idStr[OH_XCOMPONENT_ID_LEN_MAX + 1] = {'\0'};
    uint64_t idSize = OH_XCOMPONENT_ID_LEN_MAX + 1;
    if (OH_NativeXComponent_GetXComponentId(component, idStr, &idSize) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }

    if (auto *renderer = GetInstance(idStr); renderer != nullptr) {
        std::lock_guard<std::mutex> lock(renderer->mutex_);
        renderer->StopRenderLoopLocked();
        renderer->nativeWindow_ = nullptr;
        renderer->width_ = 0;
        renderer->height_ = 0;
        renderer->UpdateStateLocked("Surface destroyed");
    }
    Release(idStr);
}

void Robot36Renderer::OnVSyncFrame(long long timestamp, void *data)
{
    if (data == nullptr) {
        return;
    }
    static_cast<Robot36Renderer *>(data)->HandleVSync(timestamp);
}
