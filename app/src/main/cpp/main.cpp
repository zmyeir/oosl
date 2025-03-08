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
            LOGD("args 或 nice_name 为空，跳过处理");
            return;
        }

        const char *processName = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!processName) {
            LOGD("无法获取进程名称");
            return;
        }
        LOGD("当前进程名称: %s", processName);

        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGD("连接 Companion 失败，fd: %d", fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        int nameSize = static_cast<int>(strlen(processName));
        safeWrite(fd, &nameSize, sizeof(nameSize));
        safeWrite(fd, processName, nameSize);
        LOGD("已发送进程名称到 Companion，大小: %d", nameSize);

        int responseSize = 0;
        if (read(fd, &responseSize, sizeof(responseSize)) != sizeof(responseSize) || responseSize <= 0) {
            LOGD("读取响应大小失败或无效: %d", responseSize);
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        LOGD("接收到的 JSON 配置大小: %d", responseSize);

        std::vector<uint8_t> responseBuffer(responseSize);
        if (read(fd, responseBuffer.data(), responseSize) != responseSize) {
            LOGD("读取 JSON 配置数据失败");
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        close(fd);

        json profileJson = json::parse(responseBuffer, nullptr, false);
        if (!profileJson.is_object()) {
            LOGD("解析 JSON 失败或不是对象");
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }

        auto profile = std::make_shared<json>(profileJson);
        LOGD("匹配到配置项: %s", (*profile)["name"].get<std::string>().c_str());

        if (profile->contains("build") && (*profile)["build"].is_object()) {
            spoofBuild = (*profile)["build"].get<std::unordered_map<std::string, std::string>>();
            LOGD("获取到 %zu 个 Build 伪装参数", spoofBuild.size());
        }

        if (profile->contains("locale") && (*profile)["locale"].is_string()) {
            spoofLocale = (*profile)["locale"].get<std::string>();
            LOGD("将伪装 locale 为: %s", spoofLocale.c_str());
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
        LOGD("进入 preServerSpecialize");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        LOGD("preServerSpecialize 处理完成");
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::unordered_map<std::string, std::string> spoofBuild;
    std::string spoofLocale;

    void UpdateBuildFields() {
        LOGD("执行 UpdateBuildFields");
        jclass buildClass = env->FindClass("android/os/Build");
        jclass versionClass = env->FindClass("android/os/Build$VERSION");

        for (auto &[key, val] : spoofBuild) {
            LOGD("处理字段: %s -> %s", key.c_str(), val.c_str());
            const char *fieldName = key.c_str();
            jfieldID fieldID = nullptr;
            bool isStringField = true;

            fieldID = env->GetStaticFieldID(buildClass, fieldName, "Ljava/lang/String;");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                fieldID = env->GetStaticFieldID(versionClass, fieldName, "Ljava/lang/String;");
                
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    fieldID = env->GetStaticFieldID(versionClass, fieldName, "I");
                    if (!env->ExceptionCheck() && fieldID != nullptr) {
                        isStringField = false;
                    } else {
                        env->ExceptionClear();
                        LOGD("字段 %s 不存在或无法修改", fieldName);
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
                    LOGD("已设置 '%s' 为 '%s'", fieldName, val.c_str());
                } else {
                    int intValue = std::stoi(val);
                    env->SetStaticIntField(versionClass, fieldID, intValue);
                    LOGD("已设置 '%s' 为 %d", fieldName, intValue);
                }

                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    LOGD("设置字段 '%s' 时发生异常", fieldName);
                    continue;
                }
            }
        }

        env->DeleteLocalRef(buildClass);
        env->DeleteLocalRef(versionClass);
        LOGD("UpdateBuildFields 处理完成");
    }

    void UpdateLocale() {
        LOGD("执行 UpdateLocale");

        jclass localeClass = env->FindClass("java/util/Locale");
        jmethodID setDefaultMethod = env->GetStaticMethodID(localeClass, "setDefault", "(Ljava/util/Locale;)V");

        if (!setDefaultMethod || env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGD("无法找到 Locale.setDefault 方法");
            return;
        }

        jstring jLocaleStr = env->NewStringUTF(spoofLocale.c_str());
        jmethodID localeConstructor = env->GetMethodID(localeClass, "<init>", "(Ljava/lang/String;)V");
        jobject newLocale = env->NewObject(localeClass, localeConstructor, jLocaleStr);
        
        env->CallStaticVoidMethod(localeClass, setDefaultMethod, newLocale);
        LOGD("Locale 伪装完成: %s", spoofLocale.c_str());

        env->DeleteLocalRef(jLocaleStr);
        env->DeleteLocalRef(newLocale);
        env->DeleteLocalRef(localeClass);
    }
};

REGISTER_ZYGISK_MODULE(FakeDeviceInfo)
REGISTER_ZYGISK_COMPANION(Companion::FakeDeviceInfoD)