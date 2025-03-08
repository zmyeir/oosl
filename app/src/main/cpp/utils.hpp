#ifndef UTILS_HPP
#define UTILS_HPP

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <string>

#define JSON_NOEXCEPTION 1
#define JSON_NO_IO 1
#include "json.hpp"

#define LOG_TAG "FDI"
#ifdef DEBUG
    #define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
    #define LOGD(...)
#endif
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

inline bool safeWrite(int fd, const void *buffer, size_t size) {
    size_t written = 0;
    const uint8_t *buf = static_cast<const uint8_t*>(buffer);
    while (written < size) {
        ssize_t result = write(fd, buf + written, size - written);
        if (result <= 0) {
            return false;
        }
        written += result;
    }
    return true;
}

#endif // UTILS_HPP