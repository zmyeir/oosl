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
            spoofBuild = (*profile)["build"].get<std::unordered_map<std::string, std::string>>();
        }

        // 添加语言伪装
        if (profile->contains("locale") && (*profile)["locale"].is_string()) {
            spoofLocale = (*profile)["locale"].get<std::string>();
            LOGD("Will spoof locale to: %s", spoofLocale.c_str());
        }

        if (!spoofBuild.empty()) {
            UpdateBuildFields();
        }

        if (!spoofLocale.empty()) {
            UpdateLocale();
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
    std::unordered_map<std::string, std::string> spoofBuild;
    std::string spoofTimezone;
    std::string spoofLocale;

    void UpdateBuildFields() {
        LOGD("UpdateBuildFields");
        jclass buildClass = env->FindClass("android/os/Build");
        jclass versionClass = env->FindClass("android/os/Build$VERSION");
    
        for (auto &[key, val] : spoofBuild) {
            const char *fieldName = key.c_str();
            jfieldID fieldID = nullptr;
            bool isStringField = true;
    
            // 先尝试在 android.os.Build 里面找 String 字段
            fieldID = env->GetStaticFieldID(buildClass, fieldName, "Ljava/lang/String;");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                fieldID = env->GetStaticFieldID(versionClass, fieldName, "Ljava/lang/String;");
    
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    // 再尝试 int 类型
                    fieldID = env->GetStaticFieldID(versionClass, fieldName, "I");
                    if (!env->ExceptionCheck() && fieldID != nullptr) {
                        isStringField = false;
                    } else {
                        env->ExceptionClear();
                        continue;
                    }
                }
            }
    
            if (fieldID != nullptr) {
                if (isStringField) {
                    jstring jValue = env->NewStringUTF(val.c_str());
                    if (env->IsSameObject(buildClass, versionClass)) {
                        env->SetStaticObjectField(versionClass, fieldID, jValue);
                    } else {
                        env->SetStaticObjectField(buildClass, fieldID, jValue);
                    }
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

    void UpdateLocale() {
        LOGD("Updating locale to: %s", spoofLocale.c_str());
        
        // 获取 Locale 类
        jclass localeClass = env->FindClass("java/util/Locale");
        if (!localeClass || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGD("Failed to find Locale class");
            return;
        }
        
        // 解析语言代码和国家代码
        std::string language = spoofLocale;
        std::string country = "";
        size_t separatorPos = spoofLocale.find('-');
        if (separatorPos != std::string::npos) {
            language = spoofLocale.substr(0, separatorPos);
            country = spoofLocale.substr(separatorPos + 1);
        } else {
            separatorPos = spoofLocale.find('_');
            if (separatorPos != std::string::npos) {
                language = spoofLocale.substr(0, separatorPos);
                country = spoofLocale.substr(separatorPos + 1);
            }
        }
        
        // 创建 Locale 实例
        jmethodID localeConstructor;
        jobject localeInstance;
        jstring langString = env->NewStringUTF(language.c_str());
        
        if (!country.empty()) {
            // 使用语言和国家创建
            jstring countryString = env->NewStringUTF(country.c_str());
            localeConstructor = env->GetMethodID(localeClass, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V");
            localeInstance = env->NewObject(localeClass, localeConstructor, langString, countryString);
            env->DeleteLocalRef(countryString);
        } else {
            // 只使用语言创建
            localeConstructor = env->GetMethodID(localeClass, "<init>", "(Ljava/lang/String;)V");
            localeInstance = env->NewObject(localeClass, localeConstructor, langString);
        }
        
        if (!localeInstance || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGD("Failed to create Locale instance");
            env->DeleteLocalRef(langString);
            env->DeleteLocalRef(localeClass);
            return;
        }
        
        // 设置为默认语言
        jmethodID setDefaultMethod = env->GetStaticMethodID(localeClass, "setDefault", 
                                                         "(Ljava/util/Locale;)V");
        if (!setDefaultMethod || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGD("Failed to find setDefault method");
            env->DeleteLocalRef(localeInstance);
            env->DeleteLocalRef(langString);
            env->DeleteLocalRef(localeClass);
            return;
        }
        
        env->CallStaticVoidMethod(localeClass, setDefaultMethod, localeInstance);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGD("Failed to set default locale");
        } else {
            LOGD("Successfully set locale to: %s", spoofLocale.c_str());
        }
        
        // 释放引用
        env->DeleteLocalRef(localeInstance);
        env->DeleteLocalRef(langString);
        env->DeleteLocalRef(localeClass);
    }
};

REGISTER_ZYGISK_MODULE(FakeDeviceInfo)
REGISTER_ZYGISK_COMPANION(Companion::FakeDeviceInfoD)
