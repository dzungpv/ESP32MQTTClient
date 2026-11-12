#pragma once
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int g_log_verbose;
static inline void esp_log_stub(const char *lvl, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static inline void esp_log_stub(const char *lvl, const char *tag, const char *fmt, ...)
{ if (!g_log_verbose) return; va_list ap; va_start(ap, fmt); fprintf(stderr, "    %s (%s) ", lvl, tag); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); }
#define ESP_LOGE(tag, fmt, ...) esp_log_stub("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_stub("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_stub("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) esp_log_stub("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) esp_log_stub("V", tag, fmt, ##__VA_ARGS__)
#ifdef __cplusplus
}
#endif
