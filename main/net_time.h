/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    const char *ssid;
    const char *password;
} net_time_wifi_t;

/* Brings up Wi-Fi in station mode and starts the SNTP client. `networks` is
 * tried in order, round-robining to the next entry each time the connection
 * drops or fails (so an AP that's out of range just gets skipped). Returns
 * once the station has an IP or the timeout expires. Caller must have already
 * initialized NVS (nvs_flash_init) - the Wi-Fi driver needs it. */
esp_err_t net_time_start(const net_time_wifi_t *networks, int network_count,
                         const char *ntp_server, int connect_timeout_ms);

/* True once the system clock has been set from NTP. Non-blocking. */
bool net_time_is_synced(void);

bool net_time_is_connected(void);
