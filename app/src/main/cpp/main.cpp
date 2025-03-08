#include "utils.hpp"

#include "zygisk.hpp"

#include "companion.hpp"

using json = nlohmann::json;

class FakeDeviceInfo : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGD("FakeDeviceInfo 模块加载成功");
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        LOGD("启动 preAppSpecialize");
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

        int nameSize = static_cast<int>(strlen(processName));
        safeWrite(fd, &nameSize, sizeof(nameSize));
        safeWrite(fd, processName, nameSize);

        int responseSize = 0;
        if (read(fd, &responseSize, sizeof(responseSize)) != sizeof(responseSize) || responseSize <= 0) {
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        std::vector<uint8_t> responseBuffer(responseSize);
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
        auto profile = std::make_shared<json>(profileJson);
        LOGD("匹配到配置项: %s", (*profile)["name"].get<std::string>().c_str());

        if (profile->contains("build") && (*profile)["build"].is_object()) {
            spoofVars = (*profile)["build"].get<std::unordered_map<std::string, std::string>>();
        }

        if (!spoofVars.empty()) {
            UpdateBuildFields();
        }

        env->ReleaseStringUTFChars(args->nice_name, processName);
        LOGD("preAppSpecialize 处理完成");
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::unordered_map<std::string, std::string> spoofVars;

    void UpdateBuildFields() {
        LOGD("UpdateBuildFields");
        jclass buildClass = env->FindClass("android/os/Build");
        jclass versionClass = env->FindClass("android/os/Build$VERSION");
        
        for (auto &[key, val]: spoofVars) {
            const char *fieldName = key.c_str();
            
            jfieldID fieldID = env->GetStaticFieldID(buildClass, fieldName, "Ljava/lang/String;");
            bool isStringField = true;
    
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                
                fieldID = env->GetStaticFieldID(versionClass, fieldName, "I");
                if (!env->ExceptionCheck() && fieldID != nullptr) {
                    isStringField = false;
                } else {
                    env->ExceptionClear();
                    continue;
                }
            }
    
            if (fieldID != nullptr) {
                if (isStringField) {
                    jstring jValue = env->NewStringUTF(val.c_str());
                    env->SetStaticObjectField(buildClass, fieldID, jValue);
                    env->DeleteLocalRef(jValue);
                    LOGD("Set string field '%s' to '%s'", fieldName, val.c_str());
                } else {
                    int intValue = std::stoi(val);
                    env->SetStaticIntField(versionClass, fieldID, intValue);
                    LOGD("Set int field '%s' to %d", fieldName, intValue);
                }
    
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    continue;
                }
            }
        }
    
        env->DeleteLocalRef(buildClass);
        env->DeleteLocalRef(versionClass);
    }
    
};


REGISTER_ZYGISK_MODULE(FakeDeviceInfo)
REGISTER_ZYGISK_COMPANION(Companion::FakeDeviceInfoD)
