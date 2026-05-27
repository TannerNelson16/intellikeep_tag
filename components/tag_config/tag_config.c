#include "tag_config.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

#define TAG_NAMESPACE "cfg"
static const char *TAG = "tag_config";

static const tag_config_t k_default_cfg = {
    .wake_period_us = TAG_DEFAULT_WAKE_PERIOD_US,
    .adv_window_ms = TAG_DEFAULT_ADV_WINDOW_MS,
    .tamper_gpio = TAG_TAMPER_GPIO_USE_DEFAULT,
    .name = TAG_DEFAULT_NAME,
};

static const uint8_t k_default_warmup_cycles = 2;
static const char *k_warmup_key = "warmup_rem";

static esp_err_t open_namespace(nvs_handle_t *handle) {
    return nvs_open(TAG_NAMESPACE, NVS_READWRITE, handle);
}

static void sanitize(tag_config_t *cfg) {
    if (!cfg) {
        return;
    }
    if (cfg->wake_period_us < TAG_MIN_WAKE_PERIOD_US || cfg->wake_period_us > TAG_MAX_WAKE_PERIOD_US) {
        cfg->wake_period_us = k_default_cfg.wake_period_us;
    }
    if (cfg->adv_window_ms == 0 || cfg->adv_window_ms > (60U * 1000U)) {
        cfg->adv_window_ms = k_default_cfg.adv_window_ms;
    }
    if (cfg->tamper_gpio == TAG_TAMPER_GPIO_USE_DEFAULT) {
        cfg->tamper_gpio = k_default_cfg.tamper_gpio;
    }
    if (cfg->name[0] == '\0') {
        strlcpy(cfg->name, k_default_cfg.name, sizeof(cfg->name));
    }
}

esp_err_t tag_config_init(tag_config_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg is null");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Re-initialising NVS due to err=0x%x", err);
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    *cfg = k_default_cfg;

    nvs_handle_t handle;
    err = open_namespace(&handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Using defaults, namespace open failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    uint64_t period_us = cfg->wake_period_us;
    size_t name_len = sizeof(cfg->name);
    esp_err_t get_err = ESP_OK;
    get_err |= nvs_get_u64(handle, "period_us", &period_us);
    get_err |= nvs_get_u32(handle, "adv_window_ms", &cfg->adv_window_ms);
    get_err |= nvs_get_u8(handle, "tamper_gpio", &cfg->tamper_gpio);
    get_err |= nvs_get_str(handle, "name", cfg->name, &name_len);

    if (get_err != ESP_OK) {
        ESP_LOGI(TAG, "No persisted config yet, keeping defaults");
    } else {
        cfg->wake_period_us = period_us;
    }

    nvs_close(handle);
    sanitize(cfg);
    return ESP_OK;
}

esp_err_t tag_config_save(const tag_config_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg is null");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_namespace(&handle), TAG, "nvs_open failed");

    esp_err_t err = ESP_OK;
    err |= nvs_set_u64(handle, "period_us", cfg->wake_period_us);
    err |= nvs_set_u32(handle, "adv_window_ms", cfg->adv_window_ms);
    err |= nvs_set_u8(handle, "tamper_gpio", cfg->tamper_gpio);
    err |= nvs_set_str(handle, "name", cfg->name);

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

uint8_t tag_config_consume_warmup_slot(void) {
    nvs_handle_t handle;
    esp_err_t err = open_namespace(&handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Warmup tracking unavailable, open failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint8_t remaining = 0;
    err = nvs_get_u8(handle, k_warmup_key, &remaining);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        remaining = k_default_warmup_cycles;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Warmup read failed: %s", esp_err_to_name(err));
        remaining = 0;
    }

    uint8_t slot = remaining;
    if (slot > 0) {
        uint8_t updated = slot - 1;
        esp_err_t set_err = nvs_set_u8(handle, k_warmup_key, updated);
        if (set_err != ESP_OK) {
            ESP_LOGW(TAG, "Warmup store failed: %s", esp_err_to_name(set_err));
        } else {
            esp_err_t commit_err = nvs_commit(handle);
            if (commit_err != ESP_OK) {
                ESP_LOGW(TAG, "Warmup commit failed: %s", esp_err_to_name(commit_err));
            }
        }
    }

    nvs_close(handle);
    return slot;
}
