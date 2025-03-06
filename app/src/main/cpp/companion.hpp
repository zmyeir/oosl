#ifndef COMPANION_HPP
#define COMPANION_HPP

#include <vector>
#include <string>
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include "logger.hpp"
#include "json.hpp"

using json = nlohmann::json;

#define CONFIG_FILE "/data/adb/fdi.json"
#define FALLBACK_FILE "/data/local/tmp/fdi.json"

struct CompanionData {
    std::vector<uint8_t> configBuffer;
};

//
// 安全读取文件内容
//
static std::vector<uint8_t> readFileContents(const char *filePath) {
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

    std::vector<uint8_t> buffer(static_cast<size_t>(fileStat.st_size));
    if (!buffer.empty() && fread(buffer.data(), 1, buffer.size(), file) != buffer.size()) {
        LOGE("Failed to read complete file: %s", filePath);
        buffer.clear();
    }
    fclose(file);
    return buffer;
}

//
// 备份配置文件
//
static bool backupConfigFile(const char *sourcePath, const char *backupPath) {
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
// 加载配置文件，优先使用 CONFIG_FILE
//
static std::vector<uint8_t> loadValidConfigData() {
    std::vector<uint8_t> configBuffer = readFileContents(CONFIG_FILE);
    if (!configBuffer.empty() && json::accept(configBuffer)) {
        if (backupConfigFile(CONFIG_FILE, FALLBACK_FILE)) {
            LOGD("Config file backed up successfully.");
        }
        return configBuffer;
    }

    LOGE("Invalid JSON format in CONFIG_FILE, attempting to load FALLBACK_FILE.");
    configBuffer = readFileContents(FALLBACK_FILE);
    if (!configBuffer.empty() && json::accept(configBuffer)) {
        LOGW("Using fallback config.");
        return configBuffer;
    }

    LOGE("Fallback config is also invalid. Clearing configuration.");
    return {};
}

//
// 安全写入 fd
//
static bool safeWrite(int fd, const void *buffer, size_t size) {
    size_t written = 0;
    const uint8_t *buf = static_cast<const uint8_t *>(buffer);
    while (written < size) {
        ssize_t result = write(fd, buf + written, size - written);
        if (result <= 0) {
            LOGE("Write failed with result %zd", result);
            return false;
        }
        written += result;
    }
    return true;
}

//
// Companion 进程处理函数
//
static void companionHandler(int fd) {
    std::vector<uint8_t> configBuffer = loadValidConfigData();
    int configSize = static_cast<int>(configBuffer.size());
    if (!safeWrite(fd, &configSize, sizeof(configSize))) {
        LOGE("Failed to write config size.");
        return;
    }

    if (!configBuffer.empty() && !safeWrite(fd, configBuffer.data(), configBuffer.size())) {
        LOGE("Failed to write config data.");
    }
}

#endif // COMPANION_HPP