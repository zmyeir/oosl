#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <filesystem>
#include "zygisk.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std;
using namespace zygisk;

#define LOG_TAG "FDI"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CONFIG_FILE "/data/adb/fdi.json"
#define FALLBACK_FILE "/data/local/tmp/fdi.json"

//
// Companion 数据结构
//
struct CompanionData {
    vector<uint8_t> configBuffer;
};

//
// 安全读取文件内容（返回空 vector 表示失败）
//
static vector<uint8_t> readFileContents(const char *filePath) {
    std::error_code ec;
    size_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec) {
        LOGE("无法获取文件大小: %s, 错误: %s", filePath, ec.message().c_str());
        return {};
    }
    LOGD("读取文件: %s, 文件大小: %zu 字节", filePath, fileSize);

    FILE *file = fopen(filePath, "rb");
    if (!file) {
        LOGE("无法打开文件: %s", filePath);
        return {};
    }

    vector<uint8_t> buffer(fileSize);
    size_t bytesRead = fread(buffer.data(), 1, buffer.size(), file);
    if (bytesRead != buffer.size()) {
        LOGE("读取文件失败: %s, 预期字节: %zu, 实际读取: %zu", filePath, buffer.size(), bytesRead);
        buffer.clear();
    } else {
        LOGD("成功读取文件: %s", filePath);
    }

    fclose(file);
    return buffer;
}

//
// 备份配置文件到指定路径
//
static bool backupConfigFile(const char *sourcePath, const char *backupPath) {
    LOGD("尝试备份配置文件: 从 %s 到 %s", sourcePath, backupPath);
    std::error_code errorCode;
    std::filesystem::copy_file(sourcePath, backupPath,
                               std::filesystem::copy_options::overwrite_existing,
                               errorCode);
    if (errorCode) {
        LOGE("备份失败: 无法将 %s 备份到 %s, 错误: %s", sourcePath, backupPath, errorCode.message().c_str());
        return false;
    }
    LOGD("备份成功: %s 已经备份到 %s", sourcePath, backupPath);
    return true;
}

//
// 安全写入函数：确保将 size 字节全部写入 fd
//
static bool safeWrite(int fd, const void *buffer, size_t size) {
    size_t written = 0;
    const uint8_t *buf = static_cast<const uint8_t*>(buffer);
    while (written < size) {
        ssize_t result = write(fd, buf + written, size - written);
        if (result <= 0) {
            LOGE("写入失败，返回值: %zd, 已写入字节: %zu", result, written);
            return false;
        }
        written += result;
        LOGD("已写入: %zu/%zu 字节", written, size);
    }
    return true;
}

//
// 从 companion 读取配置数据
//
static bool readCompanionData(int fd, CompanionData &data) {
    int configSize = 0;
    ssize_t bytesRead = read(fd, &configSize, sizeof(configSize));
    if (bytesRead != sizeof(configSize) || configSize <= 0) {
        LOGE("从 companion 读取配置大小失败或无效: 读取字节: %zd, configSize: %d", bytesRead, configSize);
        return false;
    }
    LOGD("从 companion 读取到的配置大小: %d 字节", configSize);

    data.configBuffer.resize(static_cast<size_t>(configSize));
    bytesRead = read(fd, data.configBuffer.data(), data.configBuffer.size());
    if (bytesRead != static_cast<ssize_t>(data.configBuffer.size())) {
        LOGE("读取配置数据失败: 预期 %zu 字节, 实际读取 %zd 字节", data.configBuffer.size(), bytesRead);
        data.configBuffer.clear();
        return false;
    }
    LOGD("成功从 companion 读取配置数据");
    return true;
}

//
// 加载有效的配置数据：优先使用 CONFIG_FILE，其格式合法时备份，然后回退到 FALLBACK_FILE
//
static vector<uint8_t> loadValidConfigData() {
    LOGD("尝试加载主配置文件: %s", CONFIG_FILE);
    vector<uint8_t> configBuffer = readFileContents(CONFIG_FILE);
    if (!configBuffer.empty() && json::accept(configBuffer)) {
        LOGD("主配置文件 JSON 格式合法");
        if (backupConfigFile(CONFIG_FILE, FALLBACK_FILE)) {
            LOGD("配置文件备份成功");
        }
        return configBuffer;
    }

    LOGE("主配置文件 JSON 格式无效，尝试加载备用配置文件: %s", FALLBACK_FILE);
    configBuffer = readFileContents(FALLBACK_FILE);
    if (!configBuffer.empty() && json::accept(configBuffer)) {
        LOGW("使用备用配置文件");
        return configBuffer;
    }

    LOGE("备用配置文件也无效，配置已清空");
    return {};
}

//
// Companion 处理程序：写入配置数据到 fd
//
static void companionHandler(int fd) {
    LOGD("companionHandler 开始执行");
    vector<uint8_t> configBuffer = loadValidConfigData();
    int configSize = static_cast<int>(configBuffer.size());
    LOGD("写入配置数据，大小: %d 字节", configSize);
    if (!safeWrite(fd, &configSize, sizeof(configSize))) {
        LOGE("写入配置大小失败");
        return;
    }

    if (!configBuffer.empty() && !safeWrite(fd, configBuffer.data(), configBuffer.size())) {
        LOGE("写入配置数据失败");
    } else {
        LOGD("配置数据成功写入");
    }
}

//
// 模块实现：根据配置更新 Android Build 属性
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
            LOGE("无效的 AppSpecializeArgs");
            return;
        }

        const char *processName = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!processName) {
            LOGE("无法获取进程名称");
            return;
        }
        LOGD("当前进程名称: %s", processName);

        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGE("连接 companion 失败");
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        LOGD("成功连接到 companion, fd: %d", fd);

        CompanionData data;
        if (!readCompanionData(fd, data)) {
            LOGE("从 companion 读取数据失败");
            close(fd);
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        close(fd);

        json configJson = json::parse(data.configBuffer, nullptr, false);
        if (!configJson.is_array()) {
            LOGE("解析 JSON 配置失败或格式不为数组");
            env->ReleaseStringUTFChars(args->nice_name, processName);
            return;
        }
        LOGD("成功解析 JSON 配置, 配置项数量: %zu", configJson.size());

        map<string, string> buildProperties;
        map<string, string> buildVersionProperties;

        for (const auto &entry : configJson) {
            if (!entry.contains("targets")) {
                LOGD("配置项缺少 targets 字段, 跳过");
                continue;
            }

            auto targets = entry["targets"].get<vector<string>>();
            LOGD("配置项 targets: %zu 项", targets.size());
            if (find(targets.begin(), targets.end(), processName) == targets.end()) {
                LOGD("当前进程 %s 不在 targets 列表中", processName);
                continue;
            }

            if (entry.contains("build")) {
                auto buildConfig = entry["build"];
                if (buildConfig.is_object()) {
                    for (auto it = buildConfig.begin(); it != buildConfig.end(); ++it) {
                        if (it.key() == "version" && it.value().is_object()) {
                            for (auto &vEntry : it.value().items()) {
                                buildVersionProperties[vEntry.key()] = vEntry.value().get<string>();
                                LOGD("解析 build version: %s -> %s", vEntry.key().c_str(), vEntry.value().get<string>().c_str());
                            }
                        } else {
                            buildProperties[it.key()] = it.value().get<string>();
                            LOGD("解析 build 属性: %s -> %s", it.key().c_str(), it.value().get<string>().c_str());
                        }
                    }
                }
            }
            LOGD("匹配到配置项: %s", entry["name"].get<string>().c_str());
            break;
        }

        if (!buildProperties.empty())
            updateBuildProperties(buildProperties);
        if (!buildVersionProperties.empty())
            updateBuildVersionProperties(buildVersionProperties);

        env->ReleaseStringUTFChars(args->nice_name, processName);
        LOGD("preAppSpecialize 处理完成");
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize 开始");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;

    //
    // 通过 JNI 更新指定 Java 类的静态字段
    //
    void updateClassStaticFields(JNIEnv *env, const char *className, const map<string, string> &properties) {
        jclass targetClass = env->FindClass(className);
        if (!targetClass) {
            LOGE("未找到类 %s", className);
            return;
        }

        for (const auto &prop : properties) {
            jfieldID fieldID = env->GetStaticFieldID(targetClass, prop.first.c_str(), "Ljava/lang/String;");
            if (!fieldID) {
                LOGD("在 %s 中未找到字段 '%s', 跳过...", className, prop.first.c_str());
                continue;
            }

            jstring jValue = env->NewStringUTF(prop.second.c_str());
            env->SetStaticObjectField(targetClass, fieldID, jValue);
            env->DeleteLocalRef(jValue);
            LOGD("更新 %s.%s 为 '%s'", className, prop.first.c_str(), prop.second.c_str());
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