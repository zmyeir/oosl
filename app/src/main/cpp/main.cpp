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
    
        uint8_t type = 1;
        int32_t nameSize = static_cast<int32_t>(strlen(processName));
        std::vector<uint8_t> requestBuffer(1 + 4 + nameSize);
        requestBuffer[0] = type;
        memcpy(requestBuffer.data() + 1, &nameSize, sizeof(nameSize));
        memcpy(requestBuffer.data() + 1 + 4, processName, nameSize);
    
        if (write(fd, requestBuffer.data(), requestBuffer.size()) != requestBuffer.size()) {
            LOGD("发送 Process Name 失败");
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        LOGD("已发送 Process Name: %s", processName);
    
        uint8_t responseType;
        int32_t responseSize;
        if (read(fd, &responseType, 1) != 1 || read(fd, &responseSize, 4) != 4) {
            LOGD("读取响应头失败");
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
    
        // **新增：判断未匹配到数据的情况**
        if (responseType == 3) {
            LOGD("Companion 未匹配到进程: %s，跳过伪装", processName);
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
    
        LOGD("接收到响应大小: %d", responseSize);
        if (responseSize <= 0) {
            LOGE("无效的 JSON 数据大小");
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
    
        std::vector<uint8_t> responseBuffer(responseSize);
        if (read(fd, responseBuffer.data(), responseSize) != responseSize) {
            LOGE("读取 JSON 配置失败");
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        close(fd);
    
        json profileJson = json::parse(responseBuffer, nullptr, false);
        if (!profileJson.is_object()) {
            LOGE("解析 JSON 失败或不是对象");
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
    
        LOGD("匹配到配置项: %s", profileJson["name"].get<std::string>().c_str());
    
        if (profileJson.contains("build") && profileJson["build"].is_object()) {
            spoofBuild = profileJson["build"].get<std::unordered_map<std::string, std::string>>();
            LOGD("获取到 %zu 个 Build 伪装参数", spoofBuild.size());
        }
    
        if (!spoofBuild.empty()) {
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
                    LOGD("已设置 '%s' 为 '%d'", fieldName, intValue);
                }

                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    LOGW("设置字段 '%s' 时发生异常", fieldName);
                    continue;
                }
            }
        }

        env->DeleteLocalRef(buildClass);
        env->DeleteLocalRef(versionClass);
        LOGD("UpdateBuildFields 处理完成");
    }
};

REGISTER_ZYGISK_MODULE(FakeDeviceInfo)
REGISTER_ZYGISK_COMPANION(Companion::FakeDeviceInfoD)