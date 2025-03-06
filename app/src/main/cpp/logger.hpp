#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <android/log.h>

#define LOG_TAG "FDI"

#ifdef DEBUG
    #define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
    #define LOGD(...) ((void)0)
#endif

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#endif // LOGGER_HPP