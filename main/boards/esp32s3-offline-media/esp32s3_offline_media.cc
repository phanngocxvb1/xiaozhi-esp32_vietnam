#include <algorithm>
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#include "sdspi.h"
#ifdef CONFIG_OFFLINE_MODE_ENABLE
#include "features/offline/offline_mode.h"
#endif

#define TAG "Esp32s3OfflineMedia"

static bool OfflineActive() {
#ifdef CONFIG_OFFLINE_MODE_ENABLE
    return OfflineMode::GetInstance().IsActive();
#else
    return false;
#endif
}

class Esp32s3OfflineMedia : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button touch_button_;
    LcdDisplay* display_ = nullptr;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;   // NC: panel CS is tied low
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        int volume = std::clamp(codec->output_volume() + delta, 0, 100);
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();   // ignored while offline mode is active
        });

#ifdef CONFIG_OFFLINE_MODE_ENABLE
        // Vol+ hold  -> toggle offline mode
        volume_up_button_.OnLongPress([]() { OfflineMode::GetInstance().Toggle(); });
#endif

        // Vol+ / Vol-: next / previous in offline mode, volume in online mode
        volume_up_button_.OnClick([this]() {
#ifdef CONFIG_OFFLINE_MODE_ENABLE
            if (OfflineActive()) { OfflineMode::GetInstance().OnNext(); return; }
#endif
            ChangeVolume(+10);
        });
        volume_down_button_.OnClick([this]() {
#ifdef CONFIG_OFFLINE_MODE_ENABLE
            if (OfflineActive()) { OfflineMode::GetInstance().OnPrev(); return; }
#endif
            ChangeVolume(-10);
        });
        volume_down_button_.OnLongPress([this]() {
            if (OfflineActive()) return;
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        // Touch pad
        touch_button_.OnClick([this]() {
#ifdef CONFIG_OFFLINE_MODE_ENABLE
            if (OfflineActive()) { OfflineMode::GetInstance().OnTouchClick(); return; }
#endif
            Application::GetInstance().ToggleChatState();
        });
#ifdef CONFIG_OFFLINE_MODE_ENABLE
        touch_button_.OnDoubleClick([]() { OfflineMode::GetInstance().OnTouchDoubleClick(); });
        touch_button_.OnLongPress([]() { OfflineMode::GetInstance().OnTouchLongPress(); });
#endif
    }

public:
    Esp32s3OfflineMedia() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO, false, 1500),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO, TOUCH_BUTTON_ACTIVE_HIGH) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
#ifdef CONFIG_OFFLINE_MODE_ENABLE
        // Holding Vol+ while powering on boots straight into offline mode (no WiFi needed).
        OfflineMode::GetInstance().SetBootGpio(VOLUME_UP_BUTTON_GPIO);
#endif
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual SdCard* GetSdCard() override {
        static SdSPI sdspi(CARD_SPI_MISO_GPIO, CARD_SPI_MOSI_GPIO, CARD_SPI_SCLK_GPIO, CARD_SPI_CS_GPIO,
                           SPI2_HOST);
        return &sdspi;
    }
};

DECLARE_BOARD(Esp32s3OfflineMedia);
