/*
 * frank-quake2 — ESP_LOGx on the UART console.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef Q2_COMPAT_ESP_LOG_H
#define Q2_COMPAT_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)

#endif
