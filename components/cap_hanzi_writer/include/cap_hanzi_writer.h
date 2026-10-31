// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

/* Register the native stroke-order tools; call through app_claw's group registry.
 * UI and network work run on a dedicated worker, never on the Agent/LVGL task. */
esp_err_t cap_hanzi_writer_register_group(void);
