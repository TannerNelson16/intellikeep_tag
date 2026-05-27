#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#include "tag_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tag_hw_init(const tag_config_t *cfg);
uint8_t tag_hw_read_battery_pct(void);
bool tag_hw_tamper_triggered(void);
gpio_num_t tag_hw_tamper_gpio(void);
bool tag_hw_nfc_field_detected(void);
gpio_num_t tag_hw_nfc_fd_gpio(void);
gpio_num_t tag_hw_nfc_power_gpio(void);
gpio_num_t tag_hw_nfc_sda_gpio(void);
gpio_num_t tag_hw_nfc_scl_gpio(void);
esp_err_t tag_hw_write_mac_ndef(void);
void tag_hw_prepare_for_sleep(void);

#ifdef __cplusplus
}
#endif
