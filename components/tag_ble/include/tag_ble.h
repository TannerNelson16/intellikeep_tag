#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "tag_config.h"

typedef enum {
    TAG_REASON_PERIODIC = 0,
    TAG_REASON_TAMPER = 1,
    TAG_REASON_NFC = 2,
    TAG_REASON_ALERT = 3,
} tag_reason_t;

esp_err_t tag_ble_init(const tag_config_t *cfg);
esp_err_t tag_ble_start(uint8_t battery_pct, bool tamper, bool battery_fresh, tag_reason_t reason);
void tag_ble_stop(void);
void tag_ble_shutdown(void);
void tag_ble_set_fast_mode(bool enabled);
void tag_ble_update_battery(uint8_t pct);
