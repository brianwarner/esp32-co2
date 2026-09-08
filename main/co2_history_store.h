/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

#include "clockface.h"

/* Loads the last-saved 72h CO2 history and its bucket id from flash (NVS).
 * Returns false, leaving the outputs untouched, if nothing has been saved
 * yet (first boot, or the saved blob doesn't match this build). Caller must
 * have already initialized NVS (nvs_flash_init). */
bool co2_history_load(uint16_t history[CO2_HISTORY_LEN], long *bucket_epoch);

/* Saves the current history and bucket id to flash, overwriting any
 * previous save. Call this only on a bucket roll (roughly every 30
 * minutes), not on every sample, to keep flash write wear low. */
void co2_history_save(const uint16_t history[CO2_HISTORY_LEN], long bucket_epoch);
