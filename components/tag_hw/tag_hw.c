#include "tag_hw.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#define TAG "tag_hw"

// Production PCB GPIO assignments
// Note: IO4 is bodged to the battery ADC input on this rev, so NFC I2C write support is disabled.
#if TAG_HW_VARIANT == TAG_HW_VARIANT_PRODUCTION
#define TAG_DEFAULT_TAMPER_GPIO GPIO_NUM_0   // Photodiode comparator output / deep-sleep wake source
#define TAG_NFC_FD_GPIO GPIO_NUM_5           // NTAG FD line, active-low wake pulse via external pull-up
#define TAG_NFC_POWER_GPIO GPIO_NUM_10       // Powers the NFC chip through a transistor
#define TAG_NFC_SDA_GPIO GPIO_NUM_NC         // IO4 is repurposed on this rev; NFC SDA unavailable
#define TAG_NFC_SCL_GPIO GPIO_NUM_6          // NFC I2C SCL
#define TAG_BATTERY_ENABLE_GPIO GPIO_NUM_18  // Enables the switched battery sense circuit
#else
// Dev board testing - use safe GPIOs that won't interfere
#define TAG_DEFAULT_TAMPER_GPIO GPIO_NUM_NC  // Disabled for dev board
#define TAG_NFC_FD_GPIO GPIO_NUM_NC          // Disabled for dev board
#define TAG_NFC_POWER_GPIO GPIO_NUM_NC       // Disabled for dev board
#define TAG_NFC_SDA_GPIO GPIO_NUM_NC         // Disabled for dev board
#define TAG_NFC_SCL_GPIO GPIO_NUM_NC         // Disabled for dev board
#define TAG_BATTERY_ENABLE_GPIO GPIO_NUM_NC  // Disabled for dev board
#endif

#define TAG_BATTERY_ADC_UNIT ADC_UNIT_1
#define TAG_BATTERY_ADC_CHANNEL ADC_CHANNEL_4  // GPIO4 battery sense input
#define TAG_BATTERY_ATTEN ADC_ATTEN_DB_12      // 12dB attenuation for battery sense
#define TAG_BATTERY_ENABLE_ACTIVE_LEVEL 1      // Divider enable is active-high on the production board
#define TAG_BATTERY_SETTLE_US 10000            // Allow the high-impedance divider to settle before sampling
#define TAG_BATTERY_FAST_SETTLE_US 20000       // Production battery read settle time
#define TAG_BATTERY_FAST_SAMPLES 2             // Production battery read sample count
#define TAG_BATTERY_DIAG_SETTLE_US 50000       // Extra settle time for characterization sweeps
#define TAG_BATTERY_DIAG_SAMPLES 8             // Number of samples to average per enable state

#define TAG_NFC_I2C_PORT I2C_NUM_0
#define TAG_NFC_I2C_ADDR 0x55
#define TAG_NFC_I2C_WRITE_ADDR ((TAG_NFC_I2C_ADDR << 1) | 0x00)
#define TAG_NFC_I2C_READ_ADDR ((TAG_NFC_I2C_ADDR << 1) | 0x01)
#define TAG_NFC_I2C_SPEED_HZ 100000
#define TAG_NFC_I2C_TIMEOUT_MS 250
#define TAG_NFC_I2C_PROBE_TIMEOUT_MS 100
#define TAG_NFC_SCL_WAIT_US 20000
#define TAG_NFC_POWER_SETTLE_US 10000
#define TAG_NFC_POWER_OFF_SETTLE_US 5000
#define TAG_NFC_EEPROM_WRITE_US 10000
#define TAG_NFC_BLOCK_SIZE 16
#define TAG_NFC_CC_BLOCK 0x00
#define TAG_NFC_USER_BLOCK 0x01
#define TAG_NFC_LANG_CODE "en"
#define TAG_NFC_DEFAULT_ADDR_BYTE 0xAA
#define TAG_NFC_POWER_ACTIVE_LEVEL 0
#define TAG_HW_NVS_NAMESPACE "tag_hw"

#ifndef TAG_BATTERY_PROFILE_CR2032
#define TAG_BATTERY_PROFILE_CR2032 1
#endif

#ifndef TAG_BATTERY_PROFILE_LIR2032
#define TAG_BATTERY_PROFILE_LIR2032 2
#endif

#ifndef TAG_BATTERY_PROFILE
#define TAG_BATTERY_PROFILE TAG_BATTERY_PROFILE_LIR2032
#endif

static const uint8_t k_nt3h2111_cc_bytes[4] = {
    0xE1, 0x10, 0x6D, 0x00,
};

static gpio_num_t s_tamper_gpio = TAG_DEFAULT_TAMPER_GPIO;
static gpio_num_t s_nfc_fd_gpio = TAG_NFC_FD_GPIO;
static gpio_num_t s_nfc_power_gpio = TAG_NFC_POWER_GPIO;
static gpio_num_t s_nfc_sda_gpio = TAG_NFC_SDA_GPIO;
static gpio_num_t s_nfc_scl_gpio = TAG_NFC_SCL_GPIO;
static gpio_num_t s_battery_enable_gpio = TAG_BATTERY_ENABLE_GPIO;
static adc_oneshot_unit_handle_t s_adc_unit;
static bool s_adc_ready;

static uint32_t estimate_battery_mv_from_raw(int raw) {
    // First-pass empirical fit from bench measurements on the production board:
    // raw ~= 1083 at 2.3V and raw ~= 1521 at 3.2V.
    int mv = (raw * 900 + 219) / 438 + 74;
    if (mv < 0) {
        mv = 0;
    }
    return (uint32_t)mv;
}

static const char *battery_profile_name(void) {
#if TAG_BATTERY_PROFILE == TAG_BATTERY_PROFILE_LIR2032
    return "LIR2032";
#else
    return "CR2032";
#endif
}

static uint8_t battery_pct_from_mv(uint32_t mv) {
#if TAG_BATTERY_PROFILE == TAG_BATTERY_PROFILE_LIR2032
    // Simple first-pass LIR2032 discharge curve under light load.
    if (mv >= 4150) return 100;
    if (mv >= 4050) return 95;
    if (mv >= 3950) return 90;
    if (mv >= 3900) return 80;
    if (mv >= 3850) return 70;
    if (mv >= 3800) return 60;
    if (mv >= 3750) return 50;
    if (mv >= 3700) return 40;
    if (mv >= 3650) return 30;
    if (mv >= 3550) return 20;
    if (mv >= 3450) return 10;
    if (mv >= 3300) return 5;
    return 0;
#else
    // First-pass CR2032-style curve under light load.
    if (mv >= 3000) return 100;
    if (mv >= 2950) return 90;
    if (mv >= 2900) return 80;
    if (mv >= 2850) return 70;
    if (mv >= 2800) return 60;
    if (mv >= 2750) return 50;
    if (mv >= 2700) return 40;
    if (mv >= 2650) return 30;
    if (mv >= 2550) return 20;
    if (mv >= 2450) return 10;
    if (mv >= 2300) return 5;
    return 0;
#endif
}

static bool read_adc_average(uint32_t settle_us, int samples, int *avg_raw) {
    if (!avg_raw || samples <= 0) {
        return false;
    }

    int64_t total = 0;
    for (int i = 0; i < samples; ++i) {
        if (settle_us > 0) {
            esp_rom_delay_us(settle_us);
        }
        int raw = 0;
        if (adc_oneshot_read(s_adc_unit, TAG_BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
            return false;
        }
        total += raw;
    }

    *avg_raw = (int)(total / samples);
    return true;
}

static esp_err_t init_adc(void) {
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = TAG_BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_unit), TAG, "adc unit create failed");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = TAG_BATTERY_ATTEN,
    };
    ESP_RETURN_ON_ERROR(
        adc_oneshot_config_channel(s_adc_unit, TAG_BATTERY_ADC_CHANNEL, &chan_cfg), TAG, "adc channel cfg failed");
    s_adc_ready = true;
    return ESP_OK;
}

static void configure_tamper_gpio(gpio_num_t gpio_num) {
    if (!GPIO_IS_VALID_GPIO(gpio_num)) {
        return;
    }
    // Photodiode comparator output: LOW when illuminated / tampered
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,  // Comparator has push-pull output
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure tamper GPIO %d", (int)gpio_num);
    }
}

static void set_nfc_power_level(int level, uint32_t settle_us) {
    if (!GPIO_IS_VALID_GPIO(s_nfc_power_gpio)) {
        return;
    }
    gpio_set_level(s_nfc_power_gpio, level ? 1 : 0);
    if (settle_us > 0) {
        esp_rom_delay_us(settle_us);
    }
}

static void set_nfc_power(bool enabled) {
    set_nfc_power_level(enabled ? TAG_NFC_POWER_ACTIVE_LEVEL : !TAG_NFC_POWER_ACTIVE_LEVEL,
                        enabled ? TAG_NFC_POWER_SETTLE_US : TAG_NFC_POWER_OFF_SETTLE_US);
}

static void nfc_prepare_bus_gpio_inputs(void) {
    if (GPIO_IS_VALID_GPIO(s_nfc_sda_gpio)) {
        gpio_config_t sda_conf = {
            .pin_bit_mask = 1ULL << s_nfc_sda_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&sda_conf);
    }
    if (GPIO_IS_VALID_GPIO(s_nfc_scl_gpio)) {
        gpio_config_t scl_conf = {
            .pin_bit_mask = 1ULL << s_nfc_scl_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&scl_conf);
    }
}

static void nfc_log_bus_levels(const char *stage, int power_level) {
    int sda_level = GPIO_IS_VALID_GPIO(s_nfc_sda_gpio) ? gpio_get_level(s_nfc_sda_gpio) : -1;
    int scl_level = GPIO_IS_VALID_GPIO(s_nfc_scl_gpio) ? gpio_get_level(s_nfc_scl_gpio) : -1;
    int fd_level = GPIO_IS_VALID_GPIO(s_nfc_fd_gpio) ? gpio_get_level(s_nfc_fd_gpio) : -1;
    ESP_LOGI(TAG,
             "NTAG %s: power_gpio=%d level=%d sda=%d scl=%d fd=%d",
             stage ? stage : "state",
             (int)s_nfc_power_gpio,
             power_level,
             sda_level,
             scl_level,
             fd_level);
}

static esp_err_t nfc_open_nvs(nvs_handle_t *handle) {
    return nvs_open(TAG_HW_NVS_NAMESPACE, NVS_READWRITE, handle);
}

static esp_err_t nfc_is_provisioned(bool *provisioned) {
    if (!provisioned) {
        return ESP_ERR_INVALID_ARG;
    }

    *provisioned = false;
    nvs_handle_t handle;
    esp_err_t err = nfc_open_nvs(&handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, "ntag_done", &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    *provisioned = (value != 0);
    return ESP_OK;
}

static esp_err_t nfc_mark_provisioned(void) {
    nvs_handle_t handle;
    esp_err_t err = nfc_open_nvs(&handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, "ntag_done", 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nfc_open_i2c(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *dev_handle) {
    if (!bus_handle || !dev_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_GPIO(s_nfc_sda_gpio) || !GPIO_IS_VALID_GPIO(s_nfc_scl_gpio)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    *bus_handle = NULL;
    *dev_handle = NULL;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = TAG_NFC_I2C_PORT,
        .sda_io_num = s_nfc_sda_gpio,
        .scl_io_num = s_nfc_scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, bus_handle);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_DEVICE_ADDRESS_NOT_USED,
        .scl_speed_hz = TAG_NFC_I2C_SPEED_HZ,
        .scl_wait_us = TAG_NFC_SCL_WAIT_US,
    };

    err = i2c_master_bus_add_device(*bus_handle, &dev_config, dev_handle);
    if (err != ESP_OK) {
        i2c_del_master_bus(*bus_handle);
        *bus_handle = NULL;
        return err;
    }

    return ESP_OK;
}

static void nfc_close_i2c(i2c_master_bus_handle_t bus_handle, i2c_master_dev_handle_t dev_handle) {
    if (dev_handle) {
        i2c_master_bus_rm_device(dev_handle);
    }
    if (bus_handle) {
        i2c_del_master_bus(bus_handle);
    }
}

static esp_err_t nfc_read_block(i2c_master_dev_handle_t dev_handle, uint8_t block_addr, uint8_t *data) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t write_addr = TAG_NFC_I2C_WRITE_ADDR;
    uint8_t read_addr = TAG_NFC_I2C_READ_ADDR;
    i2c_operation_job_t i2c_ops[] = {
        { .command = I2C_MASTER_CMD_START },
        {
            .command = I2C_MASTER_CMD_WRITE,
            .write = {
                .ack_check = true,
                .data = &write_addr,
                .total_bytes = 1,
            },
        },
        {
            .command = I2C_MASTER_CMD_WRITE,
            .write = {
                .ack_check = true,
                .data = &block_addr,
                .total_bytes = 1,
            },
        },
        { .command = I2C_MASTER_CMD_START },
        {
            .command = I2C_MASTER_CMD_WRITE,
            .write = {
                .ack_check = true,
                .data = &read_addr,
                .total_bytes = 1,
            },
        },
        {
            .command = I2C_MASTER_CMD_READ,
            .read = {
                .ack_value = I2C_ACK_VAL,
                .data = data,
                .total_bytes = TAG_NFC_BLOCK_SIZE - 1,
            },
        },
        {
            .command = I2C_MASTER_CMD_READ,
            .read = {
                .ack_value = I2C_NACK_VAL,
                .data = &data[TAG_NFC_BLOCK_SIZE - 1],
                .total_bytes = 1,
            },
        },
        { .command = I2C_MASTER_CMD_STOP },
    };

    return i2c_master_execute_defined_operations(
        dev_handle, i2c_ops, sizeof(i2c_ops) / sizeof(i2c_ops[0]), TAG_NFC_I2C_TIMEOUT_MS);
}

static esp_err_t nfc_write_block(i2c_master_dev_handle_t dev_handle, uint8_t block_addr, const uint8_t *data) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t write_addr = TAG_NFC_I2C_WRITE_ADDR;
    uint8_t write_buf[1 + TAG_NFC_BLOCK_SIZE] = {0};
    write_buf[0] = block_addr;
    memcpy(&write_buf[1], data, TAG_NFC_BLOCK_SIZE);

    i2c_operation_job_t i2c_ops[] = {
        { .command = I2C_MASTER_CMD_START },
        {
            .command = I2C_MASTER_CMD_WRITE,
            .write = {
                .ack_check = true,
                .data = &write_addr,
                .total_bytes = 1,
            },
        },
        {
            .command = I2C_MASTER_CMD_WRITE,
            .write = {
                .ack_check = true,
                .data = write_buf,
                .total_bytes = sizeof(write_buf),
            },
        },
        { .command = I2C_MASTER_CMD_STOP },
    };

    esp_err_t err = i2c_master_execute_defined_operations(
        dev_handle, i2c_ops, sizeof(i2c_ops) / sizeof(i2c_ops[0]), TAG_NFC_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        esp_rom_delay_us(TAG_NFC_EEPROM_WRITE_US);
    }
    return err;
}

static size_t build_mac_text_ndef(uint8_t *buffer, size_t buffer_size, const char *text) {
    const size_t lang_len = strlen(TAG_NFC_LANG_CODE);
    const size_t text_len = strlen(text);
    const size_t payload_len = 1 + lang_len + text_len;
    const size_t message_len = 4 + payload_len;
    const size_t total_len = 2 + message_len + 1;

    if (!buffer || !text || buffer_size < total_len || payload_len > 0xFF || message_len > 0xFF || lang_len > 0x3F) {
        return 0;
    }

    memset(buffer, 0, buffer_size);
    buffer[0] = 0x03;                  // NDEF message TLV
    buffer[1] = (uint8_t)message_len;  // Short TLV length field
    buffer[2] = 0xD1;                  // MB/ME/SR + TNF=well-known
    buffer[3] = 0x01;                  // Type length
    buffer[4] = (uint8_t)payload_len;  // Payload length
    buffer[5] = 0x54;                  // RTD Text
    buffer[6] = (uint8_t)lang_len;     // UTF-8, language code length
    memcpy(&buffer[7], TAG_NFC_LANG_CODE, lang_len);
    memcpy(&buffer[7 + lang_len], text, text_len);
    buffer[2 + message_len] = 0xFE;    // Terminator TLV

    return total_len;
}

esp_err_t tag_hw_init(const tag_config_t *cfg) {
    if (cfg) {
        s_tamper_gpio = (cfg->tamper_gpio == TAG_TAMPER_GPIO_USE_DEFAULT)
                            ? TAG_DEFAULT_TAMPER_GPIO
                            : (gpio_num_t)cfg->tamper_gpio;
    }

    if (!GPIO_IS_VALID_GPIO(s_tamper_gpio)) {
        s_tamper_gpio = TAG_DEFAULT_TAMPER_GPIO;
    }

    // Configure tamper GPIO (photodiode comparator) when a valid pin exists.
    configure_tamper_gpio(s_tamper_gpio);
    if (GPIO_IS_VALID_GPIO(s_tamper_gpio)) {
        ESP_LOGI(TAG, "Tamper GPIO=%d level=%d", (int)s_tamper_gpio, gpio_get_level(s_tamper_gpio));
    } else {
        ESP_LOGI(TAG, "Tamper GPIO disabled");
    }

    // Keep the switched battery divider fully off during the BLE-only wake path.
    if (GPIO_IS_VALID_GPIO(s_battery_enable_gpio)) {
        gpio_config_t batt_en_conf = {
            .pin_bit_mask = 1ULL << s_battery_enable_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&batt_en_conf);
        gpio_set_level(s_battery_enable_gpio, !TAG_BATTERY_ENABLE_ACTIVE_LEVEL);
    }

    // Configure NFC field detect GPIO. The NTAG FD pin is open-drain on the board's external pull-up.
    if (GPIO_IS_VALID_GPIO(s_nfc_fd_gpio)) {
        gpio_config_t nfc_fd_conf = {
            .pin_bit_mask = 1ULL << s_nfc_fd_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,  // NFC chip has output driver
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&nfc_fd_conf);
    }

    // Keep the NFC I2C pins available for the boot-time NTAG provisioning path.
    (void)s_nfc_sda_gpio;
    (void)s_nfc_scl_gpio;

    // Force NFC power off during the BLE-only wake path.
    if (GPIO_IS_VALID_GPIO(s_nfc_power_gpio)) {
        gpio_config_t nfc_pwr_conf = {
            .pin_bit_mask = 1ULL << s_nfc_power_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&nfc_pwr_conf);
        set_nfc_power(false);
    }

    // Skip ADC init in the BLE-only wake path to avoid powering the battery sense circuit.
    return ESP_OK;
}

esp_err_t tag_hw_write_mac_ndef(void) {
    if (!GPIO_IS_VALID_GPIO(s_nfc_power_gpio) || !GPIO_IS_VALID_GPIO(s_nfc_sda_gpio) || !GPIO_IS_VALID_GPIO(s_nfc_scl_gpio)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool already_provisioned = false;
    esp_err_t err = nfc_is_provisioned(&already_provisioned);
    if (err != ESP_OK) {
        return err;
    }
    if (already_provisioned) {
        ESP_LOGI(TAG, "Skipping NTAG provisioning; already completed once");
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    char mac_text[18] = {0};
    uint8_t block0[TAG_NFC_BLOCK_SIZE] = {0};
    uint8_t ndef_buffer[2 * TAG_NFC_BLOCK_SIZE] = {0};
    uint8_t user_block_a[TAG_NFC_BLOCK_SIZE] = {0};
    uint8_t user_block_b[TAG_NFC_BLOCK_SIZE] = {0};
    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_dev_handle_t dev_handle = NULL;

    err = esp_read_mac(mac, ESP_MAC_BT);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(mac_text,
             sizeof(mac_text),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);

    size_t ndef_len = build_mac_text_ndef(ndef_buffer, sizeof(ndef_buffer), mac_text);
    if (ndef_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    int candidate_levels[2] = {1, 0};
    bool ntag_ready = false;
    nfc_log_bus_levels("before power", GPIO_IS_VALID_GPIO(s_nfc_power_gpio) ? gpio_get_level(s_nfc_power_gpio) : -1);

    for (size_t i = 0; i < 2; ++i) {
        int active_level = candidate_levels[i];
        int inactive_level = !active_level;

        set_nfc_power_level(inactive_level, TAG_NFC_POWER_OFF_SETTLE_US);
        nfc_prepare_bus_gpio_inputs();
        nfc_log_bus_levels(i == 0 ? "power-off primary" : "power-off fallback", inactive_level);

        set_nfc_power_level(active_level, TAG_NFC_POWER_SETTLE_US);
        nfc_prepare_bus_gpio_inputs();
        nfc_log_bus_levels(i == 0 ? "power-on primary" : "power-on fallback", active_level);

        err = nfc_open_i2c(&bus_handle, &dev_handle);
        if (err != ESP_OK) {
            set_nfc_power_level(inactive_level, TAG_NFC_POWER_OFF_SETTLE_US);
            return err;
        }

        esp_err_t probe_err = i2c_master_probe(bus_handle, TAG_NFC_I2C_ADDR, TAG_NFC_I2C_PROBE_TIMEOUT_MS);
        if (probe_err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "NTAG probe timed out with power level=%d, resetting I2C bus", active_level);
            esp_err_t reset_err = i2c_master_bus_reset(bus_handle);
            if (reset_err != ESP_OK) {
                ESP_LOGW(TAG, "I2C bus reset failed: %s", esp_err_to_name(reset_err));
            }
            probe_err = i2c_master_probe(bus_handle, TAG_NFC_I2C_ADDR, TAG_NFC_I2C_PROBE_TIMEOUT_MS);
        }
        if (probe_err != ESP_OK) {
            ESP_LOGW(TAG, "NTAG probe failed with power level=%d: %s", active_level, esp_err_to_name(probe_err));
            nfc_close_i2c(bus_handle, dev_handle);
            bus_handle = NULL;
            dev_handle = NULL;
            set_nfc_power_level(inactive_level, TAG_NFC_POWER_OFF_SETTLE_US);
            if (i == 1) {
                return probe_err;
            }
            continue;
        }

        // Re-open the bus before the first real memory access; this avoids carrying any odd state
        // forward from the probe transaction on boards that are slow to power the NTAG fully.
        nfc_close_i2c(bus_handle, dev_handle);
        bus_handle = NULL;
        dev_handle = NULL;
        err = nfc_open_i2c(&bus_handle, &dev_handle);
        if (err != ESP_OK) {
            set_nfc_power_level(inactive_level, TAG_NFC_POWER_OFF_SETTLE_US);
            return err;
        }

        err = nfc_read_block(dev_handle, TAG_NFC_CC_BLOCK, block0);
        if (err == ESP_OK) {
            ntag_ready = true;
            break;
        }

        ESP_LOGW(TAG, "NTAG block read failed with power level=%d: %s", active_level, esp_err_to_name(err));
        nfc_close_i2c(bus_handle, dev_handle);
        bus_handle = NULL;
        dev_handle = NULL;
        set_nfc_power_level(inactive_level, TAG_NFC_POWER_OFF_SETTLE_US);

        if (i == 1) {
            return err;
        }
    }

    if (!ntag_ready) {
        set_nfc_power(false);
        return ESP_ERR_INVALID_STATE;
    }

    block0[0] = TAG_NFC_DEFAULT_ADDR_BYTE;
    memcpy(&block0[12], k_nt3h2111_cc_bytes, sizeof(k_nt3h2111_cc_bytes));
    err = nfc_write_block(dev_handle, TAG_NFC_CC_BLOCK, block0);
    if (err == ESP_OK) {
        memcpy(user_block_a, ndef_buffer, TAG_NFC_BLOCK_SIZE);
        err = nfc_write_block(dev_handle, TAG_NFC_USER_BLOCK, user_block_a);
    }
    if (err == ESP_OK && ndef_len > TAG_NFC_BLOCK_SIZE) {
        memcpy(user_block_b, &ndef_buffer[TAG_NFC_BLOCK_SIZE], ndef_len - TAG_NFC_BLOCK_SIZE);
        err = nfc_write_block(dev_handle, TAG_NFC_USER_BLOCK + 1U, user_block_b);
    }

    nfc_close_i2c(bus_handle, dev_handle);
    set_nfc_power(false);

    if (err != ESP_OK) {
        return err;
    }

    err = nfc_mark_provisioned();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Provisioned NTAG with NDEF BLE MAC text '%s'", mac_text);
    return ESP_OK;
}

uint8_t tag_hw_read_battery_pct(void) {
    if (!s_adc_ready) {
        esp_err_t err = init_adc();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Battery ADC init failed for profile %s: %s", battery_profile_name(), esp_err_to_name(err));
            return 100;
        }
    }

    int raw_active = 0;
    int raw_inactive = -1;
    uint32_t settle_us = TAG_BATTERY_FAST_SETTLE_US;
    int samples = TAG_BATTERY_FAST_SAMPLES;

    if (GPIO_IS_VALID_GPIO(s_battery_enable_gpio)) {
        gpio_set_level(s_battery_enable_gpio, TAG_BATTERY_ENABLE_ACTIVE_LEVEL);
        if (!read_adc_average(settle_us, samples, &raw_active)) {
            gpio_set_level(s_battery_enable_gpio, !TAG_BATTERY_ENABLE_ACTIVE_LEVEL);
            ESP_LOGW(TAG, "Battery ADC read failed in active state");
            return 100;
        }
        gpio_set_level(s_battery_enable_gpio, !TAG_BATTERY_ENABLE_ACTIVE_LEVEL);
    } else {
        if (!read_adc_average(settle_us, samples, &raw_active)) {
            ESP_LOGW(TAG, "Battery ADC read failed with no enable GPIO");
            return 100;
        }
    }

    uint32_t battery_mv = estimate_battery_mv_from_raw(raw_active);
    uint8_t pct = battery_pct_from_mv(battery_mv);

    ESP_LOGI(TAG,
             "Battery profile=%s active_raw=%d inactive_raw=%d batt_mv=%lu pct=%u enable_gpio=%d active_level=%d settle_us=%lu samples=%d",
             battery_profile_name(), raw_active, raw_inactive, (unsigned long)battery_mv, (unsigned)pct,
             (int)s_battery_enable_gpio, TAG_BATTERY_ENABLE_ACTIVE_LEVEL, (unsigned long)settle_us, samples);
    return pct;
}

bool tag_hw_tamper_triggered(void) {
    if (!GPIO_IS_VALID_GPIO(s_tamper_gpio)) {
        return false;
    }
    // Comparator output is active-low after the photodiode rework.
    return gpio_get_level(s_tamper_gpio) == 0;
}

gpio_num_t tag_hw_tamper_gpio(void) {
    return s_tamper_gpio;
}

bool tag_hw_nfc_field_detected(void) {
    if (!GPIO_IS_VALID_GPIO(s_nfc_fd_gpio)) {
        return false;
    }
    return gpio_get_level(s_nfc_fd_gpio) == 0;  // FD pulls low when the configured NFC event occurs
}

gpio_num_t tag_hw_nfc_fd_gpio(void) {
    return s_nfc_fd_gpio;
}

gpio_num_t tag_hw_nfc_power_gpio(void) {
    return s_nfc_power_gpio;
}

gpio_num_t tag_hw_nfc_sda_gpio(void) {
    return s_nfc_sda_gpio;
}

gpio_num_t tag_hw_nfc_scl_gpio(void) {
    return s_nfc_scl_gpio;
}

void tag_hw_prepare_for_sleep(void) {
    if (GPIO_IS_VALID_GPIO(s_battery_enable_gpio)) {
        gpio_set_level(s_battery_enable_gpio, !TAG_BATTERY_ENABLE_ACTIVE_LEVEL);
    }

    // Power down NFC chip to save power in deep sleep
    if (GPIO_IS_VALID_GPIO(s_nfc_power_gpio)) {
        set_nfc_power(false);
    }

    // Clean up ADC
    if (s_adc_ready) {
        adc_oneshot_del_unit(s_adc_unit);
        s_adc_ready = false;
    }
}
