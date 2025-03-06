#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include "zygisk.hpp"
#include "json.hpp"
#include "companion_handler.hpp"

#include "logger.hpp"

using json = nlohmann::json;
using namespace std;
using namespace zygisk;

class FakeDeviceInfo : public ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        LOGD("preAppSpecialize started.");
        api->setOption(DLCLOSE_MODULE_LIBRARY);

        if (!args || !args->nice_name)
            return;

        const char *processName = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!processName)
            return;

        LOGD("Processing process: %s", processName);

        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGE("Failed to connect to companion for process: %s", processName);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        // 调用分离出来的 companionHandler 处理 companion 逻辑
        companionHandler(fd);
        close(fd);

        // 此处可继续根据 companion 返回的配置数据进行后续处理...
        // （例如解析 JSON 更新 Build 属性等）

        env->ReleaseStringUTFChars(args->nice_name, processName);
        LOGD("preAppSpecialize finished for process: %s", processName);
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize started.");
        api->setOption(DLCLOSE_MODULE_LIBRARY);
        LOGD("preServerSpecialize finished.");
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;

    // 可复用更新 Build 属性的代码（保持原有实现）
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