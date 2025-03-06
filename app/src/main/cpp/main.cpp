#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <filesystem>
#include "zygisk.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std;
using namespace zygisk;

#define LOG_TAG "FDI"

#ifdef DEBUG
    #define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
    #define LOGD(...) ((void)0)
#endif
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CONFIG_FILE "/data/adb/fdi.json"
#define FALLBACK_FILE "/data/local/tmp/fdi.json"

//
// Companion 数据结构
//
struct CompanionData {
    vector<uint8_t> configBuffer;
};

//
// 安全读取文件内容（返回空 vector 表示失败）
//
static vector<uint8_t> readFileContents(const char *filePath) {
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
    if (fread(buffer.data(), 1, buffer.size(), file) != buffer.size()) {
        LOGE("无法完整读取文件: %s", filePath);
        buffer.clear();
    }

    fclose(file);
    return buffer;
}

//
// 备份配置文件到指定路径
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
// 安全写入函数：确保将 size 字节全部写入 fd
//
static bool safeWrite(int fd, const void *buffer, size_t size) {
    size_t written = 0;
    const uint8_t *buf = static_cast<const uint8_t*>(buffer);
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
// 从 companion 读取配置数据
//
static bool readCompanionData(int fd, CompanionData &data) {
    int configSize = 0;
    ssize_t bytesRead = read(fd, &configSize, sizeof(configSize));
    if (bytesRead != sizeof(configSize) || configSize <= 0) {
        LOGE("Failed to read config size from companion or invalid size: %d", configSize);
        return false;
    }

    data.configBuffer.resize(static_cast<size_t>(configSize));
    bytesRead = read(fd, data.configBuffer.data(), data.configBuffer.size());
    if (bytesRead != static_cast<ssize_t>(data.configBuffer.size())) {
        LOGE("Failed to read config data: expected %zu, got %zd", data.configBuffer.size(), bytesRead);
        data.configBuffer.clear();
        return false;
    }
    return true;
}

//
// 加载有效的配置数据：优先使用 CONFIG_FILE，其格式合法时备份，然后回退到 FALLBACK_FILE
//
static vector<uint8_t> loadValidConfigData() {
    vector<uint8_t> configBuffer = readFileContents(CONFIG_FILE);
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
// Companion 处理程序：写入配置数据到 fd
//
static void companionHandler(int fd) {
    vector<uint8_t> configBuffer = loadValidConfigData();
    int configSize = static_cast<int>(configBuffer.size());
    if (!safeWrite(fd, &configSize, sizeof(configSize))) {
        LOGE("Failed to write config size.");
        return;
    }

    if (!configBuffer.empty() && !safeWrite(fd, configBuffer.data(), configBuffer.size())) {
        LOGE("Failed to write config data.");
    }
}

//
// 模块实现：根据配置更新 Android Build 属性
//
class FakeDeviceInfo : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        LOGD("preAppSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);

        if (!args || !args->nice_name)
            return;

        const char *processName = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!processName)
            return;

        LOGD("Process: %s", processName);

        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGE("Failed to connect to companion.");
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        CompanionData data;
        if (!readCompanionData(fd, data)) {
            LOGE("Failed to read companion data.");
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        close(fd);

        json configJson = json::parse(data.configBuffer, nullptr, false);
        if (!configJson.is_array()) {
            LOGE("Invalid JSON config.");
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        map<string, string> buildProperties;
        map<string, string> buildVersionProperties;

        for (const auto &entry : configJson) {
            if (!entry.contains("targets"))
                continue;

            auto targets = entry["targets"].get<vector<string>>();
            if (find(targets.begin(), targets.end(), processName) == targets.end())
                continue;

            if (entry.contains("build")) {
                auto buildConfig = entry["build"];
                if (buildConfig.is_object()) {
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
            }
            LOGD("Process %s matched profile: %s", processName, entry["name"].get<string>().c_str());
            break;
        }

        if (!buildProperties.empty())
            updateBuildProperties(buildProperties);
        if (!buildVersionProperties.empty())
            updateBuildVersionProperties(buildVersionProperties);

        env->ReleaseStringUTFChars(args->nice_name, processName);
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;

    //
    // 通过 JNI 更新指定 Java 类的静态字段
    //
    void updateClassStaticFields(JNIEnv *env, const char *className, const map<string, string> &properties) {
        jclass targetClass = env->FindClass(className);
        if (!targetClass) {
            LOGE("Failed to find class %s", className);
            return;
        }

        for (const auto &prop : properties) {
            jfieldID fieldID = env->GetStaticFieldID(targetClass, prop.first.c_str(), "Ljava/lang/String;");
            if (!fieldID) {
                LOGD("Field '%s' not found in %s, skipping...", prop.first.c_str(), className);
                continue;
            }

            jstring jValue = env->NewStringUTF(prop.second.c_str());
            env->SetStaticObjectField(targetClass, fieldID, jValue);
            env->DeleteLocalRef(jValue);
            LOGD("Set %s.%s = '%s'", className, prop.first.c_str(), prop.second.c_str());
        }
        env->DeleteLocalRef(targetClass);
    }

    void updateBuildProperties(const map<string, string> &properties) {
        updateClassStaticFields(env, "android/os/Build", properties);
    }

    void updateBuildVersionProperties(const map<string, string> &properties) {
        updateClassStaticFields(env, "android/os/Build$VERSION", properties);
    }
};

REGISTER_ZYGISK_MODULE(FakeDeviceInfo)
REGISTER_ZYGISK_COMPANION(companionHandler)