#include "wifi_sta.h"
#include "app_config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_sta";

EventGroupHandle_t g_wifi_event_group;
static int s_retry = 0;

static const char* authmode_to_str(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        default:                        return "UNKNOWN";
    }
}

static void wifi_scan_networks(void)
{
    ESP_LOGI(TAG, "=== Starting WiFi scan ===");
    
    wifi_scan_config_t scan_cfg = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, true));  /* blocking scan */
    
    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No WiFi networks found!");
        return;
    }
    
    ESP_LOGI(TAG, "=== Found %d networks ===", ap_count);
    
    wifi_ap_record_t *ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));
    
    for (int i = 0; i < ap_count; i++) {
        const char *ssid = (ap_list[i].ssid[0] != 0) ? (char *)ap_list[i].ssid : "(hidden)";
        const char *auth_str = authmode_to_str(ap_list[i].authmode);
        
        ESP_LOGI(TAG, "[%d] SSID=%s | RSSI=%d dBm | CH=%d | AUTH=%s",
                 i + 1, ssid, ap_list[i].rssi, ap_list[i].primary, auth_str);
        
        if (strcmp(ssid, WIFI_SSID) == 0) {
            ESP_LOGI(TAG, "    ✓ TARGET FOUND: SSID=%s matches config!", WIFI_SSID);
        }
    }
    
    ESP_LOGI(TAG, "=== Scan complete ===");
    
    free(ap_list);
    esp_wifi_clear_ap_list();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Scan BEFORE attempting to connect */
        wifi_scan_networks();
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "Reconnecting... (%d/%d)", s_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRY);
            xEventGroupSetBits(g_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_sta_init(void)
{
    /* NVS required by WiFi driver */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    g_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,  /* WPA2 only */
        },
    };
    
    ESP_LOGI(TAG, "DEBUG: Configured SSID=[%s] PASS=[%s] AUTHMODE=WPA2", WIFI_SSID, WIFI_PASSWORD);
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init done. Waiting for STA_START event...");
}

bool wifi_sta_wait_connected(void)
{
    EventBits_t bits = xEventGroupWaitBits(
        g_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}
