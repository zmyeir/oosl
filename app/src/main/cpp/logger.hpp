#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <android/log.h>
#include <cstdarg>

inline void log_debug_impl(const char *tag, const char *fmt, ...) {
#ifdef DEBUG
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_DEBUG, tag, fmt, args);
    va_end(args);
#endif
}

inline void log_warn_impl(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_WARN, tag, fmt, args);
    va_end(args);
}

inline void log_error_impl(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, tag, fmt, args);
    va_end(args);
}

#define DEFAULT_LOG_TAG "FDI"

#ifdef DEBUG
    #define LOGD(...) log_debug_impl(DEFAULT_LOG_TAG, __VA_ARGS__)
#else
    #define LOGD(...) ((void)0)
#endif

#define LOGW(...) log_warn_impl(DEFAULT_LOG_TAG, __VA_ARGS__)
#define LOGE(...) log_error_impl(DEFAULT_LOG_TAG, __VA_ARGS__)

#endif // LOGGER_HPP