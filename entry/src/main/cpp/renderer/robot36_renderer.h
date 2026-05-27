#ifndef ROBOT36_RENDERER_H
#define ROBOT36_RENDERER_H

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_drawing/drawing_bitmap.h>
#include <native_drawing/drawing_brush.h>
#include <native_drawing/drawing_canvas.h>
#include <native_drawing/drawing_pen.h>
#include <native_drawing/drawing_rect.h>
#include <native_window/external_window.h>
#include <js_native_api.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct OH_NativeVSync;

class Robot36Renderer {
public:
    explicit Robot36Renderer(std::string id);
    ~Robot36Renderer();

    void RegisterCallback(OH_NativeXComponent *nativeXComponent);
    void Export(napi_env env, napi_value exports);
    void SetNativeWindow(OHNativeWindow *nativeWindow);
    void SetSize(uint64_t width, uint64_t height);
    void DrawTestFrame();
    void RenderDecoderFrame();
    void SetDebugLabel(const std::string &label);
    std::string GetNativeState();

    static Robot36Renderer *FromNapiCallback(napi_env env, napi_callback_info info, size_t argc, napi_value *args);
    static Robot36Renderer *GetInstance(const std::string &id);
    static void Release(const std::string &id);

private:
    struct PanelRect {
        uint32_t left;
        uint32_t top;
        uint32_t right;
        uint32_t bottom;
    };

    static napi_value DrawTestFrameNapi(napi_env env, napi_callback_info info);
    static napi_value RenderDecoderFrameNapi(napi_env env, napi_callback_info info);
    static napi_value GetNativeStateNapi(napi_env env, napi_callback_info info);
    static napi_value SetDebugLabelNapi(napi_env env, napi_callback_info info);
    static napi_value StartDecoderNapi(napi_env env, napi_callback_info info);
    static napi_value StopDecoderNapi(napi_env env, napi_callback_info info);
    static napi_value PushAudioFrameNapi(napi_env env, napi_callback_info info);
    static napi_value SetDecoderModeNapi(napi_env env, napi_callback_info info);
    static napi_value ExportLatestImageBmpNapi(napi_env env, napi_callback_info info);

    static void OnSurfaceCreatedCB(OH_NativeXComponent *component, void *window);
    static void OnSurfaceChangedCB(OH_NativeXComponent *component, void *window);
    static void OnSurfaceDestroyedCB(OH_NativeXComponent *component, void *window);
    static void OnVSyncFrame(long long timestamp, void *data);

    void UpdateStateLocked(const std::string &state);
    void StartRenderLoopLocked();
    void StopRenderLoopLocked();
    void RequestVSyncLocked();
    void HandleVSync(long long timestamp);
    void DrawFrameLocked();
    void DrawPanel(OH_Drawing_Canvas *canvas, float left, float top, float right, float bottom, uint32_t color);
    void CopyBitmapToSurface(OH_Drawing_Bitmap *bitmap, BufferHandle *bufferHandle, void *mappedAddr);
    PanelRect GetScopePanelRect(uint32_t width_pixels, uint32_t height_pixels) const;
    PanelRect GetWaveformPanelRect(uint32_t width_pixels, uint32_t height_pixels) const;
    PanelRect GetWaterfallPanelRect(uint32_t width_pixels, uint32_t height_pixels) const;
    PanelRect GetMeterPanelRect(uint32_t width_pixels, uint32_t height_pixels) const;
    void DrawWaveformPanel(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t width_pixels, uint32_t height_pixels);
    void DrawLatestScope(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t width_pixels, uint32_t height_pixels);
    void DrawLatestWaterfall(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t width_pixels, uint32_t height_pixels);
    void DrawPeakMeter(uint32_t *bitmap_pixels, uint32_t stride_pixels, uint32_t width_pixels, uint32_t height_pixels);
    void DrawPlaceholder(OH_Drawing_Canvas *canvas, float width, float height);
    static uint32_t BlendColor(uint32_t background, uint32_t foreground, float alpha);

    OH_NativeXComponent_Callback renderCallback_ {};
    OHNativeWindow *nativeWindow_ = nullptr;
    OH_NativeVSync *nativeVsync_ = nullptr;
    std::mutex mutex_;
    std::string id_;
    std::string debugLabel_ = "Robot36 stage 0";
    std::string state_ = "Created";
    uint64_t width_ = 0;
    uint64_t height_ = 0;
    long long vsync_period_ns_ = 0;
    std::atomic<bool> vsync_enabled_ {false};
    bool vsync_callback_pending_ = false;
};

#endif
