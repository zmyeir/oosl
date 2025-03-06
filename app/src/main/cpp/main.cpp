#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <filesystem>
#include "zygisk.hpp"
#include "json.hpp"
#include <unordered_map>

using json = nlohmann::json;
using namespace std;
using namespace zygisk;

#define LOG_TAG "FDI"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CONFIG_FILE "/data/adb/fdi.json"
#define FALLBACK_FILE "/data/local/tmp/fdi.json"

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
    size_t bytesRead = fread(buffer.data(), 1, buffer.size(), file);
    fclose(file);

    if (bytesRead != buffer.size()) {
        LOGE("读取文件失败: %s", filePath);
        return {};
    }

    return buffer;
}

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

//
// Companion 进程：解析 JSON 并建立 target -> profile 映射
//
static void companionHandler(int fd) {
    LOGD("Companion 进程启动");

    vector<uint8_t> configBuffer = readFileContents(CONFIG_FILE);
    if (configBuffer.empty()) {
        int zero = 0;
        safeWrite(fd, &zero, sizeof(zero));
        return;
    }

    json configJson = json::parse(configBuffer, nullptr, false);
    if (!configJson.is_array()) {
        int zero = 0;
        safeWrite(fd, &zero, sizeof(zero));
        return;
    }

    unordered_map<string, shared_ptr<json>> targetProfileMap;
    for (const auto &profile : configJson) {
        if (!profile.contains("targets") || !profile["targets"].is_array()) {
            continue;
        }

        auto profilePtr = make_shared<json>(profile);
        for (const auto &target : profile["targets"]) {
            string targetName = target.get<string>();
            targetProfileMap[targetName] = profilePtr;
        }
    }

    json mappedJson;
    for (const auto &[target, profilePtr] : targetProfileMap) {
        mappedJson[target] = *profilePtr;
    }

    string mappedStr = mappedJson.dump();
    int mappedSize = static_cast<int>(mappedStr.size());

    safeWrite(fd, &mappedSize, sizeof(mappedSize));
    safeWrite(fd, mappedStr.data(), mappedStr.size());

    LOGD("Companion 解析完成，发送 target-profile 映射");
}

//
// Zygisk 模块：根据 target 获取对应的 profile 并修改属性
//
class FakeDeviceInfo : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGD("FakeDeviceInfo 模块加载成功");
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        LOGD("preAppSpecialize 开始");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);

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

        int mappedSize = 0;
        if (read(fd, &mappedSize, sizeof(mappedSize)) != sizeof(mappedSize) || mappedSize <= 0) {
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        vector<uint8_t> mappedBuffer(mappedSize);
        if (read(fd, mappedBuffer.data(), mappedSize) != mappedSize) {
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        close(fd);

        json targetProfileMap = json::parse(mappedBuffer, nullptr, false);
        if (!targetProfileMap.is_object() || !targetProfileMap.contains(processName)) {
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        shared_ptr<json> profile = make_shared<json>(targetProfileMap[processName]);
        LOGD("匹配到配置项: %s", (*profile)["name"].get<string>().c_str());

        map<string, string> buildProperties;
        map<string, string> buildVersionProperties;

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
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;

    void updateClassStaticFields(JNIEnv *env, const char *className, const map<string, string> &properties) {
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

    void updateBuildProperties(const map<string, string> &properties) {
        updateClassStaticFields(env, "android/os/Build", properties);
    }

    void updateBuildVersionProperties(const map<string, string> &properties) {
        updateClassStaticFields(env, "android/os/Build$VERSION", properties);
    }
};

REGISTER_ZYGISK_MODULE(FakeDeviceInfo)
REGISTER_ZYGISK_COMPANION(companionHandler)