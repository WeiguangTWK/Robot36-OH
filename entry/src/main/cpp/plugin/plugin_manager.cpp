#include "plugin/plugin_manager.h"
#include "common/log_common.h"

PluginManager *PluginManager::GetInstance()
{
    static PluginManager instance;
    return &instance;
}

PluginManager::~PluginManager()
{
    for (auto &[id, renderer] : rendererMap_) {
        delete renderer;
    }
    rendererMap_.clear();
    nativeXComponentMap_.clear();
}

void PluginManager::Export(napi_env env, napi_value exports)
{
    if (env == nullptr || exports == nullptr) {
        ROBOT36_LOGE("Export skipped because env or exports is null");
        return;
    }

    napi_value exportInstance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) != napi_ok) {
        ROBOT36_LOGE("Failed to get native xcomponent object");
        return;
    }

    OH_NativeXComponent *nativeXComponent = nullptr;
    if (napi_unwrap(env, exportInstance, reinterpret_cast<void **>(&nativeXComponent)) != napi_ok) {
        ROBOT36_LOGE("Failed to unwrap native xcomponent");
        return;
    }

    char idStr[OH_XCOMPONENT_ID_LEN_MAX + 1] = {'\0'};
    uint64_t idSize = OH_XCOMPONENT_ID_LEN_MAX + 1;
    if (OH_NativeXComponent_GetXComponentId(nativeXComponent, idStr, &idSize) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        ROBOT36_LOGE("Failed to get xcomponent id");
        return;
    }

    std::string id(idStr);
    SetNativeXComponent(id, nativeXComponent);
    auto *renderer = GetRenderer(id);
    renderer->RegisterCallback(nativeXComponent);
    renderer->Export(env, exports);
}

void PluginManager::SetNativeXComponent(const std::string &id, OH_NativeXComponent *nativeXComponent)
{
    if (nativeXComponent == nullptr) {
        ROBOT36_LOGE("Cannot store null xcomponent for %{public}s", id.c_str());
        return;
    }
    nativeXComponentMap_[id] = nativeXComponent;
}

Robot36Renderer *PluginManager::GetRenderer(const std::string &id)
{
    auto iter = rendererMap_.find(id);
    if (iter != rendererMap_.end()) {
        return iter->second;
    }

    auto *renderer = new Robot36Renderer(id);
    rendererMap_[id] = renderer;
    return renderer;
}

void PluginManager::ReleaseRenderer(const std::string &id)
{
    auto iter = rendererMap_.find(id);
    if (iter == rendererMap_.end()) {
        return;
    }
    delete iter->second;
    rendererMap_.erase(iter);
    nativeXComponentMap_.erase(id);
}
