// SPDX-License-Identifier: Apache-2.0
#include "hanzi_download.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#define HANZI_JSON_MAX_BYTES (128U * 1024U)
#define HANZI_JSON_INITIAL_BYTES 4096U
#define HANZI_HTTP_READ_BYTES 2048U
#define HANZI_HTTP_TIMEOUT_MS 15000
#define HANZI_DOWNLOAD_TIMEOUT_US (30LL * 1000 * 1000)

static const char *TAG = "hanzi_download";

bool hanzi_character_valid(const char *text)
{
    if (!text) {
        return false;
    }
    const unsigned char *s = (const unsigned char *)text;
    size_t length = strlen(text);
    uint32_t code;
    if (length == 3 && s[0] >= 0xe0 && s[0] <= 0xef &&
        (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        code = ((uint32_t)(s[0] & 0x0f) << 12) |
               ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        return (code >= 0x3400 && code <= 0x4dbf) ||
               (code >= 0x4e00 && code <= 0x9fff) ||
               (code >= 0xf900 && code <= 0xfaff);
    }
    if (length == 4 && s[0] >= 0xf0 && s[0] <= 0xf4 &&
        (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
        (s[3] & 0xc0) == 0x80) {
        code = ((uint32_t)(s[0] & 7) << 18) |
               ((uint32_t)(s[1] & 0x3f) << 12) |
               ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return (code >= 0x20000 && code <= 0x2ee5f) ||
               (code >= 0x2f800 && code <= 0x2fa1f) ||
               (code >= 0x30000 && code <= 0x3347f);
    }
    return false;
}

esp_err_t hanzi_download(const char *character, char **json)
{
    if (!json || !hanzi_character_valid(character)) {
        return ESP_ERR_INVALID_ARG;
    }
    *json = NULL;
    const char *base = CONFIG_HANZI_WRITER_DATA_BASE_URL;
    if (strncmp(base, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t base_length = strlen(base);
    char *url = malloc(base_length + 20);
    if (!url) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(url, base, base_length);
    size_t offset = base_length;
    url[offset++] = '/';
    for (const unsigned char *p = (const unsigned char *)character; *p; ++p) {
        offset += (size_t)sprintf(url + offset, "%%%02X", *p);
    }
    memcpy(url + offset, ".json", 6);
    ESP_LOGI(TAG, "GET %s", url);

    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = HANZI_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = client ? ESP_OK : ESP_ERR_NO_MEM;
    const int64_t deadline = esp_timer_get_time() + HANZI_DOWNLOAD_TIMEOUT_US;
    char *body = NULL;
    size_t capacity = 0;
    size_t received = 0;
    if (err == ESP_OK) {
        err = esp_http_client_open(client, 0);
    }
    if (err == ESP_OK) {
        int64_t length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 404) {
            err = ESP_ERR_NOT_FOUND;
        } else if (length < 0 || status != 200) {
            err = ESP_FAIL;
        } else if (length > HANZI_JSON_MAX_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            capacity = length > 0 ? (size_t)length : HANZI_JSON_INITIAL_BYTES;
            body = malloc(capacity + 1);
            if (!body) {
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    char chunk[HANZI_HTTP_READ_BYTES];
    while (err == ESP_OK) {
        if (esp_timer_get_time() >= deadline) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int count = esp_http_client_read(client, chunk, sizeof(chunk));
        if (count < 0) {
            err = ESP_FAIL;
            break;
        }
        if (count == 0) {
            break;
        }
        size_t needed = received + (size_t)count;
        if (needed > HANZI_JSON_MAX_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (needed > capacity) {
            size_t next_capacity = capacity * 2;
            if (next_capacity < needed) {
                next_capacity = needed;
            }
            if (next_capacity > HANZI_JSON_MAX_BYTES) {
                next_capacity = HANZI_JSON_MAX_BYTES;
            }
            char *grown = realloc(body, next_capacity + 1);
            if (!grown) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            body = grown;
            capacity = next_capacity;
        }
        memcpy(body + received, chunk, count);
        received += (size_t)count;
    }
    if (err == ESP_OK && (!received || !esp_http_client_is_complete_data_received(client))) {
        err = ESP_FAIL;
    }
    if (client) {
        esp_http_client_cleanup(client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(err));
    }
    free(url);
    if (err != ESP_OK) {
        free(body);
        return err;
    }
    body[received] = '\0';
    *json = body;
    return ESP_OK;
}
