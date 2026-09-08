# SPDX-License-Identifier: Apache-2.0
# Container-based build for the e-paper clock firmware. Compiling runs inside
# the espressif/idf podman image, so no local ESP-IDF install is needed.
# Flashing and monitoring run on the host via the esptool/esp-idf-monitor
# already in .venv (see requirements.txt), because podman on macOS runs
# containers inside a VM with no access to host USB serial devices.
#
#   make build          compile (in the container)
#   make flash          compile, then write to the board (on the host)
#   make monitor        open the serial console (ctrl-] to exit)
#   make run            flash then monitor
#   make erase          erase the whole flash (factory reset)
#   make factory-reset  erase, then reflash the firmware
#   make preview        render the clock face to preview.bmp on this machine
#   make shell          interactive shell in the build container
#
# Override the serial port with:  make flash PORT=/dev/cu.usbmodem1101
# Override the container engine with:  make build CONTAINER_ENGINE=docker

SHELL := /bin/bash

ENV_FILE   := .env
ENV_HEADER := main/env_config.h
GEN        := scripts/gen_env_config.sh

PROJECT_NAME := epaper-clock
BUILD_DIR     := build
ELF           := $(BUILD_DIR)/$(PROJECT_NAME).elf
FLASH_ARGS    := $(BUILD_DIR)/flash_args

CONTAINER_ENGINE ?= podman
IDF_IMAGE         ?= espressif/idf:v5.4
IDF_TARGET        ?= esp32s3

VENV       := .venv
ESPTOOL    := $(VENV)/bin/esptool
IDF_MONITOR := $(VENV)/bin/idf-monitor

BAUD ?= 460800
PORT ?=

PORT_ARG := $(if $(PORT),-p $(PORT),)

# --rm mounts the project directory into the container and runs idf.py there;
# state (build/, sdkconfig) persists on the host between runs. -it is only
# needed for interactive commands (menuconfig, shell).
CRUN    = $(CONTAINER_ENGINE) run --rm -v "$(CURDIR)":/project -w /project $(IDF_IMAGE)
CRUN_IT = $(CONTAINER_ENGINE) run --rm -it -v "$(CURDIR)":/project -w /project $(IDF_IMAGE)

.PHONY: all env build flash monitor run erase factory-reset \
        clean distclean menuconfig size ports shell \
        check-podman check-venv preview help

all: build

help:
	@sed -n '3,18p' $(MAKEFILE_LIST) | sed 's/^# \{0,1\}//'

check-podman:
	@command -v $(CONTAINER_ENGINE) >/dev/null 2>&1 || { \
	  echo "$(CONTAINER_ENGINE) not found. Install it, or pass CONTAINER_ENGINE=docker."; exit 1; }
	@$(CONTAINER_ENGINE) info >/dev/null 2>&1 || { \
	  echo "Cannot reach $(CONTAINER_ENGINE). On macOS, run: podman machine start"; exit 1; }

check-venv:
	@test -x $(ESPTOOL) || { \
	  echo "esptool not found in $(VENV). Set it up with:"; \
	  echo "  python3 -m venv $(VENV) && $(VENV)/bin/pip install -r requirements.txt"; exit 1; }

env: $(ENV_HEADER)

$(ENV_HEADER): $(ENV_FILE) $(GEN)
	@$(GEN) $(ENV_FILE) $(ENV_HEADER)

sdkconfig: sdkconfig.defaults | check-podman
	@$(CRUN) idf.py set-target $(IDF_TARGET)

build: check-podman env sdkconfig
	@$(CRUN) idf.py build

# flash_args lists binaries with paths relative to build/, so run from there.
# idf.py (v5.4) generates it with esptool's old underscore flag names
# (--flash_mode etc.) and the old write_flash command name; translate both to
# the hyphenated forms the .venv esptool (5.x) expects, to avoid deprecation
# warnings.
flash: build check-venv
	@cd $(BUILD_DIR) && "$(CURDIR)/$(ESPTOOL)" --chip $(IDF_TARGET) $(PORT_ARG) --baud $(BAUD) \
	  write-flash $$(sed -e 's/--flash_mode/--flash-mode/' -e 's/--flash_freq/--flash-freq/' -e 's/--flash_size/--flash-size/' flash_args)

monitor: check-venv
	@$(IDF_MONITOR) $(PORT_ARG) $(ELF)

run: flash monitor

erase: check-venv
	@$(ESPTOOL) --chip $(IDF_TARGET) $(PORT_ARG) erase_flash

factory-reset:
	@$(MAKE) erase
	@$(MAKE) flash

menuconfig: check-podman env
	@$(CRUN_IT) idf.py menuconfig

size: check-podman
	@$(CRUN) idf.py size

shell: check-podman
	@$(CRUN_IT) bash

clean: check-podman
	@$(CRUN) idf.py clean

distclean: check-podman
	@$(CRUN) idf.py fullclean
	@rm -f sdkconfig sdkconfig.old $(ENV_HEADER) build-preview preview.bmp

ports:
	@ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null \
	  || echo "no serial ports found"

# ---- Host-side layout preview (no ESP-IDF, container, or hardware needed) ----
PREVIEW_SRC := tools/preview.c main/clockface.c main/gfx.c main/font5x7.c main/segdigits.c

.PHONY: preview
preview: $(ENV_HEADER)
	@cc -std=c11 -D_GNU_SOURCE -Imain -o build-preview $(PREVIEW_SRC) -lm
	@./build-preview $(ARGS)
