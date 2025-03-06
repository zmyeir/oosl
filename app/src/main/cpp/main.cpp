#include <android/log.h>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

#include "zygisk.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std;
using namespace std::filesystem;
using namespace zygisk;

#define LOG_TAG "FDI"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

constexpr string_view CONFIG_FILE = "/data/adb/fdi.json";
constexpr string_view FALLBACK_FILE = "/data/local/tmp/fdi.json";

struct CompanionData {
    vector<uint8_t> configBuffer;
};

// **安全读取文件内容**
static vector<uint8_t> readFileContents(string_view filePath) {
    if (!exists(filePath)) return {};

    ifstream file(string(filePath), ios::binary);
    if (!file) return {};

    vector<uint8_t> buffer((istreambuf_iterator<char>(file)), {});
    return buffer;
}

// **备份配置文件**
static bool backupConfigFile(string_view sourcePath, string_view backupPath) {
    error_code ec;
    copy_file(sourcePath, backupPath, copy_options::overwrite_existing, ec);
    
    if (ec) {
        LOGE("Failed to backup %s to %s: %s", sourcePath.data(), backupPath.data(), ec.message().c_str());
        return false;
    }
    
    LOGD("Successfully backed up %s to %s", sourcePath.data(), backupPath.data());
    return true;
}

// **读取 Companion 进程数据**
static void readCompanionData(int fd, CompanionData &data) {
    int configSize = 0;
    if (read(fd, &configSize, sizeof(configSize)) != sizeof(configSize) || configSize <= 0) {
        return;
    }

    data.configBuffer.resize(configSize);
    if (read(fd, data.configBuffer.data(), configSize) != configSize) {
        data.configBuffer.clear();
    }
}

// **加载有效的配置数据**
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

// **Companion 处理程序**
static void companionHandler(int fd) {
    vector<uint8_t> configBuffer = loadValidConfigData();

    int configSize = configBuffer.size();
    if (write(fd, &configSize, sizeof(configSize)) != sizeof(configSize)) {
        LOGE("Failed to write config size.");
        return;
    }

    size_t written = 0;
    while (written < configSize) {
        ssize_t result = write(fd, configBuffer.data() + written, configSize - written);
        if (result < 0) {
            LOGE("Failed to write config data.");
            return;
        }
        written += result;
    }
}

class FakeDeviceInfo : public ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        LOGD("preAppSpecialize");
        api->setOption(DLCLOSE_MODULE_LIBRARY);

        if (!args || !args->nice_name) return;

        const char *processName = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!processName) return;

        LOGD("Process: %s", processName);

        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGE("Failed to connect to companion.");
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        CompanionData data;
        readCompanionData(fd, data);
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

        env->ReleaseStringUTFChars(args->nice_name, processName);
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize");
        api->setOption(DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api;
    JNIEnv *env;

    void updateClassStaticFields(JNIEnv *env, const char *className, const map<string, string> &properties) {
        jclass targetClass = env->FindClass(className);
        if (!targetClass) return;

        for (const auto &[key, value] : properties) {
            jfieldID fieldID = env->GetStaticFieldID(targetClass, key.c_str(), "Ljava/lang/String;");
            if (!fieldID) continue;

            jstring jValue = env->NewStringUTF(value.c_str());
            env->SetStaticObjectField(targetClass, fieldID, jValue);
            env->DeleteLocalRef(jValue);
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