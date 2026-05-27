#ifndef ROBOT36_PLUGIN_MANAGER_H
#define ROBOT36_PLUGIN_MANAGER_H

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <js_native_api.h>
#include <string>
#include <unordered_map>
#include "renderer/robot36_renderer.h"

class PluginManager {
public:
    static PluginManager *GetInstance();
    ~PluginManager();

    void Export(napi_env env, napi_value exports);
    void SetNativeXComponent(const std::string &id, OH_NativeXComponent *nativeXComponent);
    Robot36Renderer *GetRenderer(const std::string &id);
    void ReleaseRenderer(const std::string &id);

private:
    std::unordered_map<std::string, OH_NativeXComponent *> nativeXComponentMap_;
    std::unordered_map<std::string, Robot36Renderer *> rendererMap_;
};

#endif
