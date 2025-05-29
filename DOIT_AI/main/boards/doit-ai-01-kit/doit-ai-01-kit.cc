
#include "wifi_board.h"
#include "audio_codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
#include "mbedtls/md5.h"
#include <iomanip>
#include <sstream>
#include "system_info.h"
#include "vb6824.h"
#include "wifi_station.h"
#endif

#define TAG "CustomBoard"

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    VbAduioCodec audio_codec;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (audio_codec.InOtaMode(1) == true) {
                ESP_LOGI(TAG, "OTA mode, do not enter chat");
                return;
            }
            auto &app = Application::GetInstance();
            app.ToggleChatState();
        });
        boot_button_.OnPressRepeat([this](uint16_t count) {
            if(count >= 3){
                if (audio_codec.InOtaMode(1) == true) {
                    ESP_LOGI(TAG, "OTA mode, do not enter chat");
                    return;
                }
                ResetWifiConfiguration();
            }
        });
        boot_button_.OnLongPress([this]() {
            if (esp_timer_get_time() > 20 * 1000 * 1000) {
                ESP_LOGI(TAG, "Long press, do not enter OTA mode %ld", (uint32_t)esp_timer_get_time());
                return;
            }
           int ret = audio_codec.OtaStart(0); 
            if(ret == VbAduioCodec::OTA_ERR_NOT_SUPPORT){
                ESP_LOGW(TAG, "Please enable VB6824_OTA_SUPPORT");
            }
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }


public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO,false,3000), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO){          
        InitializeButtons();
        InitializeIot();
        audio_codec.OnWakeUp([this](const std::string& command) {
            if (command == std::string(vb6824_get_wakeup_word())){
                if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                    Application::GetInstance().WakeWordInvoke("你好小智");
                }
            }else if (command == "开始配网"){
                ResetWifiConfiguration();
            }
        });
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }
};

DECLARE_BOARD(CustomBoard);
