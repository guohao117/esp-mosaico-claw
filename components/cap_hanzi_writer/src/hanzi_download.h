// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Accept exactly one UTF-8 Han ideograph, without surrounding prose. */
bool hanzi_character_valid(const char *text);
/* Fetch one JSON document. Caller frees *json on success. */
esp_err_t hanzi_download(const char *character, char **json);
