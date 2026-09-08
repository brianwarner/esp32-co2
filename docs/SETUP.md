# Setup

## 1. Install podman and Python

Compiling runs inside the `espressif/idf` container image, so no local
ESP-IDF install is needed. Only [podman](https://podman.io) and the image are
required:

```sh
podman machine init   # first time only
podman machine start
```

Flashing and monitoring run on the host, not in the container, because
podman on macOS runs containers inside a VM with no access to host USB
serial devices. Those steps use `esptool` and `esp-idf-monitor`, listed in
`requirements.txt`:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

Use Docker instead of podman by passing `CONTAINER_ENGINE=docker` to `make`.

The container image is pinned by `IDF_IMAGE` in the Makefile (default
`espressif/idf:v5.4`). The first `make build` pulls it, several GB.

## 2. Wire the display and sensor

See [WIRING.md](WIRING.md).

## 3. Configure

```sh
cp .env.example .env
```

Edit `.env`:

| Key | Value |
| --------------------------- | ------------------------------------------------------------ |
| `WIFI_SSID` | 2.4 GHz network name. |
| `WIFI_PASSWORD` | Network password. Leave empty for an open network. |
| `WIFI_SSID_2` | 2.4 GHz network name (second network, optional) |
| `WIFI_PASSWORD_2` | Network password. Leave empty for an open network. |
| `WIFI_SSID_3` | 2.4 GHz network name (third network, optional) |
| `WIFI_PASSWORD_3` | Network password. Leave empty for an open network. |
| `TIMEZONE` | IANA name such as `America/New_York`, or a POSIX TZ string. |
| `HOURS` | `12` or `24`. Defaults to `24`. |
| `TEMP_UNIT` | `C` or `F`, for the temperature reading. Defaults to `F`. |
| `SAMPLE_INTERVAL_SECONDS` | How often to read the CO2 sensor, in seconds (1-1800). Defaults to `15`. |
| `NTP_SERVER` | Defaults to `pool.ntp.org` if omitted. |
| `REFRESH_INTERVAL_HOURS` | How often to do a full (flashing) refresh, in whole hours (1-24). Defaults to `1`. |
| `REFRESH_TIME` | Optional 24-hour `HH:MM` to anchor full refreshes to a fixed time of day instead of boot time. |
| `ELEVATED` / `ALERT` / `WARNING` | CO2 ppm thresholds for the RGB LED (must satisfy `ELEVATED < ALERT < WARNING`). Default `1000`/`2000`/`5000`. |

`scripts/gen_env_config.sh` reads `.env` on every build and writes
`main/env_config.h`. Values are compiled into the firmware, so changing `.env`
requires a rebuild and reflash (e.g., `make flash` or `make run`).

### Timezones

newlib on the ESP32 has no timezone database, so it needs a POSIX TZ string
such as `EST5EDT,M3.2.0,M11.1.0`. When `TIMEZONE` is an IANA name, the build
script resolves it by reading the POSIX footer out of the matching file in
`/usr/share/zoneinfo` on the build machine.

The resolved string includes the DST rules, so daylight saving transitions are
handled on-device. Rebuild and reflash if a government changes those rules.

A value that is neither a file in `/usr/share/zoneinfo` nor contains a digit is
rejected. `TIMEZONE=EDT` fails; `TIMEZONE=America/New_York` and
`TIMEZONE=EST5EDT,M3.2.0,M11.1.0` both work.

Check what a value resolves to:

```sh
make env
# env: TZ=America/New_York -> EST5EDT,M3.2.0,M11.1.0, 12-hour, temp=F, sample every 15s,
#      ssid=..., ntp=pool.ntp.org, full refresh every 1h from boot,
#      CO2 LED elevated=1000 alert=2000 warning=5000
```

## 4. Preview the layout (optional)

Renders the screen on the build machine, no hardware required:

```sh
make preview
```

Writes `preview.bmp` and prints ASCII art.

## 5. Build and flash

```sh
make flash
```

The first build pulls the `espressif/idf` container image and runs
`idf.py set-target esp32s3`, so it takes several minutes. Later builds only
recompile changed files.

Connect the SuperMini over USB-C. It enumerates as a USB Serial/JTAG device.
If the port is not detected automatically:

```sh
make ports
make flash PORT=/dev/tty.usbmodem####
```

If the board does not enter download mode on its own, hold BOOT, tap RESET,
release BOOT, then run `make flash`.

## 6. Watch the log

```sh
make monitor
```

Exit with `Ctrl-]`.

Expected output:

```bash
I clock TZ=EST5EDT,M3.2.0,M11.1.0, 12-hour clock, temp in F
I epd SSD1681 ready (200x200)
I co2 started continuous measurement on addr 0x64
I net connecting to 'yournetwork' (1/1)
I net got ip 192.168.1.42
I clock clock set from ntp
```

If the CO2 sensor isn't wired yet, expect `CO2 sensor not found, continuing without it`
instead of the `co2` line, and the display shows "NO SENSOR" with an empty
histogram.

## Factory reset

Erase the entire flash, including the NVS partition where Wi-Fi calibration
data is cached:

```sh
make erase
```

Erase and reflash in one step:

```sh
make factory-reset
```

## Other targets

| Target | Effect |
| --- | --- |
| `make build` | Compile, in the container |
| `make run` | Flash, then open the monitor |
| `make menuconfig` | ESP-IDF configuration UI, in the container |
| `make size` | Binary size breakdown, in the container |
| `make shell` | Interactive shell in the build container |
| `make clean` | Remove build output |
| `make distclean` | Remove build output, `sdkconfig`, generated header |

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Display stays blank | VCC on 3V3, GND connected, all six signal lines match `main/main.c` |
| `BUSY still asserted` in the log | BUSY wire, and that RST is connected. The panel holds BUSY high while refreshing. |
| Garbled or shifted image | Lower `SPI_CLOCK_HZ` in `main/main.c`; shorten the jumper wires. If the border ring toggles correctly but the image itself is garbled regardless of clock speed, see [WIRING.md](WIRING.md#controller-notes). Likely a controller LUT issue, not wiring |
| Clock is stuck at `--:--` | SSID and password in `.env`; the network must be 2.4 GHz; UDP port 123 must be reachable. After about a minute without a sync it shows CO2 readings anyway, just without a time |
| Time is off by a whole number of hours | `TIMEZONE` in `.env`, then rebuild and reflash |
| Faint ghost of the previous view | Normal for partial refresh; cleared by the periodic full refresh (`REFRESH_INTERVAL_HOURS`) |
| Display shows `NO SENSOR` or LED flashing purple | If you've just flashed the device, reboot it. Otherwise, may be CO2 sensor wiring (VCC/GND/SDA/SCL). |
| `Cannot reach podman` | Run `podman machine start` |
| `esptool not found in .venv` | Run `python3 -m venv .venv && .venv/bin/pip install -r requirements.txt` |
| `flash`/`monitor` cannot find the board | Run `make ports`, then pass `PORT=` explicitly; a container cannot reach USB serial devices |
