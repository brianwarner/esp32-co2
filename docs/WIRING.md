# Wiring

## Parts

- ESP32-S3 SuperMini (ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM)
- Waveshare 1.54 inch e-Paper Module V2, 200x200, SSD1681 controller
- 8-pin cable supplied with the display
- DFRobot Gravity STCC4 CO2 sensor (SEN0678), with an onboard SHT40 for
  temperature/humidity

## Display connections

| Display pin | Cable colour | ESP32-S3 SuperMini | Direction        |
| ----------- | ------------ | ------------------ | ---------------- |
| VCC         | grey         | 3V3                | power            |
| GND         | brown        | GND                | ground           |
| DIN         | blue         | GPIO 13            | ESP32 -> display |
| CLK         | yellow       | GPIO 12            | ESP32 -> display |
| CS          | orange       | GPIO 11            | ESP32 -> display |
| DC          | green        | GPIO 10            | ESP32 -> display |
| RST         | white        | GPIO 9             | ESP32 -> display |
| BUSY        | purple       | GPIO 8             | display -> ESP32 |

Check the colours against the silkscreen on the display board before wiring;
Waveshare have used more than one cable.

The module accepts 3.3 V or 5 V on VCC. Use 3V3, because the logic lines are
driven at 3.3 V by the ESP32.

## CO2 sensor connections

| Sensor pin | ESP32-S3 SuperMini | Direction       |
| ---------- | ------------------ | --------------- |
| VCC        | 3V3 (or 5V)        | power           |
| GND        | GND                | ground          |
| SDA        | GPIO 1             | bidirectional   |
| SCL        | GPIO 2             | ESP32 -> sensor |

The Gravity board carries its own I2C pull-up resistors, so no external
pull-ups are needed. The sensor's I2C address is selectable with an onboard
dip switch (`0x64` default, `0x65` alternate), for running more than one
sensor on the same bus; `main/main.c` assumes `0x64`
(`STCC4_I2C_ADDR_PRIMARY`).

## Changing pins

The pin numbers are defined at the top of [`main/main.c`](../main/main.c):

```c
#define PIN_BUSY 8
#define PIN_RST  9
#define PIN_DC   10
#define PIN_CS   11
#define PIN_CLK  12
#define PIN_DIN  13

#define PIN_I2C_SDA 1
#define PIN_I2C_SCL 2

#define PIN_RGB_LED 48
```

The ESP32-S3 routes SPI and I2C through the GPIO matrix, so any free GPIO works. Avoid:

| GPIO | Reason |
| --- | --- |
| 0, 45, 46 | Strapping pins, sampled at reset |
| 19, 20 | Native USB D-/D+, used by the USB console |
| 26-32 | SPI flash and PSRAM |
| 43, 44 | UART0 TX/RX |
| 48 | On-board RGB LED on most SuperMini boards |

GPIO 4-9 are on one side of the SuperMini header and are free on all variants,
which is why they are the display's defaults here. GPIO 1-2 (used for the CO2
sensor's I2C bus) are on the other side and free as well.

## Notes

- SPI runs at 1 MHz (`SPI_CLOCK_HZ` in `main/main.c`). Long or unshielded
  jumper wires may need a lower value.
- Only MOSI is wired. The display has no MISO line.
- BUSY is an input with no pull resistor. The panel drives it high while it is
  refreshing.

## Controller notes

This panel needs Waveshare's reference LUT tables instead of the SSD1681's
built-in OTP default LUT: the OTP default renders image RAM as garbage on
this panel. `main/epd_ssd1681.c` loads `LUT_FULL`/`LUT_PARTIAL` via command
`0x32`, plus the gate/source driving voltage (`0x03`/`0x04`) and VCOM
(`0x2C`) registers that ship with them, and triggers updates with control
words `0xC7` (full) / `0xCF` (partial). See
`epd_load_lut_full`/`epd_load_lut_partial` in that file.

If you swap in a different panel revision and see register-driven behavior
work (border ring, BUSY timing) while the image itself comes out garbled
regardless of SPI clock speed, that's the same class of bug: the built-in
OTP LUT not being right for that unit.

## Sensor notes

`main/co2_stcc4.c` puts the STCC4 into continuous measurement mode at boot
and polls it every `SAMPLE_INTERVAL_SECONDS` (`.env`, default 15s) — the
display itself still only repaints once a minute. The sensor's CO2 reading
takes about 20 seconds
to stabilize after power-up; this happens in the background while the board
is connecting to Wi-Fi and syncing NTP, so it's usually settled by the time
the first reading is displayed. Each reading includes a CRC-8 checksum;
`co2_sensor_read` discards a reading that fails the check rather than
displaying bad data.

If the sensor is unplugged or unresponsive at boot, `co2_sensor_init` fails
and `main.c` logs a warning and carries on. The display falls back to a
"NO SENSOR" reading and an empty history chart instead of failing to start.
