#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// Board: ESP32-S3 N16R8 (16MB flash, 8MB octal PSRAM -> GPIO35/36/37 are NOT usable)

// ---------------- Audio: INMP441 mic + MAX98357A amp (separate I2S ports) ----------------
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7   // MAX98357A DIN
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

// ---------------- Buttons ----------------
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_39   // active low (to GND)
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_40   // active low (to GND)
#define TOUCH_BUTTON_GPIO       GPIO_NUM_47   // capacitive touch pad module (TTP223 style)
#define TOUCH_BUTTON_ACTIVE_HIGH true         // set false if your module outputs LOW when touched
#define BUILTIN_LED_GPIO        GPIO_NUM_NC

// ---------------- Display: ST7789 SPI (no CS pin wired) ----------------
#define DISPLAY_SPI_HOST      SPI3_HOST
#define DISPLAY_MOSI_PIN      GPIO_NUM_41     // "SDA"
#define DISPLAY_CLK_PIN       GPIO_NUM_42     // "SCL"
#define DISPLAY_DC_PIN        GPIO_NUM_1
#define DISPLAY_RST_PIN       GPIO_NUM_2
#define DISPLAY_CS_PIN        GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_17
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define DISPLAY_WIDTH         240
#define DISPLAY_HEIGHT        320
#define DISPLAY_MIRROR_X      false
#define DISPLAY_MIRROR_Y      false
#define DISPLAY_SWAP_XY       false
#define DISPLAY_INVERT_COLOR  true            // typical for IPS ST7789; set false if colours look inverted
#define DISPLAY_RGB_ORDER     LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X      0
#define DISPLAY_OFFSET_Y      0
#define DISPLAY_SPI_MODE      0

// ---------------- SD card (SPI) ----------------
// NOTE: the CS pin was not in the pin list. GPIO21 is a placeholder - change it
// to the pin your SD module's CS is really wired to.
#define CARD_SPI_MISO_GPIO GPIO_NUM_12
#define CARD_SPI_MOSI_GPIO GPIO_NUM_13
#define CARD_SPI_SCLK_GPIO GPIO_NUM_14
#define CARD_SPI_CS_GPIO   GPIO_NUM_21

#endif // _BOARD_CONFIG_H_
