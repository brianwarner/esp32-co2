/* SPDX-License-Identifier: Apache-2.0 */

#include "epd_ssd1681.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

static spi_device_handle_t s_dev;
static epd_pins_t s_pins;

/* Waveshare's reference LUT for the SSD1681 on the 1.54" V2 panel, in place of
 * the controller's built-in OTP default. Bytes 0-152 are the waveform LUT
 * itself (command 0x32); 153 is the LUT end option (0x3F), 154 is gate
 * driving voltage (0x03), 155-157 are source driving voltage VSH1/VSH2/VSL
 * (0x04), 158 is VCOM (0x2C). */
static const uint8_t LUT_FULL[159] = {
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x01, 0x00, 0x08, 0x01, 0x00, 0x02,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
    0x22, 0x17, 0x41, 0x00, 0x32, 0x20,
};

static const uint8_t LUT_PARTIAL[159] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
    0x02, 0x17, 0x41, 0xB0, 0x32, 0x28,
};

/* BUSY is driven high while the controller is working. */
static void epd_wait_busy(int timeout_ms)
{
    int waited = 0;
    while (gpio_get_level(s_pins.busy) == 1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "BUSY still asserted after %d ms", timeout_ms);
            return;
        }
    }
}

static void epd_write(const uint8_t *data, size_t len, bool is_cmd)
{
    gpio_set_level(s_pins.dc, is_cmd ? 0 : 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_dev, &t));
}

static void epd_cmd(uint8_t c)
{
    epd_write(&c, 1, true);
}

static void epd_data(const uint8_t *d, size_t len)
{
    epd_write(d, len, false);
}

static void epd_data1(uint8_t d)
{
    epd_write(&d, 1, false);
}

static void epd_hw_reset(void)
{
    gpio_set_level(s_pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_pins.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_pins.rst, 1);
    /* Coming out of deep sleep takes noticeably longer to settle than a
     * plain soft reset (POR + oscillator startup); too short a delay here
     * leaves BUSY stuck high indefinitely on the very next wake. */
    vTaskDelay(pdMS_TO_TICKS(200));
    epd_wait_busy(2000);
}

/* Address the whole panel and park the RAM counter at the origin. Data entry
 * mode 0x03 means X increments then Y increments, so the framebuffer streams
 * out top-left to bottom-right in order. */
static void epd_set_full_window(void)
{
    epd_cmd(0x11);              /* data entry mode */
    epd_data1(0x03);

    epd_cmd(0x44);              /* RAM X start/end, in bytes */
    epd_data1(0x00);
    epd_data1(EPD_ROW_BYTES - 1);

    epd_cmd(0x45);              /* RAM Y start/end, in rows */
    epd_data1(0x00);
    epd_data1(0x00);
    epd_data1((EPD_H - 1) & 0xFF);
    epd_data1((EPD_H - 1) >> 8);

    epd_cmd(0x4E);              /* RAM X counter */
    epd_data1(0x00);

    epd_cmd(0x4F);              /* RAM Y counter */
    epd_data1(0x00);
    epd_data1(0x00);
}

static void epd_set_border(uint8_t value)
{
    epd_cmd(0x3C);
    epd_data1(value);
}

/* Stream a LUT_FULL/LUT_PARTIAL table: the 153-byte waveform (0x32) plus the
 * gate/source driving voltage and VCOM registers that follow it. */
static void epd_set_lut(const uint8_t *lut)
{
    epd_cmd(0x32);
    epd_data(lut, 153);
    epd_wait_busy(2000);

    epd_cmd(0x3F);              /* LUT end option */
    epd_data1(lut[153]);

    epd_cmd(0x03);              /* gate driving voltage */
    epd_data1(lut[154]);

    epd_cmd(0x04);              /* source driving voltage: VSH1, VSH2, VSL */
    epd_data(&lut[155], 3);

    epd_cmd(0x2C);              /* VCOM */
    epd_data1(lut[158]);
}

/* Hardware reset plus the register setup that's common to full and partial
 * refresh. Also used to bring the panel back out of deep sleep, which loses
 * all register state. */
static void epd_reset_and_common_init(void)
{
    epd_hw_reset();

    epd_cmd(0x12);              /* software reset */
    vTaskDelay(pdMS_TO_TICKS(10));
    epd_wait_busy(2000);

    epd_cmd(0x01);              /* driver output control: 200 gate lines */
    epd_data1((EPD_H - 1) & 0xFF);
    epd_data1((EPD_H - 1) >> 8);
    epd_data1(0x00);

    epd_cmd(0x11);              /* data entry mode */
    epd_data1(0x03);

    epd_set_full_window();

    epd_cmd(0x18);              /* use the built-in temperature sensor */
    epd_data1(0x80);
}

/* Load the temperature/waveform setting, then stream the matching custom LUT.
 * Mirrors Waveshare's reference driver; this panel needs it in place of the
 * SSD1681's built-in OTP default LUT. */
static void epd_load_lut_full(void)
{
    epd_set_border(0x01);

    epd_cmd(0x22);
    epd_data1(0xB1);
    epd_cmd(0x20);
    epd_wait_busy(2000);

    epd_set_lut(LUT_FULL);
}

static void epd_load_lut_partial(void)
{
    epd_set_lut(LUT_PARTIAL);

    epd_cmd(0x37);              /* display option */
    static const uint8_t opt[10] = {0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x40, 0x00, 0x00, 0x00, 0x00};
    epd_data(opt, sizeof(opt));

    epd_set_border(0x80);

    epd_cmd(0x22);
    epd_data1(0xC0);
    epd_cmd(0x20);
    epd_wait_busy(2000);
}

esp_err_t epd_init(const epd_pins_t *pins)
{
    s_pins = *pins;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << s_pins.dc) | (1ULL << s_pins.rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << s_pins.busy),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t bus = {
        .mosi_io_num = s_pins.mosi,
        .miso_io_num = -1,
        .sclk_io_num = s_pins.sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_SIZE + 16,
    };
    esp_err_t err = spi_bus_initialize(s_pins.host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = s_pins.clock_hz,
        .mode = 0,
        .spics_io_num = s_pins.cs,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(s_pins.host, &dev, &s_dev));

    epd_reset_and_common_init();
    epd_load_lut_full();
    epd_sleep();

    ESP_LOGI(TAG, "SSD1681 ready (%dx%d)", EPD_W, EPD_H);
    return ESP_OK;
}

void epd_display_full(const uint8_t *buf)
{
    epd_reset_and_common_init();
    epd_load_lut_full();

    epd_set_full_window();
    epd_cmd(0x24);              /* new image RAM */
    epd_data(buf, EPD_BUF_SIZE);

    epd_cmd(0x22);
    epd_data1(0xC7);
    epd_cmd(0x20);
    epd_wait_busy(30000);

    epd_sleep();
}

void epd_display_partial(const uint8_t *buf, const uint8_t *prev)
{
    epd_reset_and_common_init();
    epd_load_lut_partial();

    epd_set_full_window();
    epd_cmd(0x26);              /* previous image RAM, used as the difference base */
    epd_data(prev, EPD_BUF_SIZE);

    epd_set_full_window();
    epd_cmd(0x24);
    epd_data(buf, EPD_BUF_SIZE);

    epd_cmd(0x22);
    epd_data1(0xCF);
    epd_cmd(0x20);
    epd_wait_busy(5000);

    epd_sleep();
}

void epd_sleep(void)
{
    /* Mode 1 keeps RAM contents; both image RAMs are rewritten on every refresh
     * anyway, so nothing depends on that. */
    epd_cmd(0x10);
    epd_data1(0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
}
