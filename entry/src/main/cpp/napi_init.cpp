#include "napi/native_api.h"
#include "common/log_common.h"
#include "plugin/plugin_manager.h"

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    ROBOT36_LOGI("Initializing Robot36 native stage 0 module");
    PluginManager::GetInstance()->Export(env, exports);
    return exports;
}
EXTERN_C_END

static napi_module robot36Module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterRobot36Module(void)
{
    napi_module_register(&robot36Module);
}
