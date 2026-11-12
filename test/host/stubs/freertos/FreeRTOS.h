#pragma once
// Host test stub of the FreeRTOS definitions used by the library.
#include <stdint.h>
typedef uint32_t TickType_t;
#ifndef TEST_TICK_PERIOD_MS
#define TEST_TICK_PERIOD_MS 10 // CONFIG_FREERTOS_HZ = 100
#endif
#define portTICK_PERIOD_MS TEST_TICK_PERIOD_MS
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / portTICK_PERIOD_MS))
