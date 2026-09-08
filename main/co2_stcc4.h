/* SPDX-License-Identifier: Apache-2.0 */
/* I2C driver for the Sensirion STCC4 CO2 sensor (DFRobot Gravity SEN0678).
 * The Gravity board carries an onboard SHT40, so every reading also carries
 * temperature and humidity with no external compensation input needed. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define STCC4_I2C_ADDR_PRIMARY   0x64 /* dip switch default */
#define STCC4_I2C_ADDR_SECONDARY 0x65

typedef struct {
    int sda;
    int scl;
    uint8_t i2c_addr;
    int clock_hz; /* STCC4 supports standard mode; 100 kHz is the safe default */
} co2_sensor_pins_t;

typedef struct {
    uint16_t co2_ppm;
    float temperature_c;
    float humidity_pct;
} stcc4_measurement_t;

/* Brings up the I2C bus, wakes the sensor, and starts continuous measurement.
 * Safe to treat a failure as non-fatal: the clock/display keep working, just
 * without CO2 data, if the sensor is missing or unwired. */
esp_err_t co2_sensor_init(const co2_sensor_pins_t *pins);

/* Reads the latest measurement. Returns false (leaving *out unchanged) if the
 * sensor was never initialized, isn't ready yet, or the read/CRC failed. */
bool co2_sensor_read(stcc4_measurement_t *out);
