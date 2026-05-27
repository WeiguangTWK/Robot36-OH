#ifndef ROBOT36_LOG_COMMON_H
#define ROBOT36_LOG_COMMON_H

#include <hilog/log.h>

#define LOG_DOMAIN 0x3620
constexpr const char *ROBOT36_LOG_TAG = "Robot36Native";

#define ROBOT36_LOGI(...) ((void)OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, ROBOT36_LOG_TAG, __VA_ARGS__))
#define ROBOT36_LOGW(...) ((void)OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, ROBOT36_LOG_TAG, __VA_ARGS__))
#define ROBOT36_LOGE(...) ((void)OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, ROBOT36_LOG_TAG, __VA_ARGS__))

#endif
