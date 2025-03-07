#ifndef COMPANION_HPP
#define COMPANION_HPP

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <filesystem>
#include <unordered_map>

#define JSON_NOEXCEPTION 1
#define JSON_NO_IO 1
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

#define COMPANION_TAG "FDI_COMPANION"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, COMPANION_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, COMPANION_TAG, __VA_ARGS__)

#define CONFIG_FILE "/data/adb/fdi/config.json"

namespace Companion {

// **全局缓存：target -> profile 映射**
inline unordered_map<string, shared_ptr<json>> cachedTargetProfileMap;
inline std::filesystem::file_time_type lastConfigWriteTime;

// **读取配置文件内容**
inline vector<uint8_t> readConfigFile(const char *filePath) {
    std::error_code ec;
    size_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec) {
        LOGE("无法获取文件大小: %s, 错误: %s", filePath, ec.message().c_str());
        return {};
    }

    FILE *file = fopen(filePath, "rb");
    if (!file) {
        LOGE("无法打开文件: %s", filePath);
        return {};
    }

    vector<uint8_t> buffer(fileSize);
    size_t bytesRead = fread(buffer.data(), 1, buffer.size(), file);
    fclose(file);

    if (bytesRead != buffer.size()) {
        LOGE("读取文件失败: %s", filePath);
        return {};
    }

    return buffer;
}

// **更新缓存：如果配置文件发生变化则重新加载**
inline void updateTargetProfileMapCache() {
    std::error_code ec;
    auto currentWriteTime = std::filesystem::last_write_time(CONFIG_FILE, ec);
    if (ec) {
        LOGE("无法获取配置文件修改时间: %s", ec.message().c_str());
        return;
    }

    if (currentWriteTime == lastConfigWriteTime) {
        LOGD("配置文件未变更，使用缓存数据");
        return;
    }

    vector<uint8_t> configBuffer = readConfigFile(CONFIG_FILE);
    if (configBuffer.empty()) {
        LOGE("配置文件为空，无法更新缓存");
        return;
    }

    json configJson = json::parse(configBuffer, nullptr, false);
    if (!configJson.is_array()) {
        LOGE("配置文件格式无效");
        return;
    }

    cachedTargetProfileMap.clear();
    LOGD("配置文件合法，清除缓存");
    for (const auto &profile : configJson) {
        if (!profile.contains("targets") || !profile["targets"].is_array()) {
            continue;
        }
        auto profilePtr = make_shared<json>(profile);
        for (const auto &target : profile["targets"]) {
            string targetName = target.get<string>();
            cachedTargetProfileMap[targetName] = profilePtr;
        }
    }

    lastConfigWriteTime = currentWriteTime;
    LOGD("配置文件更新，缓存已刷新");
}

// **安全写数据到文件描述符**
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

// **伴生进程逻辑**
inline void FakeDeviceInfoD(int fd) {
    LOGD("Companion 进程启动");

    updateTargetProfileMapCache();

    int nameSize = 0;
    if (read(fd, &nameSize, sizeof(nameSize)) != sizeof(nameSize) || nameSize <= 0) {
        LOGE("读取进程名大小失败");
        return;
    }

    vector<char> nameBuffer(nameSize + 1);
    if (read(fd, nameBuffer.data(), nameSize) != nameSize) {
        LOGE("读取进程名失败");
        return;
    }
    nameBuffer[nameSize] = '\0';

    string processName(nameBuffer.data());
    LOGD("收到查询进程名: %s", processName.c_str());

    auto it = cachedTargetProfileMap.find(processName);
    json response;
    if (it != cachedTargetProfileMap.end()) {
        response = *(it->second);
    }

    string responseStr = response.dump();
    int responseSize = static_cast<int>(responseStr.size());

    safeWrite(fd, &responseSize, sizeof(responseSize));
    if (responseSize > 0) {
        safeWrite(fd, responseStr.data(), responseStr.size());
    }

    LOGD("Companion 发送配置完成");
}

} // namespace Companion

#endif // COMPANION_HPP