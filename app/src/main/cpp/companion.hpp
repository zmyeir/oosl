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

// **备份配置文件**
inline void backupConfigFile() {
    LOGD("开始备份配置文件...");
    std::error_code ec;
    std::filesystem::copy_file(CONFIG_FILE, CONFIG_BACKUP_FILE, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        LOGE("配置文件备份失败: %s", ec.message().c_str());
    } else {
        LOGD("配置文件已成功备份至 %s", CONFIG_BACKUP_FILE);
    }
}

// **尝试读取并解析 JSON 配置**
inline bool loadConfigFromFile(const char* filePath, json& configJson) {
    LOGD("尝试从文件加载配置: %s", filePath);
    std::error_code ec;
    size_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec) {
        LOGE("无法获取文件大小: %s, 错误: %s", filePath, ec.message().c_str());
        return false;
    }
    LOGD("文件大小: %zu 字节", fileSize);

    FILE* file = fopen(filePath, "rb");
    if (!file) {
        LOGE("无法打开文件: %s", filePath);
        return false;
    }

    std::string buffer(fileSize, '\0');
    if (fread(buffer.data(), 1, fileSize, file) != fileSize) {
        LOGE("读取文件失败: %s", filePath);
        fclose(file);
        return false;
    }
    fclose(file);
    LOGD("成功读取文件: %s", filePath);

    json parsedJson = json::parse(buffer, nullptr, false);
    if (!parsedJson.is_array()) {
        LOGE("配置文件格式无效: %s", filePath);
        return false;
    }

    configJson = std::move(parsedJson);
    LOGD("成功解析 JSON 配置文件: %s", filePath);
    return true;
}

inline void updateTargetProfileMapCache() {
    LOGD("检查配置文件是否有更新...");
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

    LOGD("检测到配置文件更新，开始重新加载...");
    json configJson;
    bool usingBackup = false;

    if (!loadConfigFromFile(CONFIG_FILE, configJson)) {
        LOGE("主配置文件加载失败，尝试加载备份文件...");
        if (!loadConfigFromFile(CONFIG_BACKUP_FILE, configJson)) {
            LOGE("备份配置文件也无法加载，放弃更新缓存");
            return;
        }
        LOGD("成功从备份文件加载配置");
        usingBackup = true;
    }

    cachedTargetProfileMap.clear();
    LOGD("清空旧缓存");

    size_t validProfileCount = 0;

    for (const auto& profile : configJson) {
        if (!profile.contains("targets") || 
            !profile["targets"].is_array() || 
            profile["targets"].empty() ||
            !profile.contains("build") || 
            profile["build"].empty()) {
            LOGW("跳过无效的配置项：targets 或 build 字段不合法");
            continue;
        }

        auto profilePtr = std::make_shared<json>(profile);

        for (const auto& target : profile["targets"]) {
            std::string targetName = target.get<std::string>();
            cachedTargetProfileMap[targetName] = profilePtr;
            LOGD("添加映射: %s -> profile", targetName.c_str());
        }

        validProfileCount++;
    }

    if (validProfileCount == 0) {
        LOGE("没有有效的配置项，保持原有缓存");
        return;
    }

    lastConfigWriteTime = currentWriteTime;
    LOGD("配置文件更新，缓存已刷新，总映射数：%zu", cachedTargetProfileMap.size());

    if (!usingBackup) {
        backupConfigFile();
    }
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
    LOGD("接收到进程名大小: %d", nameSize);

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
        LOGD("找到匹配的配置: %s", processName.c_str());
    } else {
        LOGD("未找到匹配的配置: %s", processName.c_str());
    }

    std::string responseStr = response.dump();
    int responseSize = static_cast<int>(responseStr.size());

    LOGD("准备发送响应数据，大小: %d 字节", responseSize);
    safeWrite(fd, &responseSize, sizeof(responseSize));
    if (responseSize > 0) {
        safeWrite(fd, responseStr.data(), responseStr.size());
    }

    LOGD("Companion 进程结束");
}

} // namespace Companion

#endif // COMPANION_HPP