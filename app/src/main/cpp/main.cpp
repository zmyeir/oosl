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
            fakeBuildVars = (*profile)["build"].get<std::unordered_map<std::string, std::string>>();
        }

        if (profile->contains("locale") && (*profile)["locale"].is_array()) {
            for (const auto& localeItem : (*profile)["locale"]) {
                if (!localeItem.contains("tz") || !localeItem.contains("lang")) {
                    continue;
                }
                fakeTimezone = localeItem["tz"].get<std::string>();
                fakeLanguage = localeItem["lang"].get<std::string>();
                break;  // 只使用第一个匹配项
            }
        }

        if (!fakeBuildVars.empty()) {
            UpdateBuildFields();
        }

        if (!fakeLanguage.empty() || !fakeTimezone.empty()) {
            UpdateLocaleAndTimezone();
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
    std::unordered_map<std::string, std::string> fakeBuildVars;
    std::string fakeTimezone;
    std::string fakeLanguage;

    void UpdateBuildFields() {
        LOGD("UpdateBuildFields");
        jclass buildClass = env->FindClass("android/os/Build");
        jclass versionClass = env->FindClass("android/os/Build$VERSION");

        for (auto &[key, val] : fakeBuildVars) {
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

    void UpdateLocaleAndTimezone() {
        LOGD("UpdateLocaleAndTimezone");

        if (!fakeLanguage.empty()) {
            jclass localeClass = env->FindClass("java/util/Locale");
            jmethodID localeCtor = env->GetMethodID(localeClass, "<init>", "(Ljava/lang/String;)V");
            jmethodID setDefault = env->GetStaticMethodID(localeClass, "setDefault", "(Ljava/util/Locale;)V");

            jstring jLang = env->NewStringUTF(fakeLanguage.c_str());
            jobject newLocale = env->NewObject(localeClass, localeCtor, jLang);
            env->CallStaticVoidMethod(localeClass, setDefault, newLocale);

            env->DeleteLocalRef(jLang);
            env->DeleteLocalRef(newLocale);
            env->DeleteLocalRef(localeClass);
            LOGD("语言伪造完成: %s", fakeLanguage.c_str());
        }

        if (!fakeTimezone.empty()) {
            jclass timezoneClass = env->FindClass("java/util/TimeZone");
            jmethodID getTimeZone = env->GetStaticMethodID(timezoneClass, "getTimeZone", "(Ljava/lang/String;)Ljava/util/TimeZone;");
            jmethodID setDefault = env->GetStaticMethodID(timezoneClass, "setDefault", "(Ljava/util/TimeZone;)V");

            jstring jTz = env->NewStringUTF(fakeTimezone.c_str());
            jobject newTimezone = env->CallStaticObjectMethod(timezoneClass, getTimeZone, jTz);
            env->CallStaticVoidMethod(timezoneClass, setDefault, newTimezone);

            env->DeleteLocalRef(jTz);
            env->DeleteLocalRef(newTimezone);
            env->DeleteLocalRef(timezoneClass);
            LOGD("时区伪造完成: %s", fakeTimezone.c_str());
        }
    }
};

REGISTER_ZYGISK_MODULE(FakeDeviceInfo)
REGISTER_ZYGISK_COMPANION(Companion::FakeDeviceInfoD)
