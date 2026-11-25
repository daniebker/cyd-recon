// CYD//RECON - board configuration for Freenove ESP32-S3 Display 3.5" (FNK0104N)
// Pin map taken from Freenove's official FNK0104N sources
// (github.com/Freenove/Freenove_ESP32_S3_Display).
#pragma once
#include <Arduino.h>

// ---- Display: ST77922, 320x480 portrait native, driven over QSPI ----
#define PIN_LCD_CS      10
#define PIN_LCD_BL      41
#define PIN_QSPI_SCLK   12
#define PIN_QSPI_D0     11
#define PIN_QSPI_D1     13
#define PIN_QSPI_D2     14
#define PIN_QSPI_D3      9
#define LCD_NATIVE_W   320
#define LCD_NATIVE_H   480
#define LCD_QSPI_HZ    80000000

// Logical (landscape) screen we draw into
#define SCREEN_W 480
#define SCREEN_H 320

// ---- Touch: I2C controller @0x55 (16-bit register addresses) ----
#define PIN_I2C_SDA     38
#define PIN_I2C_SCL     39
#define PIN_TOUCH_RST   48
#define PIN_TOUCH_INT   47
#define TOUCH_I2C_ADDR  0x55

// ---- Audio: ES8311 codec on the same I2C bus, I2S for samples ----
#define PIN_I2S_MCLK    17
#define PIN_I2S_BCLK    18
#define PIN_I2S_DIN     16   // codec -> ESP (mic), unused
#define PIN_I2S_DOUT    15   // ESP -> codec (speaker)
#define PIN_I2S_WS      21
#define PIN_AMP_EN       1   // speaker amp enable, active LOW
#define ES8311_ADDR     0x18
#define AUDIO_RATE      16000

// ---- microSD (SDMMC 4-bit), used for .pcap capture ----
#define PIN_SD_CMD       4
#define PIN_SD_CLK       5
#define PIN_SD_D0        6
#define PIN_SD_D1        7
#define PIN_SD_D2        2
#define PIN_SD_D3        3

// ---- Misc ----
#define PIN_WS2812     40   // single WS2812
#define PIN_BAT_ADC      8   // 1:2 divider
#define PIN_BOOT_BTN     0

// ---- Firmware tuning ----
#define MAX_DEVICES        256   // tracked device table (lives in PSRAM)
#define DEVICE_EXPIRE_MS   180000 // drop devices not heard for 3 minutes
#define HOP_DWELL_MS       220   // WiFi channel hop dwell time
#define WIFI_MAX_CH        13
#define TARGET_FPS         25

#define FW_NAME    "CYD//RECON"
#define FW_VERSION "1.0.0"
