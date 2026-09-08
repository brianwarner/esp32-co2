# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF (v5.4) firmware for an ESP32-S3 SuperMini driving a Waveshare 1.54"
e-Paper Module V2 (200x200, SSD1681 controller) and a DFRobot Gravity STCC4
CO2 sensor (I2C, onboard SHT40 for temperature/humidity). It's a CO2 monitor:
the top half of the display shows the current CO2 reading (large), the time
(small), and temperature/humidity; the bottom half shows a 72-hour histogram
of CO2 history. Time comes from SNTP over Wi-Fi; timezone, 12/24-hour mode,
and the temperature unit are set in `.env` and baked into the binary at build
time.

## Commands

Compiling happens inside the `espressif/idf:v5.4` container (podman by
default, or `docker` via `CONTAINER_ENGINE=docker`) — no local ESP-IDF install
needed. Flashing and monitoring run on the host via `.venv` (esptool /
esp-idf-monitor), because podman on macOS runs containers in a VM with no
access to host USB serial devices. `build/` and `sdkconfig` persist on the
host between runs (the container just mounts the project directory).

```sh
cp .env.example .env && $EDITOR .env   # first-time setup; see Configuration below
make build           # compile only, in the container
make flash            # build, then write to the board (on the host)
make monitor           # open the serial console (Ctrl-] to exit)
make run              # flash then monitor
make env              # print what .env currently resolves to, without building
make preview          # render the clock face to preview.bmp, natively on this machine, no container/hardware
make preview ARGS='"2026-08-26 21:07"'   # preview a specific time
make menuconfig        # ESP-IDF config UI, in the container
make size              # binary size breakdown
make shell             # interactive shell in the build container
make erase             # erase entire flash (factory reset, on the host)
make factory-reset     # erase, then reflash
make clean / distclean # clean build output (distclean also removes sdkconfig, env_config.h)
make ports             # list candidate serial devices
```

There is no test suite or linter for the firmware itself — `make preview` (see
below) is the closest thing to a fast feedback loop for layout/rendering
logic. Python tooling in `.venv` is just esptool/esp-idf-monitor, not an
application to lint.

Override the serial port with `make flash PORT=/dev/cu.usbmodem1101` when
autodetection picks the wrong device or finds none (`make ports` lists
candidates).

### Known build gotcha

If a `make build`/`make flash` is interrupted (Ctrl-C) or the container build
crashes mid-way, `build/` can end up in a state `idf.py`'s own safety check
doesn't recognize as a valid CMake directory, and it will refuse to clean it
(`fullclean` no-ops with a warning instead of erroring). If a rebuild fails
immediately with something like "doesn't seem to be a CMake build directory",
`rm -rf build` and retry — it's build output, safe to discard.

## Configuration (`.env` -> `main/env_config.h`)

`scripts/gen_env_config.sh` reads `.env` and generates `main/env_config.h`,
which is `#include`d by `main/main.c`. This runs on every `make build`/`flash`
but only rewrites the header (triggering a recompile) when its contents
actually change. Values (Wi-Fi SSID/password, resolved TZ string, NTP server,
12/24-hour flag, temperature unit, CO2 sample interval) are compiled into the firmware — changing `.env` requires a
rebuild and reflash, there's no runtime config.

Timezone handling is the one subtle piece: newlib on the ESP32 has no
zoneinfo database, so the device needs a POSIX TZ string (e.g.
`EST5EDT,M3.2.0,M11.1.0`), not an IANA name. `gen_env_config.sh` resolves an
IANA `TIMEZONE` value by reading the POSIX footer out of the matching file in
the *build machine's* `/usr/share/zoneinfo` — so the resolved offset/DST rule
is a build-time snapshot, not something the device looks up itself. A literal
POSIX TZ string is also accepted directly (anything containing a digit passes
through unresolved).

## Architecture

The Makefile's `sdkconfig` target regenerates `sdkconfig` from
`sdkconfig.defaults` via `idf.py set-target` — this is order-only-gated on a
podman/docker reachability check (`check-podman`) so it doesn't spuriously
re-run on every build.

Firmware source (`main/`), in dependency order from most to least hardware-specific:

- **`main.c`** — GPIO pin assignments (see `docs/WIRING.md` before changing
  them), startup sequence, and the main loop. Owns two full-frame buffers,
  `s_front`/`s_back`: `s_back` is rendered into, then `present()` pushes it to
  the panel (full or partial refresh) and copies it into `s_front`, which
  serves as the diff base the *next* partial refresh needs. A full refresh
  runs periodically (`CFG_REFRESH_INTERVAL_MINUTES`/`CFG_REFRESH_ANCHOR_MINUTES`)
  instead of a partial one, to clear ghosting that partial refreshes
  accumulate; that one visibly flashes. Also owns the 72-hour CO2 history
  buffer (`s_history`, 144 samples at 30-minute resolution): `history_update`
  rolls the buckets forward and folds each new reading into the current
  (rightmost, still-filling) bucket as a running max. Sensor polling
  (`CFG_SAMPLE_INTERVAL_SECONDS`) runs on its own schedule from display
  refresh, waking the loop only when due; a repaint follows immediately
  whenever a new sample changes the displayed CO2 value, not just once a
  minute. The full-refresh schedule is still minute-granular and only
  evaluated on minute ticks, so a CO2-triggered repaint between ticks is
  always partial.

  The history buffer is persisted to flash (via `co2_history_store.c`)
  every time a bucket rolls (roughly every 30 minutes, to keep flash write
  wear low), so it survives a reboot. The ESP32 has no RTC, so until SNTP
  has synced there's no real clock to timestamp samples with: `app_main`
  samples the sensor during the wait too, but against a pseudo clock
  (`esp_timer_get_time`, monotonic since boot) anchored to resume right
  where the last save left off, on the assumption that the reboot took zero
  time. Once SNTP syncs, `history_reconcile` compares that assumption
  against the real time NTP just supplied; any gap becomes a real power
  outage duration, which it splices into the history as empty buckets so
  the histogram shows a gap instead of silently compressing the outage out
  of the timeline.
- **`net_time.c`** — Wi-Fi station mode + SNTP client, exposes connect/sync
  status that `main.c` polls (and displays via `clockface_status` while
  waiting).
- **`co2_history_store.c`** — loads/saves the CO2 history buffer as an NVS
  blob (namespace `co2hist`). No hardware dependencies beyond NVS itself.
- **`co2_stcc4.c`** — I2C driver for the STCC4 CO2 sensor, written against
  ESP-IDF's `driver/i2c_master.h`. Talks the sensor's word+CRC8 protocol
  directly (see the datasheet/Sensirion or DFRobot reference source if
  extending it) rather than porting the Arduino library. Init failure (no
  sensor wired) is non-fatal — `main.c` logs a warning and the clock keeps
  running without CO2 data.
- **`clockface.c`** — the 200x200 layout logic. Deliberately has *no* hardware
  dependencies, which is what lets `tools/preview.c` render it natively on
  the host (via `make preview`) without ESP-IDF or a board — the fastest way
  to iterate on layout/rendering changes. Top half renders the current
  reading (`co2_reading_t`); bottom half renders the CO2 history bar chart
  from the `history` array, auto-scaling its ceiling to a "nice" round number
  above the current peak.
- **`gfx.c`** — 1-bit framebuffer primitives (pixels, rects, text) that
  `clockface.c` draws through.
- **`font5x7.c`** / **`segdigits.c`** — bitmap font (ASCII 0x20-0x5F) and the
  custom seven-segment digit renderer used for the large CO2 number.
- **`epd_ssd1681.c`** — the SSD1681 SPI driver (init, full/partial refresh,
  sleep), written natively against ESP-IDF rather than using GxEPD2 (which
  requires `Arduino.h`/`Adafruit_GFX`). This panel needs Waveshare's
  reference LUT tables (`LUT_FULL`/`LUT_PARTIAL`, command `0x32`) instead of
  the SSD1681's built-in OTP default LUT, which renders image RAM as garbage
  on this panel — see `docs/WIRING.md#controller-notes`. The panel is put to
  sleep between refreshes.

`tools/preview.c` is compiled and linked directly against `clockface.c`,
`gfx.c`, `font5x7.c`, `segdigits.c` with the host's `cc` (see the `preview`
target in the Makefile) — no ESP-IDF component boundary applies to it, so
those four files must stay free of ESP-IDF/FreeRTOS includes.

## Conventions

- Use simple language. This is a hobby project and does not require marketing speak, superlatives, or for anybody to be sold on decisions.
- Be straightforward, use bulleted lists, and do not over-document in user-facing documentation. You can be as specific as you want in CLAUDE.md, but keep the agent context separate from user context.
- Use American english and avoid em and en dashes.
- When investigating the contents of a file at the beginning of a prompt, always look at the file on disk. Never rely on your memory of what a file contained or use git history unless you explicitly need to check the historical state.
- Under no circumstances should any script use absolute file paths for any tasks outside the current workspace. Absolute paths are only permitted in children of the folder containing this CLAUDE.md file.
- Do not perform any read/write git operations, including committing, moving, or removing. The user will manage git.
- Always preserve these files and directories: *.code-workspace, .gitignore, .vscode/
