#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>
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

#define CONFIG_FILE "/data/adb/OOSLocalization/model"
#define DEFAULT_CONFIG "MODEL=PJD110"

#define TARGET_FILE "/data/adb/OOSLocalization/target"
std::vector<std::string> DEFAULT_TARGET = {"com.finshell.wallet", "com.unionpay.tsmservice"};

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

std::string join(const std::vector<std::string>& vec, const std::string& delimiter) {
    std::string result;
    for (const auto& item : vec) {
        if (!result.empty()) {
            result += delimiter;
        }
        result += item;
    }
    return result;
}

std::vector<uint8_t> readFile(const char *path) {
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
    std::vector<uint8_t> config_data;

    config_data = readFile(CONFIG_FILE);
    LOGD("config_data size: %zu", config_data.size());
    if (config_data.empty()) {
        LOGD("Using default config file");
        config_data.resize(strlen(DEFAULT_CONFIG));
        memcpy(config_data.data(), DEFAULT_CONFIG, strlen(DEFAULT_CONFIG));
    }

    int configSize = static_cast<int>(config_data.size());

    xwrite(fd, &configSize, sizeof(configSize));

    if (configSize > 0) {
        xwrite(fd, config_data.data(), configSize * sizeof(uint8_t));
    }
    LOGD("companion_handler done, fd: %d", fd);
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

        std::vector<uint8_t> targetData = readFile(TARGET_FILE);
        std::vector<std::string> targetApps;

        if (!targetData.empty()) {
            std::string targetList(targetData.begin(), targetData.end());
            targetApps = split(targetList, "\n"); // 按行分割
        } else {
            LOGD("Using default target apps");
            targetApps = DEFAULT_TARGET;
        }

        if (std::find(targetApps.begin(), targetApps.end(), process) == targetApps.end()) {
            env->ReleaseStringUTFChars(args->app_data_dir, app_data_dir);
            env->ReleaseStringUTFChars(args->nice_name, nice_name);
            return;
        }

        int fd = api->connectCompanion();
        LOGD("connectCompanion: %d", fd);
        int configSize;
        std::string configStr;
        xread(fd, &configSize, sizeof(configSize));
        if (configSize > 0) {
            configStr.resize(configSize);
            xread(fd, configStr.data(), configSize * sizeof(uint8_t));
            LOGD("Config: %s", configStr.c_str());
        }
        close(fd);
        LOGD("Close companion, fd: %d", fd);

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

    void UpdateBuildFields() {
        LOGD("UpdateBuildFields");
        jclass buildClass = env->FindClass("android/os/Build");
        jclass versionClass = env->FindClass("android/os/Build$VERSION");

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

// Register our module class and the companion handler function
REGISTER_ZYGISK_MODULE(MyModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
