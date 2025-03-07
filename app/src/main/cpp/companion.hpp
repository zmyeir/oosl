#ifndef COMPANION_HPP
#define COMPANION_HPP

#include "utils.hpp"

using json = nlohmann::json;

constexpr const char* CONFIG_FILE = "/data/adb/fdi/config.json";
constexpr const char* CONFIG_BACKUP_FILE = "/data/adb/fdi/do_not_edit_it";

namespace Companion {

// **全局缓存：target -> profile 映射**
inline std::unordered_map<std::string, std::shared_ptr<json>> cachedTargetProfileMap;
inline std::filesystem::file_time_type lastConfigWriteTime;

// **读取配置文件内容**
inline std::string readConfigFile(const char* filePath) {
    std::error_code ec;
    size_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec) {
        LOGE("无法获取文件大小: %s, 错误: %s", filePath, ec.message().c_str());
        return {};
    }

    FILE* file = fopen(filePath, "rb");
    if (!file) {
        LOGE("无法打开文件: %s", filePath);
        return {};
    }

    std::string buffer(fileSize, '\0');
    if (fread(buffer.data(), 1, fileSize, file) != fileSize) {
        LOGE("读取文件失败: %s", filePath);
        fclose(file);
        return {};
    }

    fclose(file);
    return buffer;
}

// **备份配置文件**
inline void backupConfigFile() {
    std::error_code ec;
    std::filesystem::copy_file(CONFIG_FILE, CONFIG_BACKUP_FILE, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        LOGE("配置文件备份失败: %s", ec.message().c_str());
    } else {
        LOGD("配置文件已备份至 %s", CONFIG_BACKUP_FILE);
    }
}

// **尝试读取并解析 JSON 配置**
inline bool loadConfigFromFile(const char* filePath, json& configJson) {
    std::string configStr = readConfigFile(filePath);
    if (configStr.empty()) {
        LOGE("配置文件为空: %s", filePath);
        return false;
    }

    json parsedJson = json::parse(configStr, nullptr, false);
    if (!parsedJson.is_array()) {
        LOGE("配置文件格式无效: %s", filePath);
        return false;
    }

    configJson = std::move(parsedJson);
    return true;
}

// **更新缓存：支持回退机制**
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

    json configJson;
    if (!loadConfigFromFile(CONFIG_FILE, configJson)) {
        LOGE("主配置文件加载失败，尝试加载备份文件...");
        if (!loadConfigFromFile(CONFIG_BACKUP_FILE, configJson)) {
            LOGE("备份配置文件也无法加载，放弃更新缓存");
            return;
        }
    }

    // 解析成功后更新缓存
    cachedTargetProfileMap.clear();
    for (const auto& profile : configJson) {
        if (!profile.contains("targets") || !profile["targets"].is_array()) {
            continue;
        }
        auto profilePtr = std::make_shared<json>(profile);
        for (const auto& target : profile["targets"]) {
            std::string targetName = target.get<std::string>();
            cachedTargetProfileMap[targetName] = profilePtr;
        }
    }

    lastConfigWriteTime = currentWriteTime;
    LOGD("配置文件更新，缓存已刷新");

    // 备份配置文件
    backupConfigFile();
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

    std::vector<char> nameBuffer(nameSize + 1);
    if (read(fd, nameBuffer.data(), nameSize) != nameSize) {
        LOGE("读取进程名失败");
        return;
    }
    nameBuffer[nameSize] = '\0';

    std::string processName(nameBuffer.data());
    LOGD("收到查询进程名: %s", processName.c_str());

    auto it = cachedTargetProfileMap.find(processName);
    json response;
    if (it != cachedTargetProfileMap.end()) {
        response = *(it->second);
    }

    std::string responseStr = response.dump();
    int responseSize = static_cast<int>(responseStr.size());

    safeWrite(fd, &responseSize, sizeof(responseSize));
    if (responseSize > 0) {
        safeWrite(fd, responseStr.data(), responseStr.size());
    }

    LOGD("Companion 发送配置完成");
}

} // namespace Companion

#endif // COMPANION_HPP