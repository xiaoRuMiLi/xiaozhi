#include "application.h" // 引入 Application 类的头文件，定义了主应用逻辑相关内容
#include "board.h" // 引入 Board 类头文件，提供硬件板级相关接口
#include "display.h" // 引入 Display 类头文件，提供显示屏相关接口
#include "system_info.h" // 引入 system_info 头文件，提供系统信息获取接口
#include "ml307_ssl_transport.h" // 引入 ML307 SSL 传输相关头文件，提供安全通信能力
#include "audio_codec.h" // 引入音频编解码器头文件，提供音频输入输出能力
#include "mqtt_protocol.h" // 引入 MQTT 协议头文件，提供 MQTT 通信能力
#include "websocket_protocol.h" // 引入 WebSocket 协议头文件，提供 WebSocket 通信能力
#include "font_awesome_symbols.h" // 引入字体图标头文件，提供 UI 图标符号
#include "iot/thing_manager.h" // 引入 IoT 设备管理器头文件，管理物联网设备
#include "assets/lang_config.h" // 引入多语言配置头文件，提供多语言字符串

#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h" // 如果启用音频处理器，包含 AfeAudioProcessor 头文件
#else
#include "dummy_audio_processor.h" // 否则包含 DummyAudioProcessor 头文件，作为占位
#endif

#include <cstring> // C++ 标准库，提供字符串操作
#include <esp_log.h> // ESP-IDF 日志库，提供日志输出功能
#include <cJSON.h> // cJSON 库，提供 JSON 解析能力
#include <driver/gpio.h> // ESP-IDF GPIO 驱动，提供 GPIO 控制
#include <arpa/inet.h> // 提供字节序转换等网络相关函数

#define TAG "Application" // 定义日志 TAG，用于日志输出标识

#if defined(CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS) && (CONFIG_USE_WAKE_WORD_DETECT || CONFIG_USE_AUDIO_PROCESSOR)
#error "audoio_processor or wake_word_detect need opus encoder" // 编译期检查，防止配置冲突
#endif

#ifndef CONFIG_TACKGROUND_TASK_STACK_SIZE
#define CONFIG_TACKGROUND_TASK_STACK_SIZE   (4096*8) // 定义后台任务栈大小，默认 32KB
#endif

#ifndef CONFIG_AUDIO_LOOP_TASK_STACK_SIZE
#define CONFIG_AUDIO_LOOP_TASK_STACK_SIZE   (4096*2) // 定义音频循环任务栈大小，默认 8KB
#endif

static const char* const STATE_STRINGS[] = { // 定义设备状态字符串数组，便于日志和 UI 显示
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "fatal_error",
    "invalid_state"
};

Application::Application() { // Application 构造函数，初始化应用各模块
    event_group_ = xEventGroupCreate(); // 创建事件组（FreeRTOS），用于任务间同步
    background_task_ = new BackgroundTask(CONFIG_TACKGROUND_TASK_STACK_SIZE); // 创建后台任务对象，负责异步处理

#if CONFIG_USE_AUDIO_PROCESSOR
    /**
    std::make_unique<AfeAudioProcessor>() 会在堆上分配一个 AfeAudioProcessor 类型的对象，并返回一个指向该对象的 std::unique_ptr<AfeAudioProcessor> 智能指针。
     */
    audio_processor_ = std::make_unique<AfeAudioProcessor>(); // 根据配置创建音频处理器实例（AfeAudioProcessor），引用自 afe_audio_processor.h
#else
    audio_processor_ = std::make_unique<DummyAudioProcessor>(); // 否则创建 DummyAudioProcessor 占位实例，引用自 dummy_audio_processor.h
#endif

    esp_timer_create_args_t clock_timer_args = { // 定义定时器参数结构体，引用自 esp_timer.h
        /*[](void* arg) { ... } 是 C++ 中的Lambda 表达式（也叫匿名函数或闭包），它允许你在代码中内联定义一个函数，而不必单独声明一个命名函数。*/
        .callback = [](void* arg) { // 定时器回调函数，触发时调用 Application::OnClockTimer
            Application* app = (Application*)arg;
            app->OnClockTimer();
        },
        .arg = this, // 传递当前 Application 实例指针
        .dispatch_method = ESP_TIMER_TASK, // 以任务方式调度
        .name = "clock_timer", // 定时器名称
        .skip_unhandled_events = true // 跳过未处理事件
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_); // 创建定时器，引用自 esp_timer.h
    esp_timer_start_periodic(clock_timer_handle_, 1000000); // 启动定时器，周期 1 秒
}

Application::~Application() { // 析构函数，释放资源
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_); // 停止定时器
        esp_timer_delete(clock_timer_handle_); // 删除定时器
    }
    if (background_task_ != nullptr) {
        delete background_task_; // 释放后台任务对象
    }
    vEventGroupDelete(event_group_); // 删除事件组
}

void Application::CheckNewVersion() { // 检查新固件版本，涉及 OTA 升级，引用自 ota_ 相关成员
    const int MAX_RETRY = 10; // 最大重试次数
    int retry_count = 0; // 当前重试次数
    int retry_delay = 10; // 初始重试延迟为10秒

    while (true) { // 无限循环，直到检查完成或达到最大重试
        SetDeviceState(kDeviceStateActivating); // 设置设备状态为"激活中"，引用自本类成员
        auto display = Board::GetInstance().GetDisplay(); // 获取显示屏对象，引用自 board.h
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION); // 显示"正在检查新版本"，引用自 lang_config.h

        if (!ota_.CheckVersion()) { // 检查新版本失败，引用自 ota_ 成员
            retry_count++; // 重试次数加一
            if (retry_count >= MAX_RETRY) { // 超过最大重试次数
                ESP_LOGE(TAG, "Too many retries, exit version check"); // 输出错误日志，引用自 esp_log.h
                return; // 退出函数
            }

            char buffer[128]; // 定义缓冲区
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota_.GetCheckVersionUrl().c_str()); // 格式化失败信息
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION); // 弹出警告，引用自本类 Alert 方法

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY); // 输出警告日志
            for (int i = 0; i < retry_delay; i++) { // 延迟 retry_delay 秒
                vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒延迟，引用自 freertos/FreeRTOS.h
                if (device_state_ == kDeviceStateIdle) { // 如果设备已空闲，提前跳出
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue; // 继续下一轮重试
        }
        retry_count = 0; // 重置重试次数
        retry_delay = 10; // 重置重试延迟时间

        if (ota_.HasNewVersion()) { // 检查到有新版本，引用自 ota_ 成员
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE); // 弹出升级提示

            vTaskDelay(pdMS_TO_TICKS(3000)); // 延迟3秒

            SetDeviceState(kDeviceStateUpgrading); // 设置设备状态为"升级中"
            
            display->SetIcon(FONT_AWESOME_DOWNLOAD); // 显示下载图标，引用自 font_awesome_symbols.h
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota_.GetFirmwareVersion(); // 拼接新版本信息
            display->SetChatMessage("system", message.c_str()); // 显示新版本信息

            auto& board = Board::GetInstance(); // 获取 Board 单例
            board.SetPowerSaveMode(false); // 关闭省电模式
#if CONFIG_USE_WAKE_WORD_DETECT
            wake_word_detect_.StopDetection(); // 停止唤醒词检测，引用自本类成员
#endif
            // 预先关闭音频输出，避免升级过程有音频操作
            auto codec = board.GetAudioCodec(); // 获取音频编解码器对象
            codec->EnableInput(false); // 关闭音频输入
            codec->EnableOutput(false); // 关闭音频输出
            {
                std::lock_guard<std::mutex> lock(mutex_); // 加锁，保护音频解码队列
                audio_decode_queue_.clear(); // 清空音频解码队列
            }
            background_task_->WaitForCompletion(); // 等待后台任务完成
            delete background_task_; // 删除后台任务对象
            background_task_ = nullptr; // 指针置空
            vTaskDelay(pdMS_TO_TICKS(1000)); // 延迟1秒

            ota_.StartUpgrade([display](int progress, size_t speed) { // 启动 OTA 升级，传入进度回调
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%d%% %zuKB/s", progress, speed / 1024); // 格式化进度信息
                display->SetChatMessage("system", buffer); // 显示进度信息
            });

            // If upgrade success, the device will reboot and never reach here
            display->SetStatus(Lang::Strings::UPGRADE_FAILED); // 升级失败，显示失败状态
            ESP_LOGI(TAG, "Firmware upgrade failed..."); // 输出日志
            vTaskDelay(pdMS_TO_TICKS(3000)); // 延迟3秒
            Reboot(); // 重启设备，引用自本类方法
            return; // 退出函数
        }

        // No new version, mark the current version as valid
        ota_.MarkCurrentVersionValid(); // 标记当前版本为有效
        if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge()) { // 没有激活码和激活挑战，检查完成
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT); // 设置事件位，通知主循环
            // Exit the loop if done checking new version
            break; // 跳出循环
        }

        display->SetStatus(Lang::Strings::ACTIVATION); // 显示"激活中"状态
        // Activation code is shown to the user and waiting for the user to input
        if (ota_.HasActivationCode()) { // 如果有激活码
            ShowActivationCode(); // 显示激活码，引用自本类方法
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) { // 最多尝试10次激活
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10); // 输出日志
            esp_err_t err = ota_.Activate(); // 执行激活操作，引用自 ota_ 成员
            if (err == ESP_OK) { // 激活成功
                xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT); // 设置事件位
                break; // 跳出循环
            } else if (err == ESP_ERR_TIMEOUT) { // 超时
                vTaskDelay(pdMS_TO_TICKS(3000)); // 延迟3秒
            } else { // 其他错误
                vTaskDelay(pdMS_TO_TICKS(10000)); // 延迟10秒
            }
            if (device_state_ == kDeviceStateIdle) { // 如果设备已空闲，提前跳出
                break;
            }
        }
    }
}

// 显示激活码函数
void Application::ShowActivationCode() {
    auto& message = ota_.GetActivationMessage(); // 获取激活提示信息，引用自 ota_ 成员
    auto& code = ota_.GetActivationCode(); // 获取激活码字符串，引用自 ota_ 成员

    // 定义数字对应的语音提示
    struct digit_sound {
        char digit; // 数字字符
        const std::string_view& sound; // 对应的语音资源
    };
    /**static这是一个静态关键字
    表示这个数组是静态的，只会被创建一次
    所有类的实例共享这个数组
    在程序运行期间一直存在 */
    /**
    std::array<digit_sound, 10>：
    std::array 是 C++ 标准库中的固定大小数组容器
    <digit_sound, 10> 是模板参数：
    digit_sound 是数组元素的类型
    10 是数组的大小（固定为10个元素）
     */
    static const std::array<digit_sound, 10> digit_sounds{{ // 静态数组，映射数字到语音，引用自 lang_config.h
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // 播放激活提示音
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION); // 调用 Alert 方法，显示激活提示并播放音效

    // 逐个播放激活码数字的语音
    for (const auto& digit : code) { // 遍历激活码字符串
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(), // 查找对应数字的语音
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlaySound(it->sound); // 播放对应数字的语音，引用自本类 PlaySound 方法
        }
    }
}

// 显示警告信息
void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion); // 输出警告日志，引用自 esp_log.h
    auto display = Board::GetInstance().GetDisplay(); // 获取显示屏对象，引用自 board.h
    display->SetStatus(status); // 设置显示屏状态
    display->SetEmotion(emotion); // 设置表情
    display->SetChatMessage("system", message); // 显示系统消息
    if (!sound.empty()) { // 如果有声音资源
        ResetDecoder(); // 重置音频解码器，引用自本类方法
        PlaySound(sound); // 播放声音，引用自本类方法
    }
}

// 关闭警告信息
void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) { // 仅在空闲状态下关闭警告
        auto display = Board::GetInstance().GetDisplay(); // 获取显示屏对象
        display->SetStatus(Lang::Strings::STANDBY); // 设置为待机状态
        display->SetEmotion("neutral"); // 设置为中性表情
        display->SetChatMessage("system", ""); // 清空系统消息
    }
}

// 播放声音
void Application::PlaySound(const std::string_view& sound) {
    // 等待前一个声音播放完成
    /**
    这段代码中的花括号 { } 创建了一个作用域块（scope block），而不是用于变量声明。这是 C++ 中一个重要的编程技巧，主要有以下作用：
    当代码块结束时，std::unique_lock 会自动调用析构函数
    析构函数会自动解锁互斥量
    不需要手动解锁，避免忘记解锁导致死锁
    限制变量作用域：
    lock 变量只在花括号内有效
    离开花括号后，lock 自动销毁
    防止锁被意外延长使用
    RAII（资源获取即初始化）原则：
    利用 C++ 的自动析构机制
    确保资源在使用完后被正确释放
    提高代码的健壮性

     */
    {
        std::unique_lock<std::mutex> lock(mutex_); // 加锁，保护音频解码队列
        audio_decode_cv_.wait(lock, [this]() { // 等待队列为空
            return audio_decode_queue_.empty();
        });
    }
    background_task_->WaitForCompletion(); // 等待后台任务完成

    // 设置解码参数（16000Hz采样率，60ms帧长）
    SetDecodeSampleRate(16000, 60); // 设置解码采样率和帧长，引用自本类方法
    const char* data = sound.data(); // 获取声音数据指针
    size_t size = sound.size(); // 获取声音数据长度
    for (const char* p = data; p < data + size; ) { // 遍历所有音频包
        auto p3 = (BinaryProtocol3*)p; // 强制转换为协议包结构体，引用自 BinaryProtocol3
        p += sizeof(BinaryProtocol3); // 指针后移到 payload

        // 解析音频数据包
        auto payload_size = ntohs(p3->payload_size); // 获取音频包长度，引用自 arpa/inet.h
        AudioStreamPacket packet; // 创建音频流包结构体，引用自 AudioStreamPacket
        packet.payload.resize(payload_size); // 分配 payload 空间
        memcpy(packet.payload.data(), p3->payload, payload_size); // 拷贝音频数据
        p += payload_size; // 指针后移到下一个包

        // 将音频包加入解码队列
        std::lock_guard<std::mutex> lock(mutex_); // 加锁
        audio_decode_queue_.emplace_back(std::move(packet)); // 入队
    }
}

// 切换聊天状态
void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) { // 如果处于激活中，切换为空闲
        SetDeviceState(kDeviceStateIdle); // 设置为空闲状态
        return;
    }

    if (!protocol_) { // 协议未初始化
        ESP_LOGE(TAG, "Protocol not initialized"); // 输出错误日志
        return;
    }

    // 根据当前状态执行相应操作
    if (device_state_ == kDeviceStateIdle) { // 空闲状态，准备连接
        Schedule([this]() { // 异步调度任务，引用自本类 Schedule 方法
            SetDeviceState(kDeviceStateConnecting); // 设置为连接中
            if (!protocol_->OpenAudioChannel()) { // 打开音频通道，引用自协议类
                return;
            }

            SetListeningMode(realtime_chat_enabled_ ? kListeningModeRealtime : kListeningModeAutoStop); // 设置监听模式
        });
    } else if (device_state_ == kDeviceStateSpeaking) { // 说话状态，准备中止
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone); // 中止说话，引用自本类方法
        });
    } else if (device_state_ == kDeviceStateListening) { // 监听状态，准备关闭音频通道
        Schedule([this]() {
            protocol_->CloseAudioChannel(); // 关闭音频通道
        });
    }
}

// 开始监听
void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) { // 如果设备处于激活中，切换为空闲
        SetDeviceState(kDeviceStateIdle); // 设置为空闲状态
        return;
    }

    if (!protocol_) { // 协议未初始化
        ESP_LOGE(TAG, "Protocol not initialized"); // 输出错误日志
        return;
    }
    
    // 根据当前状态执行相应操作
    if (device_state_ == kDeviceStateIdle) { // 空闲状态，准备连接
        Schedule([this]() { // 异步调度任务
            if (!protocol_->IsAudioChannelOpened()) { // 如果音频通道未打开
                SetDeviceState(kDeviceStateConnecting); // 设置为连接中
                if (!protocol_->OpenAudioChannel()) { // 打开音频通道
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop); // 设置监听模式为手动停止
        });
    } else if (device_state_ == kDeviceStateSpeaking) { // 说话状态，准备中止并切换监听
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone); // 中止说话
            SetListeningMode(kListeningModeManualStop); // 切换监听模式
        });
    }
}

// 停止监听
void Application::StopListening() {
    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // 如果当前状态无效，直接返回
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) { // 仅监听状态下可停止
            protocol_->SendStopListening(); // 发送停止监听命令
            SetDeviceState(kDeviceStateIdle); // 切换为空闲状态
        }
    });
}

// 应用程序启动函数
void Application::Start() {
    auto& board = Board::GetInstance(); // 获取 Board 单例
    SetDeviceState(kDeviceStateStarting); // 设置设备状态为启动中

    /* 初始化显示 */
    auto display = board.GetDisplay(); // 获取显示屏对象

    /* 初始化音频编解码器 */
    auto codec = board.GetAudioCodec(); // 获取音频编解码器对象
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
#else
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS); // 创建 Opus 解码器，引用自 OpusDecoderWrapper
#endif

#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
#else
    // 配置Opus编码器
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS); // 创建 Opus 编码器，引用自 OpusEncoderWrapper
    if (realtime_chat_enabled_) {
        ESP_LOGI(TAG, "Realtime chat enabled, setting opus encoder complexity to 0"); // 实时聊天，编码复杂度设为0
        opus_encoder_->SetComplexity(0); // 设置编码复杂度
    } else if (board.GetBoardType() == "ml307") {
        ESP_LOGI(TAG, "ML307 board detected, setting opus encoder complexity to 5"); // ML307 板，复杂度设为5
        opus_encoder_->SetComplexity(5);
    } else {
        ESP_LOGI(TAG, "WiFi board detected, setting opus encoder complexity to 3"); // WiFi 板，复杂度设为3
        opus_encoder_->SetComplexity(3);
    }
#endif

    // 配置重采样器
    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000); // 配置输入重采样器
        reference_resampler_.Configure(codec->input_sample_rate(), 16000); // 配置参考通道重采样器
    }
    codec->Start(); // 启动音频编解码器

    // 创建音频处理任务
#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop(); // 启动音频循环
        vTaskDelete(NULL);
    }, "audio_loop", CONFIG_AUDIO_LOOP_TASK_STACK_SIZE, this, 8, &audio_loop_task_handle_, 1); // 在指定核上创建任务
#else
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop(); // 启动音频循环
        vTaskDelete(NULL);
    }, "audio_loop", CONFIG_AUDIO_LOOP_TASK_STACK_SIZE, this, 8, &audio_loop_task_handle_); // 创建任务
#endif

    /* 启动网络 */
    board.StartNetwork(); // 启动网络，引用自 Board

    // 检查新版本
    CheckNewVersion(); // 检查 OTA 升级

    // 初始化协议
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL); // 显示"加载协议"

    // 根据OTA配置选择协议
    if (ota_.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>(); // 使用 MQTT 协议
    } else if (ota_.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>(); // 使用 WebSocket 协议
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT"); // 未指定协议，默认 MQTT
        protocol_ = std::make_unique<MqttProtocol>();
    }

    // 设置协议回调函数
    protocol_->OnNetworkError([this](const std::string& message) {
        SetDeviceState(kDeviceStateIdle); // 网络错误，切换为空闲
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION); // 弹出错误提示
    });

    // 处理接收到的音频数据
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) {
        const int max_packets_in_queue = 600 / OPUS_FRAME_DURATION_MS; // 队列最大包数
        /**
        std::lock_guard：
        这是 C++11 引入的 RAII 风格的互斥锁包装器
        用于自动管理互斥锁的生命周期
        在构造时自动加锁，在析构时自动解锁
        模板参数 <std::mutex>：
        指定要使用的互斥锁类型
        std::mutex 是标准库中的互斥锁类
        用于保护共享资源不被多个线程同时访问
        lock(mutex_)：
        lock 是变量名
        mutex_ 是类的成员变量，类型为 std::mutex
        构造函数会立即对 mutex_ 加锁 */
        std::lock_guard<std::mutex> lock(mutex_); // 加锁
        if (audio_decode_queue_.size() < max_packets_in_queue) {
            audio_decode_queue_.emplace_back(std::move(packet)); // 入队
        }
    });

    // 音频通道打开时的处理
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false); // 关闭省电模式
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate()); // 采样率不一致，可能失真
        }
        SetDecodeSampleRate(protocol_->server_sample_rate(), protocol_->server_frame_duration()); // 设置解码采样率
        auto& thing_manager = iot::ThingManager::GetInstance(); // 获取物联网管理器
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson()); // 发送 IoT 描述符
        std::string states;
        if (thing_manager.GetStatesJson(states, false)) {
            protocol_->SendIotStates(states); // 发送 IoT 状态
        }
    });

    // 音频通道关闭时的处理
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true); // 开启省电模式
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay(); // 获取显示屏对象
            display->SetChatMessage("system", ""); // 清空系统消息
            SetDeviceState(kDeviceStateIdle); // 切换为空闲
        });
    });

    // 处理接收到的JSON消息
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // 解析JSON数据
        auto type = cJSON_GetObjectItem(root, "type"); // 获取 type 字段
        if (strcmp(type->valuestring, "tts") == 0) { // TTS 消息
            // 处理TTS（文本转语音）消息
            auto state = cJSON_GetObjectItem(root, "state"); // 获取 state 字段
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false; // 标记未中止
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking); // 切换为说话状态
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    background_task_->WaitForCompletion(); // 等待后台任务完成
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle); // 手动停止，切换为空闲
                        } else {
                            SetDeviceState(kDeviceStateListening); // 否则切换为监听
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text"); // 获取文本内容
                if (text != NULL) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring); // 输出日志
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str()); // 显示助手消息
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) { // STT 消息
            // 处理STT（语音转文本）消息
            auto text = cJSON_GetObjectItem(root, "text"); // 获取文本内容
            if (text != NULL) {
                ESP_LOGI(TAG, ">> %s", text->valuestring); // 输出日志
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str()); // 显示用户消息
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) { // LLM 消息
            // 处理LLM（语言模型）消息
            auto emotion = cJSON_GetObjectItem(root, "emotion"); // 获取情感字段
            if (emotion != NULL) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str()); // 设置表情
                });
            }
        } else if (strcmp(type->valuestring, "iot") == 0) { // IoT 消息
            // 处理IoT（物联网）消息
            auto commands = cJSON_GetObjectItem(root, "commands"); // 获取命令数组
            if (commands != NULL) {
                auto& thing_manager = iot::ThingManager::GetInstance(); // 获取物联网管理器
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i); // 获取每个命令
                    thing_manager.Invoke(command); // 执行命令
                }
            }
        } else if (strcmp(type->valuestring, "system") == 0) { // 系统消息
            // 处理系统消息
            auto command = cJSON_GetObjectItem(root, "command"); // 获取命令字段
            if (command != NULL) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring); // 输出日志
                if (strcmp(command->valuestring, "reboot") == 0) {
                    Schedule([this]() {
                        Reboot(); // 重启设备
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring); // 未知命令
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) { // 警告消息
            // 处理警告消息
            auto status = cJSON_GetObjectItem(root, "status"); // 获取状态
            auto message = cJSON_GetObjectItem(root, "message"); // 获取消息
            auto emotion = cJSON_GetObjectItem(root, "emotion"); // 获取情感
            if (status != NULL && message != NULL && emotion != NULL) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION); // 弹出警告
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion"); // 参数不足
            }
        }
    });

    // 启动协议
    bool protocol_started = protocol_->Start(); // 启动协议

    // 初始化音频处理器
    audio_processor_->Initialize(codec); // 初始化音频处理器
#ifndef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            if (protocol_->IsAudioChannelBusy()) {
                return;
            }
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                AudioStreamPacket packet;
                packet.payload = std::move(opus);
                packet.timestamp = last_output_timestamp_;
                last_output_timestamp_ = 0;
                Schedule([this, packet = std::move(packet)]() {
                    protocol_->SendAudio(packet); // 发送音频数据
                });
            });
        });
    });
#endif

    // 设置VAD状态变化回调
    audio_processor_->OnVadStateChange([this](bool speaking) {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true; // 检测到语音
                } else {
                    voice_detected_ = false; // 未检测到语音
                }
                auto led = Board::GetInstance().GetLed(); // 获取 LED 对象
                led->OnStateChanged(); // 更新 LED 状态
            });
        }
    });

#if CONFIG_USE_WAKE_WORD_DETECT
    // 初始化唤醒词检测
    wake_word_detect_.Initialize(codec); // 初始化唤醒词检测器
    wake_word_detect_.OnWakeWordDetected([this](const std::string& wake_word) {
        Schedule([this, &wake_word]() {
            if (device_state_ == kDeviceStateIdle) {
                SetDeviceState(kDeviceStateConnecting); // 切换为连接中
                wake_word_detect_.EncodeWakeWordData(); // 编码唤醒词数据

                if (!protocol_ || !protocol_->OpenAudioChannel()) {
                    wake_word_detect_.StartDetection(); // 重新开始检测
                    return;
                }
                
                // 发送唤醒词数据
                AudioStreamPacket packet;
                while (wake_word_detect_.GetWakeWordOpus(packet.payload)) {
                    protocol_->SendAudio(packet); // 发送音频包
                }
                protocol_->SendWakeWordDetected(wake_word); // 通知服务器唤醒词已检测
                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str()); // 输出日志
                SetListeningMode(realtime_chat_enabled_ ? kListeningModeRealtime : kListeningModeAutoStop); // 设置监听模式
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected); // 说话时检测到唤醒词，中止说话
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle); // 激活中检测到唤醒词，切换为空闲
            }
        });
    });
    wake_word_detect_.StartDetection(); // 启动唤醒词检测
#endif

    // 等待版本检查完成
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY); // 等待事件位
    SetDeviceState(kDeviceStateIdle); // 切换为空闲

    if (protocol_started) {
        // 显示版本信息
        std::string message = std::string(Lang::Strings::VERSION) + ota_.GetCurrentVersion(); // 拼接版本信息
        display->ShowNotification(message.c_str()); // 显示通知
        display->SetChatMessage("system", ""); // 清空系统消息
        // 播放启动成功提示音
        ResetDecoder(); // 重置解码器
        PlaySound(Lang::Sounds::P3_SUCCESS); // 播放成功音效
    }
    
    // 进入主事件循环
    MainEventLoop(); // 启动主事件循环
}

// 时钟定时器回调函数
void Application::OnClockTimer() {
    clock_ticks_++;

    // 每10秒打印一次调试信息
    if (clock_ticks_ % 10 == 0) {
        // SystemInfo::PrintRealTimeStats(pdMS_TO_TICKS(1000));

        // 打印内存使用情况
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);

#if 0
        char pcWriteBuffer[1024];
        // 生成任务列表信息到缓冲区
        vTaskList(pcWriteBuffer);
        // 打印任务列表信息
        printf("Task List:\n%s\n", pcWriteBuffer);
#endif

        // 如果已同步服务器时间，在空闲状态下显示时钟
        if (ota_.HasServerTime()) {
            if (device_state_ == kDeviceStateIdle) {
                Schedule([this]() {
                    // 设置状态为时钟格式 "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str);
                });
            }
        }
    }
}

// 添加异步任务到主循环
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// 主事件循环：控制聊天状态和WebSocket连接
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SCHEDULE_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            std::list<std::function<void()>> tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

// 音频循环：处理音频输入输出
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    while (true) {
        OnAudioInput();
        if (codec->output_enabled()) {
            OnAudioOutput();
        }
#if CONFIG_FREERTOS_HZ == 1000
        vTaskDelay(pdMS_TO_TICKS(10));
#endif
    }
}

// 处理音频输出
void Application::OnAudioOutput() {
    if (busy_decoding_audio_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // 如果长时间没有音频数据，禁用输出
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    // 在监听状态下清空音频队列
    if (device_state_ == kDeviceStateListening) {
        audio_decode_queue_.clear();
        audio_decode_cv_.notify_all();
        return;
    }

    // 获取并处理音频包
    auto packet = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();
    audio_decode_cv_.notify_all();

    // 检查内存状态
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if(free_sram < 10000){
        return;
    }

    // 解码音频数据
    busy_decoding_audio_ = true;
    background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        busy_decoding_audio_ = false;
        if (aborted_) {
            return;
        }
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
        WriteAudio(packet.payload);
#else
        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) {
            return;
        }
        WriteAudio(pcm, opus_decoder_->sample_rate());
#endif
        last_output_timestamp_ = packet.timestamp;
        last_output_time_ = std::chrono::steady_clock::now();
    });
}

// 处理音频输入
void Application::OnAudioInput() {
#if CONFIG_USE_WAKE_WORD_DETECT
    // 处理唤醒词检测
    if (wake_word_detect_.IsDetectionRunning()) {
        std::vector<int16_t> data;
        int samples = wake_word_detect_.GetFeedSize();
        if (samples > 0) {
            ReadAudio(data, 16000, samples);
            wake_word_detect_.Feed(data);
            return;
        }
    }
#endif
    // 处理音频处理器
    if (audio_processor_->IsRunning()) {
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
        // 检查内存状态
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if(free_sram < 10000){
            return;
        }
        // 读取并发送音频数据
        std::vector<uint8_t> opus;
        if (!protocol_->IsAudioChannelBusy()) {
            ReadAudio(opus, 16000, 30 * 16000 / 1000);
            AudioStreamPacket packet;
            packet.payload = std::move(opus);
            packet.timestamp = last_output_timestamp_;
            last_output_timestamp_ = 0;
            Schedule([this, packet = std::move(packet)]() {
                protocol_->SendAudio(packet);
            });
        }
#else
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize();
        if (samples > 0) {
            ReadAudio(data, 16000, samples);
            audio_processor_->Feed(data);
            return;
        }
#endif
    }
       
#if CONFIG_FREERTOS_HZ != 1000
    vTaskDelay(pdMS_TO_TICKS(30));
#endif
}

// 读取音频数据
void Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec->input_sample_rate() != sample_rate) {
        // 需要重采样
        data.resize(samples * codec->input_sample_rate() / sample_rate);
        if (!codec->InputData(data)) {
            return;
        }
        if (codec->input_channels() == 2) {
            // 处理双通道音频
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            // 重采样麦克风通道
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            // 重采样单通道音频
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        // 直接读取音频数据
        data.resize(samples);
        if (!codec->InputData(data)) {
            return;
        }
    }
}

#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
// 读取Opus编码的音频数据
void Application::ReadAudio(std::vector<uint8_t>& opus, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    opus.resize(samples);
    if (!codec->InputData(opus)) {
        return;
    }
}
#endif

// 写入音频数据
void Application::WriteAudio(std::vector<int16_t>& data, int sample_rate) {
    auto codec = Board::GetInstance().GetAudioCodec(); // 获取音频编解码器实例
    // 如果需要重采样
    if (sample_rate != codec->output_sample_rate()) { // 如果采样率不匹配
        int target_size = output_resampler_.GetOutputSamples(data.size()); // 计算重采样后的数据大小
        std::vector<int16_t> resampled(target_size); // 创建重采样缓冲区
        output_resampler_.Process(data.data(), data.size(), resampled.data()); // 执行重采样
        data = std::move(resampled); // 移动重采样后的数据
    }
    codec->OutputData(data); // 输出音频数据
}

#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
// 写入Opus编码的音频数据
void Application::WriteAudio(std::vector<uint8_t>& opus) {
    auto codec = Board::GetInstance().GetAudioCodec(); // 获取音频编解码器实例
    codec->OutputData(opus); // 直接输出Opus编码数据
}
#endif

// 中止语音播放
void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking"); // 输出日志
    aborted_ = true; // 设置中止标志
    protocol_->SendAbortSpeaking(reason); // 发送中止命令
}

// 设置监听模式
void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode; // 设置监听模式
    SetDeviceState(kDeviceStateListening); // 切换到监听状态
}

// 设置设备状态
void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) { // 如果状态未改变
        return;
    }
    
    clock_ticks_ = 0; // 重置时钟计数
    auto previous_state = device_state_; // 保存之前的状态
    device_state_ = state; // 更新状态
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]); // 输出状态日志
    // 等待所有后台任务完成
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance(); // 获取板级实例
    auto display = board.GetDisplay(); // 获取显示对象
    auto led = board.GetLed(); // 获取LED对象
    led->OnStateChanged(); // 更新LED状态
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // 空闲状态
            display->SetStatus(Lang::Strings::STANDBY); // 设置待机状态
            display->SetEmotion("neutral"); // 设置中性表情
            audio_processor_->Stop(); // 停止音频处理器
#if CONFIG_USE_WAKE_WORD_DETECT
            wake_word_detect_.StartDetection(); // 启动唤醒词检测
#endif
            break;
        case kDeviceStateConnecting:
            // 连接状态
            display->SetStatus(Lang::Strings::CONNECTING); // 设置连接状态
            display->SetEmotion("neutral"); // 设置中性表情
            display->SetChatMessage("system", ""); // 清空系统消息
            break;
        case kDeviceStateListening:
            // 监听状态
            display->SetStatus(Lang::Strings::LISTENING); // 设置监听状态
            display->SetEmotion("neutral"); // 设置中性表情

            // 更新IoT状态
            UpdateIotStates(); // 更新物联网设备状态

            // 确保音频处理器运行
            if (!audio_processor_->IsRunning()) { // 如果音频处理器未运行
                // 发送开始监听命令
                protocol_->SendStartListening(listening_mode_); // 发送开始监听命令
                if (listening_mode_ == kListeningModeAutoStop && previous_state == kDeviceStateSpeaking) {
                    // FIXME: 等待扬声器清空缓冲区
                    vTaskDelay(pdMS_TO_TICKS(120)); // 延迟120ms等待缓冲区清空
                }
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
#else
                opus_encoder_->ResetState(); // 重置Opus编码器状态
#endif
#if CONFIG_USE_WAKE_WORD_DETECT
                wake_word_detect_.StopDetection(); // 停止唤醒词检测
#endif
                audio_processor_->Start(); // 启动音频处理器
            }
            break;
        case kDeviceStateSpeaking:
            // 说话状态
            display->SetStatus(Lang::Strings::SPEAKING); // 设置说话状态

            if (listening_mode_ != kListeningModeRealtime) {
                audio_processor_->Stop();
#if CONFIG_USE_WAKE_WORD_DETECT
                wake_word_detect_.StartDetection();
#endif
            }
            ResetDecoder();
            break;
        default:
            // 其他状态不做处理
            break;
    }
}

// 重置解码器
void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_); // 加锁保护解码器状态
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
#else
    opus_decoder_->ResetState(); // 重置Opus解码器状态
#endif
    audio_decode_queue_.clear(); // 清空音频解码队列
    audio_decode_cv_.notify_all(); // 通知所有等待的线程
    last_output_time_ = std::chrono::steady_clock::now(); // 更新最后输出时间
    
    auto codec = Board::GetInstance().GetAudioCodec(); // 获取音频编解码器实例
    codec->EnableOutput(true); // 启用音频输出
}

// 设置解码采样率
void Application::SetDecodeSampleRate(int sample_rate, int frame_duration) {
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS // 如果使用Opus解码
    auto codec = Board::GetInstance().GetAudioCodec(); // 获取音频编解码器实例
    codec->ConfigDecode(sample_rate, 1, frame_duration); // 配置解码参数
#else
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) { // 如果参数未改变
        return;
    }

    // 配置解码器
    opus_decoder_->Config(sample_rate, 1, frame_duration); // 配置Opus解码器

    auto codec = Board::GetInstance().GetAudioCodec(); // 获取音频编解码器实例
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) { // 如果采样率不匹配
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate()); // 输出重采样日志
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate()); // 配置输出重采样器
    }
#endif
}

// 更新IoT状态
void Application::UpdateIotStates() {
    auto& thing_manager = iot::ThingManager::GetInstance(); // 获取物联网设备管理器实例
    std::string states; // 状态JSON字符串
    if (thing_manager.GetStatesJson(states, true)) { // 获取设备状态JSON
        protocol_->SendIotStates(states); // 发送IoT状态到服务器
    }
}

// 重启设备
void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting..."); // 输出重启日志
    esp_restart(); // 重启ESP32
}

// 唤醒词触发
void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) { // 如果设备处于空闲状态
        ToggleChatState(); // 切换聊天状态
        Schedule([this, wake_word]() { // 调度任务
            if (protocol_) { // 如果协议已初始化
                protocol_->SendWakeWordDetected(wake_word); // 发送唤醒词检测通知
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) { // 如果设备正在说话
        Schedule([this]() { // 调度任务
            AbortSpeaking(kAbortReasonNone); // 中止说话
        });
    } else if (device_state_ == kDeviceStateListening) { // 如果设备正在监听
        Schedule([this]() { // 调度任务
            if (protocol_) { // 如果协议已初始化
                protocol_->CloseAudioChannel(); // 关闭音频通道
            }
        });
    }
}

// 检查是否可以进入睡眠模式
bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) { // 如果设备不在空闲状态
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) { // 如果音频通道已打开
        return false;
    }

    // 可以安全进入睡眠模式
    return true;
}

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
// 释放解码器资源
void Application::ReleaseDecoder() {
    ESP_LOGW(TAG, "Release decoder"); // 输出释放解码器日志
    while (!audio_decode_queue_.empty()) // 等待音频解码队列清空
    {  
        vTaskDelay(pdMS_TO_TICKS(200)); // 每200ms检查一次
    }
    std::lock_guard<std::mutex> lock(mutex_); // 加锁保护资源
    vTaskDelete(audio_loop_task_handle_); // 删除音频循环任务
    audio_loop_task_handle_ = nullptr; // 指针置空
    background_task_->WaitForCompletion(); // 等待后台任务完成
    background_task_->WaitForCompletion(); // 再次等待，确保彻底完成
    delete background_task_; // 删除后台任务对象
    background_task_ = nullptr; // 指针置空
    opus_decoder_.reset(); // 释放Opus解码器
    ESP_LOGW(TAG, "Decoder released DONE"); // 输出释放完成日志
}

// 显示OTA信息
void Application::ShowOtaInfo(const std::string& code,const std::string& ip) {
    Schedule([this]() {
        if(device_state_ != kDeviceStateActivating && device_state_ != kDeviceStateIdle && protocol_ != nullptr) {
            protocol_->CloseAudioChannel(); // 非激活/空闲状态下关闭音频通道
        }
    });
    vTaskDelay(pdMS_TO_TICKS(600)); // 延迟600ms，等待状态切换
    if (device_state_ != kDeviceStateIdle) { // 如果设备不在空闲状态
        ESP_LOGW(TAG, "ShowOtaInfo, device_state_:%s != kDeviceStateIdle", STATE_STRINGS[device_state_]); // 输出警告日志
        background_task_->Schedule([this, code, ip](){
            this->ShowOtaInfo(code, ip); // 递归重试显示OTA信息
        });
        return;
    }
    if(protocol_ != nullptr) {    // 如果协议对象还存在
        Schedule([this]() {
            protocol_.reset(); // 释放协议对象
            protocol_ = nullptr;
            
        });
        vTaskDelay(pdMS_TO_TICKS(100)); // 延迟100ms，等待协议释放
        background_task_->Schedule([this, code, ip](){
            this->ShowOtaInfo(code, ip); // 递归重试显示OTA信息
        });
        return;
    }
    
    ResetDecoder(); // 重置解码器
    ESP_LOGW(TAG,"DEV CODE:%s ip:%s", code.c_str(), ip.c_str()); // 输出设备码和IP
    struct digit_sound {
        char digit; // 数字字符
        const std::string_view& sound; // 对应的语音资源
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    Schedule([this,code,ip](){
        auto display = Board::GetInstance().GetDisplay(); // 获取显示屏对象
        std::string message;
        if (ip.empty()) {
            message = "浏览器访问\nhttp://vbota.esp32.cn/vbota\n设备码:"+code; // 无IP时只显示云端地址
        } else {
            message = "浏览器访问\nhttp://vbota.esp32.cn/vbota\n或\nhttp://"+ip+"\n设备码:"+code; // 有IP时显示本地和云端地址
        }
        
        display->SetStatus("升级模式"); // 设置显示屏为升级模式
        display->SetChatMessage("system", message.c_str()); // 显示OTA信息
        PlaySound(Lang::Sounds::P3_START_OTA); // 播放OTA开始提示音
        for (const auto& digit : code) { // 逐个播放设备码数字语音
            auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                [digit](const digit_sound& ds) { return ds.digit == digit; });
            if (it != digit_sounds.end()) {
                PlaySound(it->sound); // 播放对应数字语音
            }
        }
    });
}
#endif
