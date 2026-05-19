#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) do { fprintf(stderr, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
