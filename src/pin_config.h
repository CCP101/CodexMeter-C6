#pragma once

// Waveshare ESP32-C6-Touch-AMOLED-1.43 board wiring (Rev1.3).
// The board uses a 466x466 CO5300-compatible QSPI AMOLED and an FT6146 touch
// controller. This milestone enables the display and USB CDC transport only;
// touch is intentionally deferred.

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  11
#define LCD_CS    10
#define LCD_RST   3
#define LCD_WIDTH  466
#define LCD_HEIGHT 466
#define LCD_COLUMN_OFFSET 6

#define IIC_SDA 18
#define IIC_SCL 8

#define IO_EXPANDER_ADDRESS 0x20
#define LCD_ENABLE_EXIO 2
