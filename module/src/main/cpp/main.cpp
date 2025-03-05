#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include "zygisk.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std;
using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

#define LOG_TAG "SpoofX"

#ifdef NDEBUG
#define LOGD(...) ((void)0)
#define LOGV(...) ((void)0)
#else
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#endif
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__)

#define CONFIG_FILE "/data/adb/SpoofX/config.json"
#define TARGET_FILE "/data/adb/SpoofX/target"

struct CompanionData {
    vector<uint8_t> config_data;
    vector<uint8_t> target_data;
};

// 读取完整文件
static vector<uint8_t> readFile(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return {};

    int size = static_cast<int>(filesystem::file_size(path));
    vector<uint8_t> buffer(size);

    fread(buffer.data(), 1, size, file);
    fclose(file);
    return buffer;
}

// 读取 Companion 发送的数据
void readCompanionData(int fd, CompanionData &data) {
    int configSize, targetSize;

    read(fd, &configSize, sizeof(configSize));
    if (configSize > 0) {
        data.config_data.resize(configSize);
        read(fd, data.config_data.data(), configSize);
    }

    read(fd, &targetSize, sizeof(targetSize));
    if (targetSize > 0) {
        data.target_data.resize(targetSize);
        read(fd, data.target_data.data(), targetSize);
    }
}

// 字符串处理工具
vector<string> split(const string &str, const string &delimiter) {
    vector<string> tokens;
    size_t start = 0, end = str.find(delimiter);

    while (end != string::npos) {
        tokens.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
        end = str.find(delimiter, start);
    }
    tokens.push_back(str.substr(start));
    return tokens;
}

string trim(const string &str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    size_t end = str.find_last_not_of(" \t\n\r");
    return (start == string::npos || end == string::npos) ? "" : str.substr(start, end - start + 1);
}

class SpoofXModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        LOGD("preAppSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);

        if (!args || !args->nice_name) return;

        const char *process = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!process) return;
        LOGD("Process: %s", process);

        // 连接 Companion 读取配置
        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGE("Failed to connect to companion.");
            env->ReleaseStringUTFChars(args->nice_name, process);
            return;
        }

        CompanionData data;
        readCompanionData(fd, data);
        close(fd);

        // 解析目标应用
        unordered_map<string, string> targetMap;
        if (!data.target_data.empty()) {
            string targetStr(data.target_data.begin(), data.target_data.end());
            auto lines = split(targetStr, "\n");
            for (const auto &line : lines) {
                auto parts = split(trim(line), "|");
                if (parts.size() == 2) {
                    targetMap[parts[0]] = parts[1];
                }
            }
        }

        auto it = targetMap.find(process);
        if (it == targetMap.end()) {
            LOGD("Process %s is not in target list, skipping.", process);
            env->ReleaseStringUTFChars(args->nice_name, process);
            return;
        }

        string targetName = it->second;
        LOGI("Process %s is mapped to config %s", process, targetName.c_str());

        // 解析 JSON 配置
        unordered_map<string, string> spoofVars;
        if (!data.config_data.empty()) {
            json configJson = json::parse(data.config_data, nullptr, false);
            if (configJson.is_array()) {
                for (const auto &entry : configJson) {
                    if (entry.contains("name") && entry["name"].is_string() &&
                        entry["name"].get<string>() == targetName &&
                        entry.contains("configuration") && entry["configuration"].is_object()) {
                        for (auto &[key, value] : entry["configuration"].items()) {
                            if (value.is_string()) {
                                spoofVars[key] = value.get<string>();
                            }
                        }
                        break;
                    }
                }
            }
        }

        if (spoofVars.empty()) {
            LOGW("No spoofing config found for %s", process);
            env->ReleaseStringUTFChars(args->nice_name, process);
            return;
        }

        // 更新系统属性
        updateBuildFields(spoofVars);
        LOGI("Spoofing applied for %s", process);

        env->ReleaseStringUTFChars(args->nice_name, process);
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api;
    JNIEnv *env;

    void updateBuildFields(const unordered_map<string, string> &spoofVars) {
        jclass buildClass = env->FindClass("android/os/Build");
        jclass versionClass = env->FindClass("android/os/Build$VERSION");

        for (const auto &[key, value] : spoofVars) {
            jfieldID fieldID = env->GetStaticFieldID(buildClass, key.c_str(), "Ljava/lang/String;");
            if (!fieldID) {
                fieldID = env->GetStaticFieldID(versionClass, key.c_str(), "Ljava/lang/String;");
                if (!fieldID) continue;
            }
            jstring jValue = env->NewStringUTF(value.c_str());
            env->SetStaticObjectField(buildClass, fieldID, jValue);
            env->DeleteLocalRef(jValue);
            LOGI("Set '%s' to '%s'", key.c_str(), value.c_str());
        }

        env->DeleteLocalRef(buildClass);
        env->DeleteLocalRef(versionClass);
    }
};

static void companion_handler(int fd) {
    vector<uint8_t> config_data = readFile(CONFIG_FILE);
    vector<uint8_t> target_data = readFile(TARGET_FILE);

    int configSize = config_data.size();
    write(fd, &configSize, sizeof(configSize));
    if (configSize > 0) write(fd, config_data.data(), configSize);

    int targetSize = target_data.size();
    write(fd, &targetSize, sizeof(targetSize));
    if (targetSize > 0) write(fd, target_data.data(), targetSize);
}

REGISTER_ZYGISK_MODULE(SpoofXModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
