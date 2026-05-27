#include "tag_ble.h"

#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

#define TAG "tag_ble"
#define TAG_COMPANY_ID 0x1337
#define TAG_BLE_STARTUP_SETTLE_MS 150U
#define TAG_BLE_FAST_STARTUP_SETTLE_MS 5U
#define TAG_BLE_ADV_SETTLE_MS 100U
#define TAG_BLE_FAST_ADV_SETTLE_MS 0U
#define TAG_BLE_READY_TIMEOUT_MS 1000U
#define TAG_BLE_READY_POLL_MS 20U
#define TAG_BLE_TX_POWER ESP_PWR_LVL_P9
#define TAG_BLE_EXT_ADV_INSTANCE 0U

static tag_config_t s_cfg;
static bool s_nimble_started;
static bool s_ble_ready;
static bool s_gap_configured;
static bool s_ext_adv_configured;
static bool s_fast_mode;
static uint8_t s_battery_pct;
static bool s_last_tamper;
static bool s_last_battery_fresh;
static tag_reason_t s_last_reason;

#if CONFIG_BT_NIMBLE_EXT_ADV
static struct ble_gap_ext_adv_params s_ext_adv_params = {
    .connectable = 0,
    .scannable = 0,
    .legacy_pdu = 1,
    .scan_req_notif = 0,
    .itvl_min = 80,
    .itvl_max = 160,
    .own_addr_type = BLE_OWN_ADDR_PUBLIC,
    .primary_phy = BLE_HCI_LE_PHY_1M,
    .secondary_phy = BLE_HCI_LE_PHY_1M,
    .sid = 0,
};
#else
static struct ble_gap_adv_params s_adv_params = {
    .conn_mode = BLE_GAP_CONN_MODE_NON,
    .disc_mode = BLE_GAP_DISC_MODE_GEN,
    .itvl_min = 80,
    .itvl_max = 160,
};
#endif

static void nimble_host_task(void *param);
static void sync_cb(void);
static int gap_event_handler(struct ble_gap_event *event, void *arg);

static uint32_t startup_settle_ms(void) {
    return s_fast_mode ? TAG_BLE_FAST_STARTUP_SETTLE_MS : TAG_BLE_STARTUP_SETTLE_MS;
}

static uint32_t adv_settle_ms(void) {
    return s_fast_mode ? TAG_BLE_FAST_ADV_SETTLE_MS : TAG_BLE_ADV_SETTLE_MS;
}

static uint32_t shutdown_settle_ms(void) {
    return s_fast_mode ? 0U : 50U;
}

static void configure_tx_power(void) {
    esp_err_t err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, TAG_BLE_TX_POWER);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set default BLE TX power: %s", esp_err_to_name(err));
    }

    err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, TAG_BLE_TX_POWER);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set advertising TX power: %s", esp_err_to_name(err));
    }
}

static void populate_adv_fields(struct ble_hs_adv_fields *fields, uint8_t battery_pct, bool tamper, bool battery_fresh, tag_reason_t reason) {
    static uint8_t mfg_payload[5];

    memset(fields, 0, sizeof(*fields));
    fields->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    mfg_payload[0] = TAG_COMPANY_ID & 0xFF;
    mfg_payload[1] = (TAG_COMPANY_ID >> 8) & 0xFF;
    mfg_payload[2] = battery_pct;
    mfg_payload[3] = (tamper ? 0x01 : 0x00) | (battery_fresh ? 0x00 : 0x02);
    mfg_payload[4] = (uint8_t)reason;

    fields->mfg_data = mfg_payload;
    fields->mfg_data_len = sizeof(mfg_payload);

    if (s_cfg.name[0]) {
        fields->name = (uint8_t *)s_cfg.name;
        fields->name_len = strlen(s_cfg.name);
        fields->name_is_complete = 1;
    }
}

#if CONFIG_BT_NIMBLE_EXT_ADV
static int set_ext_adv_payload(uint8_t battery_pct, bool tamper, bool battery_fresh, tag_reason_t reason) {
    struct ble_hs_adv_fields adv_fields;
    struct os_mbuf *adv_data = NULL;
    int rc;

    populate_adv_fields(&adv_fields, battery_pct, tamper, battery_fresh, reason);
    adv_data = os_msys_get_pkthdr(31, 0);
    if (!adv_data) {
        return BLE_HS_ENOMEM;
    }
    rc = ble_hs_adv_set_fields_mbuf(&adv_fields, adv_data);
    if (rc != 0) {
        os_mbuf_free_chain(adv_data);
        return rc;
    }
    return ble_gap_ext_adv_set_data(TAG_BLE_EXT_ADV_INSTANCE, adv_data);
}
#else
static int set_legacy_adv_payload(uint8_t battery_pct, bool tamper, bool battery_fresh, tag_reason_t reason) {
    struct ble_hs_adv_fields adv_fields;

    populate_adv_fields(&adv_fields, battery_pct, tamper, battery_fresh, reason);
    return ble_gap_adv_set_fields(&adv_fields);
}
#endif

static void ensure_gap_params(void) {
    if (s_gap_configured) {
        return;
    }
    ble_svc_gap_device_name_set(s_cfg.name);
    s_gap_configured = true;
}

#if CONFIG_BT_NIMBLE_EXT_ADV
static int ensure_ext_adv_configured(void) {
    if (s_ext_adv_configured) {
        return 0;
    }

    int rc = ble_gap_ext_adv_configure(TAG_BLE_EXT_ADV_INSTANCE, &s_ext_adv_params, NULL, gap_event_handler, NULL);
    if (rc == 0) {
        s_ext_adv_configured = true;
    }
    return rc;
}
#endif

static int start_advertising_internal(void) {
    int rc;

    ensure_gap_params();
#if CONFIG_BT_NIMBLE_EXT_ADV
    rc = ensure_ext_adv_configured();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_configure failed: %d", rc);
        return rc;
    }
    rc = set_ext_adv_payload(s_battery_pct, s_last_tamper, s_last_battery_fresh, s_last_reason);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_set_data failed: %d", rc);
        return rc;
    }
#else
    rc = set_legacy_adv_payload(s_battery_pct, s_last_tamper, s_last_battery_fresh, s_last_reason);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return rc;
    }
#endif

    uint32_t settle_ms = adv_settle_ms();
    if (settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
    }

#if CONFIG_BT_NIMBLE_EXT_ADV
    return ble_gap_ext_adv_start(TAG_BLE_EXT_ADV_INSTANCE, 0, 0);
#else
    return ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &s_adv_params, gap_event_handler, NULL);
#endif
}

static int stop_advertising_internal(void) {
#if CONFIG_BT_NIMBLE_EXT_ADV
    return ble_gap_ext_adv_stop(TAG_BLE_EXT_ADV_INSTANCE);
#else
    return ble_gap_adv_stop();
#endif
}

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGD(TAG, "Advertising completed");
            break;
        default:
            break;
    }
    return 0;
}

static void nimble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void sync_cb(void) {
    uint8_t addr_val[6] = {0};
    uint8_t addr_type;

    ble_hs_id_infer_auto(0, &addr_type);
    ble_hs_id_copy_addr(addr_type, addr_val, NULL);
    ESP_LOGI(TAG, "BLE address %02X:%02X:%02X:%02X:%02X:%02X",
             addr_val[5], addr_val[4], addr_val[3], addr_val[2], addr_val[1], addr_val[0]);
    s_ble_ready = true;
}

static esp_err_t ensure_stack_started(void) {
    if (s_nimble_started) {
        return ESP_OK;
    }

    esp_err_t err = nimble_port_init();
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nimble_port_init failed");

    ble_hs_cfg.sync_cb = sync_cb;
    ble_hs_cfg.reset_cb = NULL;

    nimble_port_freertos_init(nimble_host_task);
    s_nimble_started = true;

    uint32_t settle_ms = startup_settle_ms();
    if (settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
    }
    configure_tx_power();
    return ESP_OK;
}

esp_err_t tag_ble_init(const tag_config_t *cfg) {
    if (cfg) {
        s_cfg = *cfg;
    } else {
        memset(&s_cfg, 0, sizeof(s_cfg));
        strlcpy(s_cfg.name, "Tag", sizeof(s_cfg.name));
    }

    esp_err_t err = ensure_stack_started();
    if (err != ESP_OK) {
        return err;
    }

    uint32_t waited_ms = 0;
    while (!s_ble_ready && waited_ms < TAG_BLE_READY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(TAG_BLE_READY_POLL_MS));
        waited_ms += TAG_BLE_READY_POLL_MS;
    }

    if (!s_ble_ready) {
        ESP_LOGW(TAG, "BLE stack did not sync within %ums", (unsigned)TAG_BLE_READY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t tag_ble_start(uint8_t battery_pct, bool tamper, bool battery_fresh, tag_reason_t reason) {
    ESP_RETURN_ON_ERROR(ensure_stack_started(), TAG, "ensure stack");

    if (!s_ble_ready) {
        ESP_LOGW(TAG, "BLE stack not yet synced; delaying advertisement");
        return ESP_ERR_INVALID_STATE;
    }

    s_battery_pct = battery_pct;
    s_last_tamper = tamper;
    s_last_battery_fresh = battery_fresh;
    s_last_reason = reason;

    int rc = start_advertising_internal();
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE advertise start failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void tag_ble_stop(void) {
    int rc = stop_advertising_internal();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop rc=%d", rc);
    }
}

void tag_ble_set_fast_mode(bool enabled) {
    s_fast_mode = enabled;
}

void tag_ble_shutdown(void) {
    if (!s_nimble_started) {
        return;
    }

    tag_ble_stop();
    int rc = nimble_port_stop();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "nimble_port_stop rc=%d", rc);
    }

    uint32_t settle_ms = shutdown_settle_ms();
    if (settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
    }

    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nimble_port_deinit err=%s", esp_err_to_name(err));
    }

    s_nimble_started = false;
    s_ble_ready = false;
    s_gap_configured = false;
    s_ext_adv_configured = false;
}

void tag_ble_update_battery(uint8_t pct) {
    s_battery_pct = pct;
}
