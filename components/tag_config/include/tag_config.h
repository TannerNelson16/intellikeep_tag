#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifndef TAG_BEHAVIOR_VARIANT_STANDARD
#define TAG_BEHAVIOR_VARIANT_STANDARD 1
#endif

#ifndef TAG_BEHAVIOR_VARIANT_DEMO
#define TAG_BEHAVIOR_VARIANT_DEMO 2
#endif

#ifndef TAG_BEHAVIOR_VARIANT
#define TAG_BEHAVIOR_VARIANT TAG_BEHAVIOR_VARIANT_STANDARD
#endif

#ifndef TAG_DEFAULT_WAKE_PERIOD_US
#define TAG_DEFAULT_WAKE_PERIOD_US (300ULL * 1000ULL * 1000ULL)
#endif

#ifndef TAG_DEFAULT_ADV_WINDOW_MS
#define TAG_DEFAULT_ADV_WINDOW_MS (245U)
#endif

#ifndef TAG_DEFAULT_NAME
#define TAG_DEFAULT_NAME "Tag"
#endif

#define TAG_TAMPER_GPIO_USE_DEFAULT UINT8_MAX

#define TAG_MIN_WAKE_PERIOD_US (1ULL * 1000ULL * 1000ULL)
#define TAG_MAX_WAKE_PERIOD_US (12ULL * 60ULL * 60ULL * 1000ULL * 1000ULL)

typedef struct {
    uint64_t wake_period_us;
    uint32_t adv_window_ms;
    uint8_t tamper_gpio;
    char name[24];
} tag_config_t;

esp_err_t tag_config_init(tag_config_t *cfg);
esp_err_t tag_config_save(const tag_config_t *cfg);
uint8_t tag_config_consume_warmup_slot(void);
