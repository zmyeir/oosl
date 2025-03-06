#include "zygisk.hpp"
#include <jni.h>
#include <map>
#include <string>
#include "logger.hpp"
#include "companion.hpp"
#include "json.hpp"

using namespace zygisk;
using json = nlohmann::json;

class FakeDeviceInfo : public ModuleBase {
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

        // 连接 Companion 获取配置
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

        // 解析 JSON 配置
        json configJson = json::parse(data.configBuffer, nullptr, false);
        if (!configJson.is_array()) {
            LOGE("Invalid JSON config.");
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        std::map<std::string, std::string> buildProperties;
        std::map<std::string, std::string> buildVersionProperties;

        for (const auto &entry : configJson) {
            if (!entry.contains("targets"))
                continue;

            auto targets = entry["targets"].get<std::vector<std::string>>();
            if (std::find(targets.begin(), targets.end(), processName) == targets.end())
                continue;

            if (entry.contains("build")) {
                auto buildConfig = entry["build"];
                if (buildConfig.is_object()) {
                    for (auto it = buildConfig.begin(); it != buildConfig.end(); ++it) {
                        if (it.key() == "version" && it.value().is_object()) {
                            for (auto &vEntry : it.value().items()) {
                                buildVersionProperties[vEntry.key()] = vEntry.value().get<std::string>();
                            }
                        } else {
                            buildProperties[it.key()] = it.value().get<std::string>();
                        }
                    }
                }
            }
            LOGD("Process %s matched profile: %s", processName, entry["name"].get<std::string>().c_str());
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
    // 更新指定 Java 类的静态字段
    //
    void updateClassStaticFields(JNIEnv *env, const char *className, const std::map<std::string, std::string> &properties) {
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

    //
    // 更新 android/os/Build 静态字段
    //
    void updateBuildProperties(const std::map<std::string, std::string> &properties) {
        updateClassStaticFields(env, "android/os/Build", properties);
    }

    //
    // 更新 android/os/Build$VERSION 静态字段
    //
    void updateBuildVersionProperties(const std::map<std::string, std::string> &properties) {
        updateClassStaticFields(env, "android/os/Build$VERSION", properties);
    }
};

// 注册 Zygisk 模块
REGISTER_ZYGISK_MODULE(FakeDeviceInfo)

// 注册 Companion 进程
REGISTER_ZYGISK_COMPANION(companionHandler)