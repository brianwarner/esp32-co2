/* SPDX-License-Identifier: Apache-2.0 */

#include "co2_stcc4.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "co2";

/* Commands from the STCC4 datasheet (16-bit command words, MSB first). The
 * onboard SHT40 supplies RH/T, so RH/T compensation is never configured. */
#define CMD_START_CONT_MEASURE 0x218B
#define CMD_READ_MEASURE       0xEC05

#define MEASURE_RESP_LEN 12

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

/* Sensirion CRC-8: poly 0x31, init 0xFF, computed over each 16-bit word. */
static uint8_t crc8(uint8_t msb, uint8_t lsb)
{
    uint8_t crc = 0xFF;
    uint8_t bytes[2] = {msb, lsb};

    for (int b = 0; b < 2; b++) {
        crc ^= bytes[b];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t write_cmd16(uint16_t cmd)
{
    uint8_t buf[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), -1);
}

esp_err_t co2_sensor_init(const co2_sensor_pins_t *pins)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = pins->sda,
        .scl_io_num = pins->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = pins->i2c_addr,
        .scl_speed_hz = pins->clock_hz > 0 ? (uint32_t)pins->clock_hz : 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c device add failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }

    /* Wake, in case the sensor was left asleep by a previous run; the
     * datasheet allows sending this even when already awake. */
    uint8_t wakeup = 0x00;
    i2c_master_transmit(s_dev, &wakeup, sizeof(wakeup), -1);
    vTaskDelay(pdMS_TO_TICKS(10));

    err = write_cmd16(CMD_START_CONT_MEASURE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start measurement failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "started continuous measurement on addr 0x%02X", pins->i2c_addr);
    s_ready = true;
    return ESP_OK;
}

bool co2_sensor_read(stcc4_measurement_t *out)
{
    if (!s_ready) {
        return false;
    }

    if (write_cmd16(CMD_READ_MEASURE) != ESP_OK) {
        return false;
    }

    uint8_t rx[MEASURE_RESP_LEN];
    if (i2c_master_receive(s_dev, rx, sizeof(rx), -1) != ESP_OK) {
        return false;
    }

    if (rx[2] != crc8(rx[0], rx[1]) || rx[5] != crc8(rx[3], rx[4]) ||
        rx[8] != crc8(rx[6], rx[7])) {
        ESP_LOGW(TAG, "measurement CRC mismatch, discarding reading");
        return false;
    }

    out->co2_ppm = ((uint16_t)rx[0] << 8) | rx[1];
    uint16_t t_raw = ((uint16_t)rx[3] << 8) | rx[4];
    uint16_t h_raw = ((uint16_t)rx[6] << 8) | rx[7];
    out->temperature_c = -45.0f + (175.0f * (float)t_raw) / 65535.0f;
    out->humidity_pct = -6.0f + (125.0f * (float)h_raw) / 65535.0f;
    return true;
}
