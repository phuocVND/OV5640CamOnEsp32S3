#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* Bit set in g_wifi_event_group when IP is obtained */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

extern EventGroupHandle_t g_wifi_event_group;

/**
 * Initialize WiFi in station mode and start connection.
 * Non-blocking — use g_wifi_event_group to wait for WIFI_CONNECTED_BIT.
 */
void wifi_sta_init(void);

/**
 * Block until WiFi is connected (or failed after max retries).
 * @return true if connected, false if failed
 */
bool wifi_sta_wait_connected(void);
