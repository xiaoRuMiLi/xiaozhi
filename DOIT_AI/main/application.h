#ifndef _APPLICATION_H_
#define _APPLICATION_H_

// 包含 FreeRTOS 相关头文件，用于任务和事件管理
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

// 包含标准 C++ 库头文件
#include <string>        // 字符串处理
#include <mutex>         // 互斥锁
#include <list>          // 双向链表
#include <vector>        // 动态数组
#include <condition_variable>  // 条件变量
#include <memory>        // 智能指针

// 包含音频处理相关头文件
#include <opus_encoder.h>    // Opus 音频编码器
#include <opus_decoder.h>    // Opus 音频解码器
#include <opus_resampler.h>  // Opus 音频重采样器

// 包含项目自定义头文件
#include "protocol.h"        // 协议处理
#include "ota.h"            // 在线升级
#include "background_task.h" // 后台任务
#include "audio_processor.h" // 音频处理器

// 条件编译：如果启用了唤醒词检测功能，则包含相关头文件
#if CONFIG_USE_WAKE_WORD_DETECT
#include "wake_word_detect.h"
#endif

// 定义事件标志位
#define SCHEDULE_EVENT (1 << 0)              // 调度事件
#define AUDIO_INPUT_READY_EVENT (1 << 1)     // 音频输入就绪事件
#define AUDIO_OUTPUT_READY_EVENT (1 << 2)    // 音频输出就绪事件
#define CHECK_NEW_VERSION_DONE_EVENT (1 << 3) // 检查新版本完成事件

// 定义设备状态枚举
enum DeviceState {
    kDeviceStateUnknown,        // 未知状态
    kDeviceStateStarting,       // 启动中
    kDeviceStateWifiConfiguring,// WiFi配置中
    kDeviceStateIdle,          // 空闲状态
    kDeviceStateConnecting,    // 连接中
    kDeviceStateListening,     // 监听状态
    kDeviceStateSpeaking,      // 说话状态
    kDeviceStateUpgrading,     // 升级中
    kDeviceStateActivating,    // 激活中
    kDeviceStateFatalError     // 致命错误
};

// 定义 Opus 音频帧持续时间（毫秒）
#define OPUS_FRAME_DURATION_MS 60

// Application 类定义
class Application {
public:
    // 获取 Application 单例实例
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    
    // 删除拷贝构造函数和赋值运算符，确保单例模式
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 公共接口函数
    void Start();  // 启动应用
    DeviceState GetDeviceState() const { return device_state_; }  // 获取设备状态
    bool IsVoiceDetected() const { return voice_detected_; }      // 检测是否有声音
    void Schedule(std::function<void()> callback);  // 调度任务
    void SetDeviceState(DeviceState state);         // 设置设备状态
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");  // 发送告警
    void DismissAlert();  // 关闭告警
    void AbortSpeaking(AbortReason reason);  // 中止说话
    void ToggleChatState();  // 切换聊天状态
    void StartListening();   // 开始监听
    void StopListening();    // 停止监听
    void UpdateIotStates();  // 更新物联网设备状态
    void Reboot();          // 重启设备
    void WakeWordInvoke(const std::string& wake_word);  // 唤醒词触发
    void PlaySound(const std::string_view& sound);      // 播放声音
    bool CanEnterSleepMode();  // 检查是否可以进入睡眠模式

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    void ReleaseDecoder();  // 释放解码器
    void ShowOtaInfo(const std::string& code, const std::string& ip="");  // 显示OTA信息
#endif

private:
    Application();  // 构造函数
    ~Application(); // 析构函数

    // 成员变量
#if CONFIG_USE_WAKE_WORD_DETECT
    WakeWordDetect wake_word_detect_;  // 唤醒词检测器
#endif
    std::unique_ptr<AudioProcessor> audio_processor_;  // 音频处理器
    Ota ota_;  // OTA升级管理器
    std::mutex mutex_;  // 互斥锁
    std::list<std::function<void()>> main_tasks_;  // 主任务列表
    std::unique_ptr<Protocol> protocol_;  // 协议处理器
    EventGroupHandle_t event_group_ = nullptr;  // 事件组句柄
    esp_timer_handle_t clock_timer_handle_ = nullptr;  // 时钟定时器句柄
    volatile DeviceState device_state_ = kDeviceStateUnknown;  // 设备状态
    ListeningMode listening_mode_ = kListeningModeAutoStop;  // 监听模式
#if CONFIG_USE_DEVICE_AEC || CONFIG_USE_SERVER_AEC
    bool realtime_chat_enabled_ = true;  // 实时聊天使能
#else
    bool realtime_chat_enabled_ = false;
#endif
    bool aborted_ = false;  // 中止标志
    bool voice_detected_ = false;  // 声音检测标志
    bool busy_decoding_audio_ = false;  // 音频解码忙标志
    int clock_ticks_ = 0;  // 时钟计数
    TaskHandle_t check_new_version_task_handle_ = nullptr;  // 检查新版本任务句柄

    // 音频编解码相关成员
    TaskHandle_t audio_loop_task_handle_ = nullptr;  // 音频循环任务句柄
    BackgroundTask* background_task_ = nullptr;  // 后台任务指针
    std::chrono::steady_clock::time_point last_output_time_;  // 上次输出时间
    std::atomic<uint32_t> last_output_timestamp_ = 0;  // 上次输出时间戳
    std::list<AudioStreamPacket> audio_decode_queue_;  // 音频解码队列
    std::condition_variable audio_decode_cv_;  // 音频解码条件变量

    // Opus 编解码器相关成员
    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;  // Opus编码器
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;  // Opus解码器

    // 音频重采样器
    OpusResampler input_resampler_;     // 输入重采样器
    OpusResampler reference_resampler_; // 参考重采样器
    OpusResampler output_resampler_;    // 输出重采样器

    // 私有成员函数
    void MainEventLoop();  // 主事件循环
    void OnAudioInput();   // 音频输入处理
    void OnAudioOutput();  // 音频输出处理
    void ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples);  // 读取音频数据
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
    void ReadAudio(std::vector<uint8_t>& opus, int sample_rate, int samples);  // 读取Opus编码音频
#endif
    void WriteAudio(std::vector<int16_t>& data, int sample_rate);  // 写入音频数据
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
    void WriteAudio(std::vector<uint8_t>& opus, int sample_rate);  // 写入Opus编码音频
#endif
    void ResetDecoder();  // 重置解码器
    void SetDecodeSampleRate(int sample_rate, int frame_duration);  // 设置解码采样率
    void CheckNewVersion();  // 检查新版本
    void ShowActivationCode();  // 显示激活码
    void OnClockTimer();  // 时钟定时器回调
    void SetListeningMode(ListeningMode mode);  // 设置监听模式
    void AudioLoop();  // 音频循环处理
};

#endif // _APPLICATION_H_
