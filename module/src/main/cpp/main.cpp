#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <unistd.h>
#include <fcntl.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

#define LOG_TAG "OOSLocalization"

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

#define CONFIG_FILE "/data/adb/OOSLocalization/spoof_vars"
#define APPLIST_FILE "/data/adb/OOSLocalization/applist"
#define DEFAULT_CONFIG "MODEL=PJD110"

ssize_t xread(int fd, void *buffer, size_t count) {
    LOGD("xread, fd: %d, count: %zu", fd, count);
    ssize_t total = 0;
    char *buf = (char *)buffer;
    while (count > 0) {
        ssize_t ret = read(fd, buf, count);
        if (ret < 0) return -1;
        buf += ret;
        total += ret;
        count -= ret;
    }
    return total;
}

ssize_t xwrite(int fd, const void *buffer, size_t count) {
    LOGD("xwrite, fd: %d, count: %zu", fd, count);
    ssize_t total = 0;
    char *buf = (char *)buffer;
    while (count > 0) {
        ssize_t ret = write(fd, buf, count);
        if (ret < 0) return -1;
        buf += ret;
        total += ret;
        count -= ret;
    }
    return total;
}

struct CompanionData {
    std::vector<uint8_t> config_data;
    std::vector<uint8_t> applist_data;
};

std::vector<std::string> split(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0;
    size_t end = str.find(delimiter);

    while (end != std::string::npos) {
        tokens.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
        end = str.find(delimiter, start);
    }

    tokens.push_back(str.substr(start));
    return tokens;
}

std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    size_t end = str.find_last_not_of(" \t\n\r");
    if (start == std::string::npos || end == std::string::npos || start > end) {
        return "";
    }
    return str.substr(start, end - start + 1);
}

class MyModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        LOGD("preAppSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);

        if (!args || !args->app_data_dir) {
            return;
        }

        auto app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        auto nice_name = env->GetStringUTFChars(args->nice_name, nullptr);

        std::string_view dir(app_data_dir);
        std::string_view process(nice_name);

        LOGD("process: %s", process.data());

        int fd = api->connectCompanion();
        LOGD("connectCompanion: %d", fd);
        
        // 读取配置和应用列表
        CompanionData data;
        readCompanionData(fd, data);
        
        // 解析应用列表
        std::unordered_set<std::string> targetApps;
        if (!data.applist_data.empty()) {
            std::string applistStr(data.applist_data.begin(), data.applist_data.end());
            auto lines = split(applistStr, "\n");
            for (auto &line : lines) {
                auto trimmed = trim(line);
                if (!trimmed.empty()) {
                    targetApps.insert(trimmed);
                }
            }
        } else {
            // 默认应用列表
            targetApps = {"com.finshell.wallet", "com.unionpay.tsmservice"};
        }

        // 检查当前进程是否在目标应用列表中
        if (!std::any_of(targetApps.begin(), targetApps.end(), [&process](const std::string &app) {
            return process.starts_with(app);
        })) {
            close(fd);
            env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
            env->ReleaseStringUTFChars(args->nice_name, nice_name);
            return;
        }

        // 解析配置文件
        if (!data.config_data.empty()) {
            std::string configStr(data.config_data.begin(), data.config_data.end());
            parseConfig(configStr);
        }

        close(fd);
        LOGD("Close companion, fd: %d", fd);

        LOGI("Spoofing build vars for %s", process.data());
        UpdateBuildFields();
        LOGI("Spoofed build vars for %s", process.data());

        spoofVars.clear();
        env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
        env->ReleaseStringUTFChars(args->nice_name, nice_name);
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api;
    JNIEnv *env;
    std::unordered_map<std::string, std::string> spoofVars;

    void readCompanionData(int fd, CompanionData &data) {
        int configSize, applistSize;
        
        // 读取配置文件大小和内容
        xread(fd, &configSize, sizeof(configSize));
        if (configSize > 0) {
            data.config_data.resize(configSize);
            xread(fd, data.config_data.data(), configSize);
        }
        
        // 读取应用列表大小和内容
        xread(fd, &applistSize, sizeof(applistSize));
        if (applistSize > 0) {
            data.applist_data.resize(applistSize);
            xread(fd, data.applist_data.data(), applistSize);
        }
    }

    void parseConfig(const std::string &configStr) {
        LOGD("Parsing config");
        auto lines = split(configStr, "\n");
        LOGD("Parsed %zu lines", lines.size());
        for (auto &line: lines) {
            if (trim(line).empty()) {
                continue;
            }
            auto parts = split(line, "=");
            if (parts.size() != 2) {
                continue;
            }
            auto key = trim(parts[0]);
            auto value = trim(parts[1]);
            spoofVars[key] = value;
            LOGD("Parsed: %s=%s", key.c_str(), value.c_str());
        }
    }

    void UpdateBuildFields() {
        LOGD("UpdateBuildFields");
        jclass buildClass = env->FindClass("android/os/Build");
        LOGD("buildClass: %p", buildClass);
        jclass versionClass = env->FindClass("android/os/Build$VERSION");
        LOGD("versionClass: %p", versionClass);

        for (auto &[key, val]: spoofVars) {
            const char *fieldName = key.c_str();

            jfieldID fieldID = env->GetStaticFieldID(buildClass, fieldName, "Ljava/lang/String;");

            if (env->ExceptionCheck()) {
                env->ExceptionClear();

                fieldID = env->GetStaticFieldID(versionClass, fieldName, "Ljava/lang/String;");

                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    continue;
                }
            }

            if (fieldID != nullptr) {
                const char *value = val.c_str();
                jstring jValue = env->NewStringUTF(value);

                env->SetStaticObjectField(buildClass, fieldID, jValue);

                env->DeleteLocalRef(jValue);

                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    continue;
                }

                LOGI("Set '%s' to '%s'", fieldName, value);
            }
        }

        env->DeleteLocalRef(buildClass);
        env->DeleteLocalRef(versionClass);
    }
};

static std::vector<uint8_t> readFile(const char *path) {
    FILE *file = fopen(path, "rb");

    if (!file) return {};

    int size = static_cast<int>(std::filesystem::file_size(path));

    std::vector<uint8_t> vector(size);

    fread(vector.data(), 1, size, file);

    fclose(file);

    return vector;
}

static void companion_handler(int fd) {
    LOGD("companion_handler, fd: %d", fd);
    
    // 读取配置文件
    std::vector<uint8_t> config_data = readFile(CONFIG_FILE);
    if (config_data.empty()) {
        config_data.resize(strlen(DEFAULT_CONFIG));
        memcpy(config_data.data(), DEFAULT_CONFIG, strlen(DEFAULT_CONFIG));
    }
    
    // 读取应用列表文件
    std::vector<uint8_t> applist_data = readFile(APPLIST_FILE);
    
    // 发送配置文件大小和内容
    int configSize = static_cast<int>(config_data.size());
    xwrite(fd, &configSize, sizeof(configSize));
    if (configSize > 0) {
        xwrite(fd, config_data.data(), configSize);
    }
    
    // 发送应用列表大小和内容
    int applistSize = static_cast<int>(applist_data.size());
    xwrite(fd, &applistSize, sizeof(applistSize));
    if (applistSize > 0) {
        xwrite(fd, applist_data.data(), applistSize);
    }
    
    LOGD("companion_handler done, fd: %d", fd);
}

REGISTER_ZYGISK_MODULE(MyModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
