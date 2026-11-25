// Cyberpunk neon palette (RGB565)
#pragma once
#include <stdint.h>

namespace theme {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t BG = rgb(4, 4, 12);
constexpr uint16_t BG2 = rgb(10, 10, 24);
constexpr uint16_t SCAN = rgb(8, 9, 20);       // scanline tint
constexpr uint16_t GRID = rgb(20, 26, 52);
constexpr uint16_t PANEL = rgb(12, 14, 32);
constexpr uint16_t CYAN = rgb(0, 255, 230);
constexpr uint16_t CYAN_DIM = rgb(0, 110, 110);
constexpr uint16_t TEAL = rgb(0, 170, 150);
constexpr uint16_t MAGENTA = rgb(255, 0, 150);
constexpr uint16_t MAGENTA_DIM = rgb(110, 0, 70);
constexpr uint16_t YELLOW = rgb(250, 235, 0);
constexpr uint16_t ORANGE = rgb(255, 140, 0);
constexpr uint16_t RED = rgb(255, 40, 60);
constexpr uint16_t GREEN = rgb(57, 255, 20);
constexpr uint16_t GREEN_DIM = rgb(20, 110, 10);
constexpr uint16_t PURPLE = rgb(180, 70, 255);
constexpr uint16_t BLUE = rgb(60, 140, 255);
constexpr uint16_t BLUE_DIM = rgb(60, 90, 170);
constexpr uint16_t TEXT = rgb(210, 230, 255);
constexpr uint16_t DIM = rgb(90, 110, 150);
constexpr uint16_t DARK = rgb(40, 48, 80);
constexpr uint16_t BLACK = 0x0000;

}  // namespace theme
