// Minimal Arduino shim so the UI can be rendered on a desktop for previews.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <arpa/inet.h>
using std::min; using std::max;
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define IRAM_ATTR
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0
uint32_t millis();
void delay(uint32_t);
uint32_t esp_random();
uint32_t analogReadMilliVolts(int);
inline void pinMode(int, int) {}
inline int digitalRead(int) { return 1; }
#define ADC_11db 3
inline void analogSetPinAttenuation(int, int) {}
struct SerialShim { void printf(const char* f, ...) { va_list a; va_start(a, f); vfprintf(stderr, f, a); va_end(a);} void println(const char* s){fprintf(stderr,"%s\n",s);} void begin(int){} };
extern SerialShim Serial;
struct EspShim { uint32_t getFreeHeap() { return 187000; } };
extern EspShim ESP;
