#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <filesystem>
#include <unordered_map>
#include "zygisk.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std;
using namespace zygisk;

#define LOG_TAG "FDI"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CONFIG_FILE "/data/adb/fdi/config.json"

// 读取配置文件内容
static vector<uint8_t> readConfigFile(const char *filePath) {
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
    LOGD("文件读取成功: %s", filePath);
}

// 安全写数据到文件描述符
static bool safeWrite(int fd, const void *buffer, size_t size) {
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

// 全局缓存：target->profile 映射
static unordered_map<string, shared_ptr<json>> cachedTargetProfileMap;
static std::filesystem::file_time_type lastConfigWriteTime;

// 更新缓存：如果配置文件发生变化则重新加载
static void updateTargetProfileMapCache() {
    std::error_code ec;
    auto currentWriteTime = std::filesystem::last_write_time(CONFIG_FILE, ec);
    if (ec) {
        LOGE("无法获取配置文件修改时间: %s", ec.message().c_str());
        return;
    }

    // 如果文件未变更，则直接使用缓存数据
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

// 伴生进程逻辑：接收主进程传入的进程名，并返回对应的 profile（如果存在）
static void companionHandler(int fd) {
    LOGD("Companion 进程启动");

    // 更新缓存数据
    updateTargetProfileMapCache();

    // 读取主进程发送的进程名长度
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

    // 查找对应配置项
    auto it = cachedTargetProfileMap.find(processName);
    json response;
    if (it != cachedTargetProfileMap.end()) {
        response = *(it->second);
    }

    string responseStr = response.dump();
    int responseSize = static_cast<int>(responseStr.size());

    // 发送 JSON 数据长度和内容
    safeWrite(fd, &responseSize, sizeof(responseSize));
    if (responseSize > 0) {
        safeWrite(fd, responseStr.data(), responseStr.size());
    }

    LOGD("Companion 发送配置完成");
}

// Zygisk 模块：根据 target 获取对应的 profile 并修改 Build 属性
class FakeDeviceInfo : public ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGD("FakeDeviceInfo 模块加载成功");
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        LOGD("启动 preAppSpecialize");
        api->setOption(DLCLOSE_MODULE_LIBRARY);

        if (!args || !args->nice_name) {
            return;
        }

        const char *processName = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!processName) {
            return;
        }
        LOGD("当前进程名称: %s", processName);

        int fd = api->connectCompanion();
        if (fd < 0) {
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        // 主进程向伴生进程发送自身进程名
        int nameSize = static_cast<int>(strlen(processName));
        safeWrite(fd, &nameSize, sizeof(nameSize));
        safeWrite(fd, processName, nameSize);

        // 读取伴生进程返回的 JSON 数据长度
        int responseSize = 0;
        if (read(fd, &responseSize, sizeof(responseSize)) != sizeof(responseSize) || responseSize <= 0) {
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        vector<uint8_t> responseBuffer(responseSize);
        if (read(fd, responseBuffer.data(), responseSize) != responseSize) {
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        close(fd);

        json profileJson = json::parse(responseBuffer, nullptr, false);
        if (!profileJson.is_object()) {
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        auto profile = make_shared<json>(profileJson);
        LOGD("匹配到配置项: %s", (*profile)["name"].get<string>().c_str());

        // 使用 unordered_map 存储 Build 相关属性以提高查找效率
        unordered_map<string, string> buildProperties;
        unordered_map<string, string> buildVersionProperties;

        if (profile->contains("build") && (*profile)["build"].is_object()) {
            auto &buildConfig = (*profile)["build"];
            for (auto it = buildConfig.begin(); it != buildConfig.end(); ++it) {
                if (it.key() == "version" && it.value().is_object()) {
                    for (auto &vEntry : it.value().items()) {
                        buildVersionProperties[vEntry.key()] = vEntry.value().get<string>();
                    }
                } else {
                    buildProperties[it.key()] = it.value().get<string>();
                }
            }
        }

        if (!buildProperties.empty())
            updateBuildProperties(buildProperties);
        if (!buildVersionProperties.empty())
            updateBuildVersionProperties(buildVersionProperties);

        env->ReleaseStringUTFChars(args->nice_name, processName);
        LOGD("preAppSpecialize 处理完成");
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        api->setOption(DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;

    // 更新指定类的静态字段
    void updateClassStaticFields(JNIEnv *env, const char *className, const unordered_map<string, string> &properties) {
        jclass targetClass = env->FindClass(className);
        if (!targetClass) {
            return;
        }
        for (const auto &prop : properties) {
            jfieldID fieldID = env->GetStaticFieldID(targetClass, prop.first.c_str(), "Ljava/lang/String;");
            if (!fieldID) {
                continue;
            }
            jstring jValue = env->NewStringUTF(prop.second.c_str());
            env->SetStaticObjectField(targetClass, fieldID, jValue);
            env->DeleteLocalRef(jValue);
        }
        env->DeleteLocalRef(targetClass);
    }

    void updateBuildProperties(const unordered_map<string, string> &properties) {
        updateClassStaticFields(env, "android/os/Build", properties);
    }

    void updateBuildVersionProperties(const unordered_map<string, string> &properties) {
        updateClassStaticFields(env, "android/os/Build$VERSION", properties);
    }
};

REGISTER_ZYGISK_MODULE(FakeDeviceInfo)
REGISTER_ZYGISK_COMPANION(companionHandler)