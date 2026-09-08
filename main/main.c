/* SPDX-License-Identifier: Apache-2.0 */
/* E-paper CO2 monitor for ESP32-S3 SuperMini + Waveshare 1.54" e-Paper V2
 * (SSD1681) + Gravity STCC4 CO2 sensor.
 *
 * Time comes from SNTP over Wi-Fi. Timezone, 12/24-hour mode, the
 * temperature unit, and the CO2 sample interval are set in .env and baked
 * into env_config.h at build time by scripts/gen_env_config.sh.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "clockface.h"
#include "co2_history_store.h"
#include "co2_stcc4.h"
#include "env_config.h"
#include "epd_ssd1681.h"
#include "gfx.h"
#include "net_time.h"
#include "rgb_led.h"

static const char *TAG = "clock";

/* ---- Wiring -------------------------------------------------------------
 * Change these to match how the panel and sensor are connected. See
 * docs/WIRING.md.
 */
#define PIN_BUSY 8
#define PIN_RST 9
#define PIN_DC 10
#define PIN_CS 11
#define PIN_CLK 12
#define PIN_DIN 13

#define PIN_I2C_SDA 1
#define PIN_I2C_SCL 2

/* Onboard WS2812 RGB LED, most ESP32-S3 SuperMini boards. */
#define PIN_RGB_LED 48

/* Lower this if the panel misbehaves on long jumper wires. */
#define SPI_CLOCK_HZ (1 * 1000 * 1000)
#define I2C_CLOCK_HZ (100 * 1000)

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define NTP_SYNC_TIMEOUT_MS 60000

/* Each history bucket covers 30 minutes; 144 of them span 72 hours. */
#define BUCKET_SECONDS (30 * 60)

/* Window for the CO2 average that drives the RGB LED, and enough samples to
 * cover it even at the minimum 1-second CFG_SAMPLE_INTERVAL_SECONDS. */
#define CO2_AVG_WINDOW_SECONDS 120
#define CO2_AVG_MAX_SAMPLES 120

static uint8_t s_front[EPD_BUF_SIZE]; /* what the panel is currently showing */
static uint8_t s_back[EPD_BUF_SIZE];  /* what we are about to show */

static uint16_t s_history[CO2_HISTORY_LEN]; /* ppm, 0 = no reading yet */
static long s_bucket_epoch = -1;            /* bucket id of history[last] */

/* Push s_back to the panel and keep it as the difference base for the next
 * partial refresh. */
static void present(bool full)
{
    if (full)
    {
        epd_display_full(s_back);
    }
    else
    {
        epd_display_partial(s_back, s_front);
    }
    memcpy(s_front, s_back, EPD_BUF_SIZE);
}

/* Rolls the 30-minute buckets forward to `now` and folds `co2_ppm` into the
 * current (rightmost, still-filling) bucket as a running max. Returns true
 * if at least one bucket rolled, i.e. a bucket was finalized - the signal
 * the caller uses to decide when to persist history to flash. */
static bool history_update(time_t now, uint16_t co2_ppm)
{
    long bucket_id = (long)(now / BUCKET_SECONDS);
    bool rolled = false;

    if (s_bucket_epoch < 0)
    {
        s_bucket_epoch = bucket_id;
    }

    while (bucket_id > s_bucket_epoch)
    {
        memmove(&s_history[0], &s_history[1], sizeof(s_history[0]) * (CO2_HISTORY_LEN - 1));
        s_history[CO2_HISTORY_LEN - 1] = 0;
        s_bucket_epoch++;
        rolled = true;
    }

    if (co2_ppm > s_history[CO2_HISTORY_LEN - 1])
    {
        s_history[CO2_HISTORY_LEN - 1] = co2_ppm;
    }

    return rolled;
}

/* Called once, right after NTP sync succeeds for the first time this boot.
 * Until now, history_update has been rolling buckets forward from
 * boot_bucket_epoch (the bucket persisted at the last save before this
 * boot) using elapsed boot time as a stand-in for the clock, on the
 * assumption that the reboot took zero time. Comparing that assumption
 * against the real time NTP just supplied tells us how long the device was
 * actually powered off: if a gap opened up, splice in that many empty
 * buckets so the histogram shows a gap instead of quietly compressing the
 * outage out of the timeline. */
static void history_reconcile(time_t real_now, long boot_bucket_epoch)
{
    long actual_bucket = (long)(real_now / BUCKET_SECONDS);
    long gap = actual_bucket - s_bucket_epoch;

    if (gap > 0)
    {
        long rolled_since_boot = s_bucket_epoch - boot_bucket_epoch;
        int insert_pos = CO2_HISTORY_LEN - (int)rolled_since_boot;
        if (insert_pos < 0)
        {
            insert_pos = 0;
        }
        if (insert_pos > CO2_HISTORY_LEN)
        {
            insert_pos = CO2_HISTORY_LEN;
        }

        int g = (gap > insert_pos) ? insert_pos : (int)gap;
        if (g < insert_pos)
        {
            memmove(&s_history[0], &s_history[g], sizeof(s_history[0]) * (insert_pos - g));
        }
        memset(&s_history[insert_pos - g], 0, sizeof(s_history[0]) * g);

        ESP_LOGW(TAG, "clock sync found a %ld-bucket (%ld min) gap since the last save; "
                      "showing it as a gap in the history",
                 gap, gap * BUCKET_SECONDS / 60);
    }

    s_bucket_epoch = actual_bucket;
}

/* Ring buffer of recent (timestamp, ppm) samples, used to compute the
 * trailing average that drives the RGB LED's CO2 thresholds. */
static time_t s_co2_avg_at[CO2_AVG_MAX_SAMPLES];
static uint16_t s_co2_avg_ppm[CO2_AVG_MAX_SAMPLES];
static int s_co2_avg_head;
static int s_co2_avg_count;

static void co2_avg_push(time_t now, uint16_t ppm)
{
    s_co2_avg_at[s_co2_avg_head] = now;
    s_co2_avg_ppm[s_co2_avg_head] = ppm;
    s_co2_avg_head = (s_co2_avg_head + 1) % CO2_AVG_MAX_SAMPLES;
    if (s_co2_avg_count < CO2_AVG_MAX_SAMPLES)
    {
        s_co2_avg_count++;
    }
}

/* Average of samples taken within the last CO2_AVG_WINDOW_SECONDS. Returns
 * false (leaving *out_avg unchanged) if no sample has been taken yet. */
static bool co2_avg_get(time_t now, uint16_t *out_avg)
{
    if (s_co2_avg_count == 0)
    {
        return false;
    }

    long sum = 0;
    int n = 0;
    for (int i = 0; i < s_co2_avg_count; i++)
    {
        int idx = (s_co2_avg_head - 1 - i + CO2_AVG_MAX_SAMPLES) % CO2_AVG_MAX_SAMPLES;
        if (now - s_co2_avg_at[idx] > CO2_AVG_WINDOW_SECONDS)
        {
            break;
        }
        sum += s_co2_avg_ppm[idx];
        n++;
    }
    *out_avg = (uint16_t)(sum / n);
    return true;
}

/* ---- RGB LED: CO2 status indicator ---------------------------------------
 * Priority order, highest first: sensor missing, then the 2-minute CO2
 * average against the .env thresholds. LED_MODE_STARTUP (blue) is the
 * initial value, shown until either the sensor is found missing or the
 * first average becomes available.
 */
typedef enum
{
    LED_MODE_STARTUP,   /* solid blue */
    LED_MODE_NO_SENSOR, /* flashing purple */
    LED_MODE_WARNING,   /* flashing red: avg > CFG_CO2_WARNING_PPM */
    LED_MODE_ALERT,     /* solid red: avg > CFG_CO2_ALERT_PPM */
    LED_MODE_ELEVATED,  /* solid yellow: avg > CFG_CO2_ELEVATED_PPM */
    LED_MODE_NORMAL,    /* solid green */
} led_mode_t;

/* Written by app_main's main loop, read by rgb_led_task. A plain enum store
 * is a single atomic word write on this target, so this one-writer/one-reader
 * flag needs no lock. */
static volatile led_mode_t s_led_mode = LED_MODE_STARTUP;

static led_mode_t compute_led_mode(bool sensor_present, bool have_avg, uint16_t avg_ppm)
{
    if (!sensor_present)
    {
        return LED_MODE_NO_SENSOR;
    }
    if (!have_avg)
    {
        return LED_MODE_STARTUP;
    }
    if (avg_ppm > CFG_CO2_WARNING_PPM)
    {
        return LED_MODE_WARNING;
    }
    if (avg_ppm > CFG_CO2_ALERT_PPM)
    {
        return LED_MODE_ALERT;
    }
    if (avg_ppm > CFG_CO2_ELEVATED_PPM)
    {
        return LED_MODE_ELEVATED;
    }
    return LED_MODE_NORMAL;
}

/* Drives the LED from s_led_mode: solid for steady states, 1s on/1s off for
 * the two flashing states. */
static void rgb_led_task(void *arg)
{
    (void)arg;
    bool flash_on = true;

    while (1)
    {
        switch (s_led_mode)
        {
        case LED_MODE_NO_SENSOR:
            rgb_led_set(flash_on ? 128 : 0, 0, flash_on ? 128 : 0);
            break;
        case LED_MODE_WARNING:
            rgb_led_set(flash_on ? 255 : 0, 0, 0);
            break;
        case LED_MODE_ALERT:
            rgb_led_set(255, 0, 0);
            break;
        case LED_MODE_ELEVATED:
            rgb_led_set(255, 100, 0);
            break;
        case LED_MODE_NORMAL:
            rgb_led_set(0, 255, 5);
            break;
        case LED_MODE_STARTUP:
        default:
            rgb_led_set(0, 0, 255);
            break;
        }

        flash_on = !flash_on;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Reads the sensor (if a new reading is ready) and folds it into history,
 * the LED average, and *reading at `sample_time`. Used both by the
 * pre-sync wait (with a pseudo clock) and the main loop (with the real
 * clock), so history keeps accumulating even before NTP has synced.
 * Returns true if a new sample was taken. */
static bool sample_co2(time_t sample_time, bool sensor_present, co2_reading_t *reading)
{
    bool got_sample = false;

    stcc4_measurement_t m;
    if (co2_sensor_read(&m))
    {
        reading->valid = true;
        reading->co2_ppm = m.co2_ppm;
        reading->temperature_c = m.temperature_c;
        reading->humidity_pct = m.humidity_pct;
        if (history_update(sample_time, m.co2_ppm))
        {
            co2_history_save(s_history, s_bucket_epoch);
        }
        co2_avg_push(sample_time, m.co2_ppm);
        got_sample = true;
    }

    if (sensor_present)
    {
        uint16_t avg_ppm = 0;
        bool have_avg = co2_avg_get(sample_time, &avg_ppm);
        s_led_mode = compute_led_mode(sensor_present, have_avg, avg_ppm);
    }

    return got_sample;
}

void app_main(void)
{
    ESP_LOGI(TAG, "TZ=%s, %s clock, temp in %s", CFG_TZ, CFG_CLOCK_24H ? "24-hour" : "12-hour",
             CFG_TEMP_UNIT_F ? "F" : "C");

    setenv("TZ", CFG_TZ, 1);
    tzset();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    epd_pins_t pins = {
        .host = SPI2_HOST,
        .mosi = PIN_DIN,
        .sclk = PIN_CLK,
        .cs = PIN_CS,
        .dc = PIN_DC,
        .rst = PIN_RST,
        .busy = PIN_BUSY,
        .clock_hz = SPI_CLOCK_HZ,
    };
    ESP_ERROR_CHECK(epd_init(&pins));

    rgb_led_pins_t rgb_pins = {.gpio = PIN_RGB_LED};
    bool rgb_led_present = (rgb_led_init(&rgb_pins) == ESP_OK);
    if (rgb_led_present)
    {
        xTaskCreate(rgb_led_task, "rgb_led", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    }
    else
    {
        ESP_LOGW(TAG, "RGB LED not found, skipping CO2 indicator");
    }

    co2_sensor_pins_t sensor_pins = {
        .sda = PIN_I2C_SDA,
        .scl = PIN_I2C_SCL,
        .i2c_addr = STCC4_I2C_ADDR_PRIMARY,
        .clock_hz = I2C_CLOCK_HZ,
    };
    bool sensor_present = (co2_sensor_init(&sensor_pins) == ESP_OK);
    if (!sensor_present)
    {
        ESP_LOGW(TAG, "CO2 sensor not found, continuing without it");
        s_led_mode = LED_MODE_NO_SENSOR;
    }

    /* boot_bucket_epoch is the bucket id as of the last save before this
     * boot (-1 if nothing was ever saved) - kept separate from the live
     * s_bucket_epoch, which starts there but keeps rolling forward during
     * the pre-sync wait below. history_reconcile needs both to work out how
     * many buckets were rolled for real vs. assumed. */
    long boot_bucket_epoch = -1;
    if (co2_history_load(s_history, &boot_bucket_epoch))
    {
        s_bucket_epoch = boot_bucket_epoch;
        ESP_LOGI(TAG, "restored CO2 history from flash (bucket %ld)", boot_bucket_epoch);
    }
    else
    {
        ESP_LOGI(TAG, "no saved CO2 history, starting fresh");
    }

    clockface_status(s_back, "WIFI", CFG_WIFI_SSID_1);
    present(true);

    static const net_time_wifi_t wifi_networks[] = {
        { CFG_WIFI_SSID_1, CFG_WIFI_PASS_1 },
        { CFG_WIFI_SSID_2, CFG_WIFI_PASS_2 },
        { CFG_WIFI_SSID_3, CFG_WIFI_PASS_3 },
    };

    if (net_time_start(wifi_networks, CFG_WIFI_COUNT, CFG_NTP_SERVER,
                       WIFI_CONNECT_TIMEOUT_MS) != ESP_OK)
    {
        ESP_LOGW(TAG, "no IP within %d ms, still waiting", WIFI_CONNECT_TIMEOUT_MS);
    }

    clockface_status(s_back, "SYNC", CFG_NTP_SERVER);
    present(false);

    int minutes_since_full = 0;
    int last_min = -1;
    int last_shown_co2 = -1; /* ppm currently on the panel; -1 = nothing shown yet */
    co2_reading_t reading = {0};
    time_t next_sample_due = 0; /* due immediately, so the first loop samples */

    /* Until NTP syncs, there's no real clock to sample against. Rather than
     * sit idle, keep sampling the sensor using elapsed boot time as a
     * stand-in - as if this boot picked up right where the last save left
     * off, i.e. assuming the reboot took zero time. history_reconcile()
     * below corrects that assumption once the real time is known. */
    int64_t boot_time_us = esp_timer_get_time();
    time_t pseudo_epoch_base = (boot_bucket_epoch >= 0) ? (time_t)(boot_bucket_epoch * BUCKET_SECONDS) : 0;
    bool timed_out_waiting = false; /* past NTP_SYNC_TIMEOUT_MS with still no clock */

    while (!net_time_is_synced())
    {
        time_t pseudo_now = pseudo_epoch_base + (time_t)((esp_timer_get_time() - boot_time_us) / 1000000);
        bool got_sample = false;
        if (pseudo_now >= next_sample_due)
        {
            got_sample = sample_co2(pseudo_now, sensor_present, &reading);
            next_sample_due = pseudo_now + CFG_SAMPLE_INTERVAL_SECONDS;
        }

        /* No point sitting on a status screen forever waiting for a network
         * or a clock: once we've given NTP a fair shot, show what we
         * actually have (CO2 reading, history) without a clock, and keep it
         * fresh until a real time source (NTP now, an RTC later) shows up. */
        bool co2_changed = got_sample && (int)reading.co2_ppm != last_shown_co2;
        if (!timed_out_waiting &&
            (esp_timer_get_time() - boot_time_us) > (int64_t)NTP_SYNC_TIMEOUT_MS * 1000)
        {
            timed_out_waiting = true;
            clockface_render(s_back, NULL, false, &reading, s_history);
            present(true);
            last_shown_co2 = reading.co2_ppm;
        }
        else if (timed_out_waiting && co2_changed)
        {
            clockface_render(s_back, NULL, false, &reading, s_history);
            present(false);
            last_shown_co2 = reading.co2_ppm;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    time_t synced_at;
    time(&synced_at);
    ESP_LOGI(TAG, "clock set from ntp");
    history_reconcile(synced_at, boot_bucket_epoch);
    co2_history_save(s_history, s_bucket_epoch);
    next_sample_due = 0; /* due immediately, now against the real clock */

    while (1)
    {
        time_t now;
        struct tm tm;

        time(&now);
        localtime_r(&now, &tm);

        bool got_new_sample = false;
        if (now >= next_sample_due)
        {
            got_new_sample = sample_co2(now, sensor_present, &reading);
            next_sample_due = now + CFG_SAMPLE_INTERVAL_SECONDS;
        }

        bool minute_changed = (tm.tm_min != last_min);
        bool co2_changed = got_new_sample && (int)reading.co2_ppm != last_shown_co2;
        bool full = false;

        if (minute_changed)
        {
            if (last_min == -1)
            {
                /* Force a full refresh out of the WIFI/SYNC status screens. */
                full = true;
            }
            else if (CFG_REFRESH_ANCHOR_MINUTES >= 0)
            {
                /* Anchor full refreshes to a fixed time of day rather than to
                 * boot time, so they land on predictable wall-clock times. */
                int minute_of_day = tm.tm_hour * 60 + tm.tm_min;
                int since_anchor = (minute_of_day - CFG_REFRESH_ANCHOR_MINUTES) % CFG_REFRESH_INTERVAL_MINUTES;
                if (since_anchor < 0)
                {
                    since_anchor += CFG_REFRESH_INTERVAL_MINUTES;
                }
                full = (since_anchor == 0);
            }
            else
            {
                full = minutes_since_full >= CFG_REFRESH_INTERVAL_MINUTES;
            }

            minutes_since_full = full ? 0 : minutes_since_full + 1;
            last_min = tm.tm_min;
        }

        /* Repaint on a minute tick (for the clock/histogram/full-refresh
         * schedule) or as soon as the CO2 reading changes, whichever comes
         * first, so the big number stays as fresh as the sample interval
         * without repainting on every identical sample. */
        if (minute_changed || co2_changed)
        {
            clockface_render(s_back, &tm, true, &reading, s_history);
            present(full);
            last_shown_co2 = reading.co2_ppm;
        }

        /* Wake at whichever comes first: the next sample, or the next minute
         * boundary (for the display). */
        time(&now);
        localtime_r(&now, &tm);
        time_t next_minute = now + (60 - tm.tm_sec);
        time_t wake_at = next_sample_due < next_minute ? next_sample_due : next_minute;
        int sleep_ms = (int)(wake_at - now) * 1000 + 250;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms > 250 ? sleep_ms : 250));
    }
}
