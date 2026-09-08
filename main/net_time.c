/* SPDX-License-Identifier: Apache-2.0 */

#include "net_time.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "net";

#define GOT_IP_BIT BIT0

/* Any timestamp past 2023-01-01 means SNTP has set the clock. */
#define TIME_IS_SET_EPOCH 1672531200

static EventGroupHandle_t s_events;
static volatile bool s_connected;
static const net_time_wifi_t *s_networks;
static int s_network_count;
static int s_network_index;

static void connect_to_current_network(void)
{
    const net_time_wifi_t *net = &s_networks[s_network_index];

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, net->ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, net->password, sizeof(wc.sta.password) - 1);

    ESP_LOGI(TAG, "connecting to '%s' (%d/%d)", net->ssid, s_network_index + 1, s_network_count);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        connect_to_current_network();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_network_index = (s_network_index + 1) % s_network_count;
        ESP_LOGW(TAG, "disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        connect_to_current_network();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

esp_err_t net_time_start(const net_time_wifi_t *networks, int network_count,
                         const char *ntp_server, int connect_timeout_ms)
{
    s_events = xEventGroupCreate();
    s_networks = networks;
    s_network_count = network_count;
    s_network_index = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(connect_timeout_ms));

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntp_server);
    esp_sntp_init();

    return (bits & GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool net_time_is_synced(void)
{
    time_t now = 0;
    time(&now);
    return now > TIME_IS_SET_EPOCH;
}

bool net_time_is_connected(void)
{
    return s_connected;
}
