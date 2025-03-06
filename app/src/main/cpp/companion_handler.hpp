#ifndef COMPANION_HANDLER_HPP
#define COMPANION_HANDLER_HPP

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/log.h>
#include <filesystem>
#include <vector>
#include "json.hpp"
#include "logger.hpp"

using json = nlohmann::json;
using namespace std;

namespace companion {

// 定义配置文件路径
#ifndef CONFIG_FILE
#define CONFIG_FILE "/data/adb/fdi.json"
#endif

#ifndef FALLBACK_FILE
#define FALLBACK_FILE "/data/local/tmp/fdi.json"
#endif

//
// CompanionData 结构体，用于保存 companion 模式下的配置数据
//
struct CompanionData {
    vector<uint8_t> configBuffer;
};

//
// 内联函数：安全读取指定文件内容到 vector，失败时返回空 vector
//
static inline vector<uint8_t> readFileContents(const char *filePath) {
    LOGD("Attempting to open file: %s", filePath);
    FILE *file = fopen(filePath, "rb");
    if (!file) {
        LOGE("Failed to open file: %s", filePath);
        return {};
    }

    struct stat fileStat;
    if (stat(filePath, &fileStat) != 0) {
        LOGE("Failed to stat file: %s", filePath);
        fclose(file);
        return {};
    }
    LOGD("File %s size: %ld bytes", filePath, fileStat.st_size);

    vector<uint8_t> buffer(static_cast<size_t>(fileStat.st_size));
    if (!buffer.empty()) {
        size_t bytesRead = fread(buffer.data(), 1, buffer.size(), file);
        LOGD("Read %zu bytes from %s", bytesRead, filePath);
        if (bytesRead != buffer.size()) {
            LOGE("Incomplete read: expected %zu, got %zu", buffer.size(), bytesRead);
            buffer.clear();
        }
    }
    fclose(file);
    return buffer;
}

//
// 内联函数：备份配置文件到备用路径
//
static inline bool backupConfigFile(const char *sourcePath, const char *backupPath) {
    LOGD("Attempting to backup file from %s to %s", sourcePath, backupPath);
    std::error_code errorCode;
    std::filesystem::copy_file(sourcePath, backupPath,
                               std::filesystem::copy_options::overwrite_existing,
                               errorCode);
    if (errorCode) {
        LOGE("Failed to backup %s to %s: %s", sourcePath, backupPath, errorCode.message().c_str());
        return false;
    }
    LOGD("Successfully backed up %s to %s", sourcePath, backupPath);
    return true;
}

//
// 内联函数：安全写入指定缓冲区中的 size 字节到 fd
//
static inline bool safeWrite(int fd, const void *buffer, size_t size) {
    LOGD("Starting safeWrite of %zu bytes", size);
    size_t written = 0;
    const uint8_t *buf = static_cast<const uint8_t*>(buffer);
    while (written < size) {
        ssize_t result = write(fd, buf + written, size - written);
        if (result <= 0) {
            LOGE("Write failed with result %zd", result);
            return false;
        }
        written += result;
        LOGD("Wrote %zd bytes, total written: %zu", result, written);
    }
    LOGD("Completed safeWrite, total bytes written: %zu", written);
    return true;
}

//
// 内联函数：从 companion 读取配置数据，成功返回 true，否则返回 false
//
static inline bool readCompanionData(int fd, CompanionData &data) {
    int configSize = 0;
    ssize_t bytesRead = read(fd, &configSize, sizeof(configSize));
    if (bytesRead != sizeof(configSize) || configSize <= 0) {
        LOGE("Failed to read config size from companion or invalid size: %d", configSize);
        return false;
    }
    LOGD("Config size read from companion: %d", configSize);

    data.configBuffer.resize(static_cast<size_t>(configSize));
    bytesRead = read(fd, data.configBuffer.data(), data.configBuffer.size());
    if (bytesRead != static_cast<ssize_t>(data.configBuffer.size())) {
        LOGE("Failed to read config data: expected %zu, got %zd", data.configBuffer.size(), bytesRead);
        data.configBuffer.clear();
        return false;
    }
    LOGD("Successfully read config data of %zu bytes", data.configBuffer.size());
    return true;
}

//
// 内联函数：加载有效的配置数据
// 优先使用 CONFIG_FILE，若 JSON 格式合法则备份；否则尝试使用 FALLBACK_FILE
//
static inline vector<uint8_t> loadValidConfigData() {
    LOGD("Loading configuration from %s", CONFIG_FILE);
    vector<uint8_t> configBuffer = readFileContents(CONFIG_FILE);
    if (!configBuffer.empty() && json::accept(configBuffer)) {
        LOGD("Valid JSON found in %s", CONFIG_FILE);
        if (backupConfigFile(CONFIG_FILE, FALLBACK_FILE)) {
            LOGD("Config file backed up successfully.");
        }
        return configBuffer;
    }
    LOGE("Invalid JSON format in CONFIG_FILE, attempting to load FALLBACK_FILE.");
    configBuffer = readFileContents(FALLBACK_FILE);
    if (!configBuffer.empty() && json::accept(configBuffer)) {
        LOGW("Using fallback config from %s", FALLBACK_FILE);
        return configBuffer;
    }
    LOGE("Fallback config is also invalid. Clearing configuration.");
    return {};
}

//
// 内联函数：companionHandler，由 zygisk 框架在 companion 模式下调用
//
static inline void companionHandler(int fd) {
    LOGD("Companion handler started.");
    vector<uint8_t> configBuffer = loadValidConfigData();
    int configSize = static_cast<int>(configBuffer.size());
    if (!safeWrite(fd, &configSize, sizeof(configSize))) {
        LOGE("Failed to write config size to companion.");
        return;
    }
    if (!configBuffer.empty() && !safeWrite(fd, configBuffer.data(), configBuffer.size())) {
        LOGE("Failed to write config data to companion.");
    }
    LOGD("Companion handler finished writing config data.");
}

} // namespace companion

#endif // COMPANION_HANDLER_HPP