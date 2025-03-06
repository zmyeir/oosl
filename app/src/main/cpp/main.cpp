#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include "zygisk.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std;
using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

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

struct CompanionData {
    vector<uint8_t> configBuffer;
};

// 使用 std::ifstream 读取文件内容
static vector<uint8_t> readFileContents(const char *filePath) {
    ifstream file(filePath, ios::binary | ios::ate);
    if (!file) return {};

    streamsize size = file.tellg();
    file.seekg(0, ios::beg);

    vector<uint8_t> buffer(static_cast<size_t>(size));
    if (size > 0 && file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return buffer;
    }
    return {};
}

// 备份配置文件
static bool backupConfigFile(const char *sourcePath, const char *backupPath) {
    std::error_code errorCode;
    std::filesystem::copy_file(sourcePath, backupPath, std::filesystem::copy_options::overwrite_existing, errorCode);
    if (errorCode) {
        LOGE("Failed to backup %s to %s: %s", sourcePath, backupPath, errorCode.message().c_str());
        return false;
    }
    LOGD("Successfully backed up %s to %s", sourcePath, backupPath);
    return true;
}

// 使用不抛出异常的方式解析 JSON（需确保 nlohmann::json 配置为不使用异常）
static optional<json> parseJson(const vector<uint8_t>& buffer) {
    auto j = json::parse(buffer, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        return nullopt;
    }
    return j;
}

// 读取 Companion 进程数据
static void readCompanionData(int fd, CompanionData &data) {
    int configSize = 0;
    if (read(fd, &configSize, sizeof(configSize)) != sizeof(configSize) || configSize <= 0) {
        return;
    }

    vector<uint8_t> buffer(static_cast<size_t>(configSize));
    if (read(fd, buffer.data(), configSize) != configSize) {
        return;
    }
    data.configBuffer = std::move(buffer);
}

// 加载有效的配置数据
static vector<uint8_t> loadValidConfigData() {
    vector<uint8_t> configBuffer = readFileContents(CONFIG_FILE);
    if (auto configJson = parseJson(configBuffer)) {
        backupConfigFile(CONFIG_FILE, FALLBACK_FILE);
        LOGD("Config file backed up successfully.");
        return configBuffer;
    }

    LOGE("Invalid JSON in CONFIG_FILE, attempting to load FALLBACK_FILE.");
    configBuffer = readFileContents(FALLBACK_FILE);
    if (parseJson(configBuffer)) {
        LOGW("Using fallback config.");
        return configBuffer;
    }

    LOGE("Fallback config is also invalid. Clearing configuration.");
    return {};
}

// Companion 处理程序
static void companionHandler(int fd) {
    auto configBuffer = loadValidConfigData();
    int configSize = static_cast<int>(configBuffer.size());
    if (write(fd, &configSize, sizeof(configSize)) != sizeof(configSize)) {
        LOGE("Failed to write config size.");
        return;
    }

    size_t totalWritten = 0;
    while (totalWritten < configBuffer.size()) {
        ssize_t written = write(fd, configBuffer.data() + totalWritten, configBuffer.size() - totalWritten);
        if (written <= 0) {
            LOGE("Failed to write config data, written: %zd", written);
            return;
        }
        totalWritten += written;
    }
}

class FakeDeviceInfo : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        LOGD("preAppSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        if (!args || !args->nice_name) return;

        const char *processName = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!processName) return;
        // 使用 unique_ptr 管理 GetStringUTFChars 返回的字符串，确保资源自动释放
        unique_ptr<const char, decltype(&env->ReleaseStringUTFChars)> processGuard(
            processName, [this, args](const char *p) { env->ReleaseStringUTFChars(args->nice_name, p); }
        );

        LOGD("Process: %s", processName);

        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGE("Failed to connect to companion.");
            return;
        }

        CompanionData data;
        readCompanionData(fd, data);
        close(fd);

        auto configJsonOpt = parseJson(data.configBuffer);
        if (!configJsonOpt || !configJsonOpt->is_array()) {
            LOGE("Invalid JSON config.");
            return;
        }
        json configJson = *configJsonOpt;

        map<string, string> buildProperties;
        map<string, string> buildVersionProperties;
        for (const auto &entry : configJson) {
            if (!entry.contains("targets")) continue;
            auto targets = entry["targets"].get<vector<string>>();
            if (find(targets.begin(), targets.end(), processName) == targets.end()) continue;

            if (entry.contains("build")) {
                auto buildConfig = entry["build"];
                if (buildConfig.is_object()) {
                    for (auto &[key, value] : buildConfig.items()) {
                        if (key == "version" && value.is_object()) {
                            for (auto &[vKey, vValue] : value.items()) {
                                buildVersionProperties[vKey] = vValue.get<string>();
                            }
                        } else {
                            buildProperties[key] = value.get<string>();
                        }
                    }
                }
            }
            LOGD("Process %s matched profile: %s", processName, entry["name"].get<string>().c_str());
            break;
        }

        if (!buildProperties.empty()) updateBuildProperties(buildProperties);
        if (!buildVersionProperties.empty()) updateBuildVersionProperties(buildVersionProperties);
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api;
    JNIEnv *env;

    // 更新类静态字段，利用 RAII 管理 JNI 局部引用
    void updateClassStaticFields(JNIEnv *env, const char *className, const map<string, string> &properties) {
        jclass targetClass = env->FindClass(className);
        if (!targetClass) {
            LOGE("Failed to find class %s", className);
            return;
        }
        unique_ptr<jclass, void(*)(JNIEnv*, jclass)> classGuard(targetClass, [](JNIEnv *env, jclass cls) {
            env->DeleteLocalRef(cls);
        });

        for (const auto &[key, value] : properties) {
            jfieldID fieldID = env->GetStaticFieldID(targetClass, key.c_str(), "Ljava/lang/String;");
            if (!fieldID) {
                LOGD("Field '%s' not found in %s, skipping...", key.c_str(), className);
                continue;
            }
            jstring jValue = env->NewStringUTF(value.c_str());
            env->SetStaticObjectField(targetClass, fieldID, jValue);
            env->DeleteLocalRef(jValue);
            LOGD("Set %s.%s = '%s'", className, key.c_str(), value.c_str());
        }
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
