#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "tag_ble.h"
#include "tag_config.h"
#include "tag_hw.h"

static const char *TAG = "tag_main";
static const uint32_t TAG_FAST_PRE_BLE_INIT_DELAY_MS = 0U;
static const uint32_t TAG_FAST_PRE_BLE_START_DELAY_MS = 0U;
static const uint32_t TAG_BATTERY_REFRESH_INTERVAL = 100U;
static const uint64_t TAG_WAKE_JITTER_MAX_US = 0ULL;
static const uint32_t TAG_ADV_BURST_GAP_US = 10000U;
static const uint32_t TAG_ADV_BURST_COUNT = 6U;

static RTC_DATA_ATTR uint32_t s_short_wake_counter;
static RTC_DATA_ATTR uint8_t s_cached_battery_pct = 100U;
static RTC_DATA_ATTR bool s_cached_battery_valid;

static bool can_use_deep_sleep_wakeup_gpio(gpio_num_t gpio) {
    return GPIO_IS_VALID_GPIO(gpio) && esp_sleep_is_valid_wakeup_gpio(gpio);
}

static const char *reset_reason_to_str(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "unknown";
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "int_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "wdt";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "power_glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu_lockup";
        default:
            return "other";
    }
}

static tag_reason_t resolve_reason(esp_sleep_wakeup_cause_t cause, bool tamper_triggered, bool nfc_triggered) {
    if (nfc_triggered) {
        return TAG_REASON_NFC;
    }
    if (tamper_triggered) {
        return TAG_REASON_TAMPER;
    }
    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            return TAG_REASON_PERIODIC;
        default:
            return TAG_REASON_PERIODIC;
    }
}

static uint32_t effective_adv_burst_count(uint32_t total_window_ms) {
    if (total_window_ms == 0U) {
        return 0U;
    }

    uint32_t burst_count = (total_window_ms < TAG_ADV_BURST_COUNT) ? total_window_ms : TAG_ADV_BURST_COUNT;
    uint64_t total_window_us = (uint64_t)total_window_ms * 1000ULL;
    uint64_t total_gap_us = (burst_count > 1U) ? (uint64_t)(burst_count - 1U) * TAG_ADV_BURST_GAP_US : 0ULL;
    if (total_window_us <= total_gap_us) {
        return 1U;
    }
    return burst_count;
}

static uint32_t adv_burst_window_us(uint32_t total_window_ms, uint32_t burst_index, uint32_t burst_count) {
    if (burst_count == 0U) {
        return 0U;
    }

    uint64_t total_window_us = (uint64_t)total_window_ms * 1000ULL;
    uint64_t total_gap_us = (burst_count > 1U) ? (uint64_t)(burst_count - 1U) * TAG_ADV_BURST_GAP_US : 0ULL;
    if (total_window_us <= total_gap_us) {
        return 0U;
    }

    uint32_t adv_budget_us = (uint32_t)(total_window_us - total_gap_us);
    uint32_t base_window_us = adv_budget_us / burst_count;
    uint32_t remainder_us = adv_budget_us % burst_count;
    return base_window_us + (burst_index < remainder_us ? 1U : 0U);
}

static void delay_window_us(uint32_t duration_us) {
    if (duration_us == 0U) {
        return;
    }

    TickType_t delay_ticks = pdMS_TO_TICKS(duration_us / 1000U);
    uint64_t delayed_us = 0U;
    if (delay_ticks > 0) {
        vTaskDelay(delay_ticks);
        delayed_us = (uint64_t)delay_ticks * (uint64_t)portTICK_PERIOD_MS * 1000ULL;
    }

    if ((uint64_t)duration_us > delayed_us) {
        esp_rom_delay_us((uint32_t)((uint64_t)duration_us - delayed_us));
    }
}

static esp_err_t run_advertising_session(uint32_t total_window_ms,
                                         uint8_t battery_pct,
                                         bool tamper,
                                         bool battery_fresh,
                                         tag_reason_t reason) {
    uint32_t burst_count = effective_adv_burst_count(total_window_ms);
    if (burst_count == 0U) {
        ESP_LOGI(TAG, "Pure beacon mode active; advertising disabled because the total window is 0ms");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Pure beacon mode active; %u burst%s within %ums total wake window",
             (unsigned)burst_count,
             burst_count == 1U ? "" : "s",
             (unsigned)total_window_ms);

    // Standard mode keeps the whole wake session inside 75ms by reserving a fixed split gap.
    for (uint32_t burst_index = 0; burst_index < burst_count; ++burst_index) {
        uint32_t burst_window_us = adv_burst_window_us(total_window_ms, burst_index, burst_count);
        if (burst_window_us == 0U) {
            continue;
        }

        if (burst_index > 0U) {
            ESP_LOGI(TAG, "Waiting %uus before BLE burst %u/%u",
                     (unsigned)TAG_ADV_BURST_GAP_US,
                     (unsigned)(burst_index + 1U),
                     (unsigned)burst_count);
            delay_window_us(TAG_ADV_BURST_GAP_US);
        }

        ESP_LOGI(TAG, "Starting BLE burst %u/%u for %uus",
                 (unsigned)(burst_index + 1U),
                 (unsigned)burst_count,
                 (unsigned)burst_window_us);
        esp_err_t adv_err = tag_ble_start(battery_pct, tamper, battery_fresh, reason);
        if (adv_err != ESP_OK) {
            return adv_err;
        }

        delay_window_us(burst_window_us);
        tag_ble_stop();
    }

    return ESP_OK;
}

void app_main(void) {
    tag_config_t cfg = {0};
    esp_reset_reason_t reset_reason = esp_reset_reason();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    ESP_LOGI(TAG, "Reset reason: %s (%d), wake cause=%d",
             reset_reason_to_str(reset_reason), (int)reset_reason, (int)cause);

    ESP_ERROR_CHECK(tag_config_init(&cfg));
    ESP_ERROR_CHECK(tag_hw_init(&cfg));

    ESP_LOGI(TAG, "Booting tag firmware, wake period=%lluus name=%s",
             (unsigned long long)cfg.wake_period_us, cfg.name);

    if (reset_reason == ESP_RST_POWERON || reset_reason == ESP_RST_UNKNOWN) {
        esp_err_t nfc_provision_err = tag_hw_write_mac_ndef();
        if (nfc_provision_err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "Skipping NTAG MAC provisioning; NFC hardware is not available in this variant");
        } else if (nfc_provision_err != ESP_OK) {
            ESP_LOGW(TAG, "NTAG MAC provisioning failed: %s", esp_err_to_name(nfc_provision_err));
        }
    }

    uint8_t warmup_cycles = tag_config_consume_warmup_slot();
    uint32_t total_adv_window_ms = cfg.adv_window_ms;
    if (total_adv_window_ms == 0U) {
        total_adv_window_ms = TAG_DEFAULT_ADV_WINDOW_MS;
    }
    if (warmup_cycles > 0) {
        ESP_LOGI(TAG, "Warmup slot %u present; keeping configured wake session at %ums total advertising budget",
                 warmup_cycles, (unsigned)total_adv_window_ms);
    }

    bool short_wake_mode = true;
    uint32_t pre_ble_init_delay_ms = TAG_FAST_PRE_BLE_INIT_DELAY_MS;
    uint32_t pre_ble_start_delay_ms = TAG_FAST_PRE_BLE_START_DELAY_MS;

    ESP_LOGI(TAG, "Waiting %ums before BLE init to let supply settle", (unsigned)pre_ble_init_delay_ms);
    if (pre_ble_init_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(pre_ble_init_delay_ms));
    }

    bool tamper = tag_hw_tamper_triggered();
    bool nfc = tag_hw_nfc_field_detected();

    // Check if woken by GPIO (tamper or NFC)
    if (cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_GPIO) {
        uint64_t wakeup_pin_mask = 0;
#if SOC_PM_SUPPORT_EXT1_WAKEUP
        if (cause == ESP_SLEEP_WAKEUP_EXT1) {
            wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
        }
#endif
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        if (cause == ESP_SLEEP_WAKEUP_GPIO) {
            wakeup_pin_mask = esp_sleep_get_gpio_wakeup_status();
        }
#endif
        if (can_use_deep_sleep_wakeup_gpio(tag_hw_tamper_gpio()) &&
            (wakeup_pin_mask & (1ULL << tag_hw_tamper_gpio()))) {
            tamper = true;
        }
        if (can_use_deep_sleep_wakeup_gpio(tag_hw_nfc_fd_gpio()) &&
            (wakeup_pin_mask & (1ULL << tag_hw_nfc_fd_gpio()))) {
            nfc = true;
        }
    }

    uint8_t battery_pct = 100;
    bool refreshed_battery = false;
    if (!short_wake_mode || !s_cached_battery_valid || (s_short_wake_counter % TAG_BATTERY_REFRESH_INTERVAL) == 0) {
        battery_pct = tag_hw_read_battery_pct();
        s_cached_battery_pct = battery_pct;
        s_cached_battery_valid = true;
        refreshed_battery = true;
    } else {
        battery_pct = s_cached_battery_pct;
    }
    tag_reason_t reason = resolve_reason(cause, tamper, nfc);

    if (refreshed_battery) {
        ESP_LOGI(TAG, "Advertising with measured battery level %u%% (fresh=true)", (unsigned)battery_pct);
    } else {
        ESP_LOGI(TAG, "Advertising with cached battery level %u%% (fresh=false, refresh every %u short wakes)",
                 (unsigned)battery_pct, (unsigned)TAG_BATTERY_REFRESH_INTERVAL);
    }
    tag_ble_update_battery(battery_pct);
    ESP_LOGI(TAG, "Delaying %ums before advertising", (unsigned)pre_ble_start_delay_ms);
    if (pre_ble_start_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(pre_ble_start_delay_ms));
    }

    tag_ble_set_fast_mode(short_wake_mode);
    ESP_ERROR_CHECK(tag_ble_init(&cfg));

    uint64_t next_wake_us = cfg.wake_period_us;
    uint32_t wake_jitter_us = 0;
    if (TAG_WAKE_JITTER_MAX_US > 0) {
        wake_jitter_us = esp_random() % (uint32_t)(TAG_WAKE_JITTER_MAX_US + 1ULL);
        next_wake_us += wake_jitter_us;
    }
    ++s_short_wake_counter;

    esp_err_t adv_err = run_advertising_session(total_adv_window_ms, battery_pct, tamper, refreshed_battery, reason);
    if (adv_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE advertise start failed: %s", esp_err_to_name(adv_err));
    }

    tag_ble_shutdown();

    tag_hw_prepare_for_sleep();

    ESP_LOGI(TAG, "Wake session complete; sleeping for %lluus (jitter=%uus)",
             (unsigned long long)next_wake_us,
             (unsigned)wake_jitter_us);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(next_wake_us));

    // Enable deep-sleep wakeup for both tamper (GPIO0) and NFC FD (GPIO5).
    uint64_t wakeup_mask = 0;
    gpio_num_t tamper_gpio = tag_hw_tamper_gpio();
    gpio_num_t nfc_fd_gpio = tag_hw_nfc_fd_gpio();

    if (can_use_deep_sleep_wakeup_gpio(tamper_gpio)) {
        wakeup_mask |= (1ULL << tamper_gpio);
    } else if (GPIO_IS_VALID_GPIO(tamper_gpio)) {
        ESP_LOGW(TAG, "Skipping GPIO %d for deep sleep wakeup; target does not support it", (int)tamper_gpio);
    }
    if (can_use_deep_sleep_wakeup_gpio(nfc_fd_gpio)) {
        wakeup_mask |= (1ULL << nfc_fd_gpio);
    } else if (GPIO_IS_VALID_GPIO(nfc_fd_gpio)) {
        ESP_LOGW(TAG, "Skipping GPIO %d for deep sleep wakeup; target does not support it", (int)nfc_fd_gpio);
    }

    if (wakeup_mask != 0) {
        // The production tamper comparator and NTAG FD lines are both active-low.
#if SOC_PM_SUPPORT_EXT1_WAKEUP
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW));
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(wakeup_mask, ESP_GPIO_WAKEUP_GPIO_LOW));
#else
#error "No supported deep sleep GPIO wakeup method for this target"
#endif
    }

    esp_deep_sleep_start();
}
