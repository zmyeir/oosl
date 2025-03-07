#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <unordered_map>
#include "zygisk.hpp"

#define JSON_NOEXCEPTION 1
#define JSON_NO_IO 1

#include "json.hpp"
#include "companion.hpp"

using json = nlohmann::json;
using namespace std;
using namespace zygisk;

#define LOG_TAG "FDI"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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

        // 主进程向伴生进程发送进程名
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
            LOGD("未匹配到配置项");
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
REGISTER_ZYGISK_COMPANION(Companion::FakeDeviceInfoD)