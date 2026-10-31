// SPDX-License-Identifier: Apache-2.0
#include "cap_hanzi_writer.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "display_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "hanzi_download.h"
#include "hanzi_view.h"

/* TLS and initial SVG rasterization execute synchronously on this task. */
#define HANZI_WORKER_STACK 32768
#define HANZI_WORKER_PRIORITY 3
#define HANZI_COMMAND_QUEUE_DEPTH 4

typedef struct {
    char character[5];
    SemaphoreHandle_t done;
    esp_err_t result;
    char detail[128];
} hanzi_request_t;

typedef struct {
    hanzi_request_t *request;
    uint32_t generation;
} hanzi_command_t;

/* The process-lifetime worker owns the session/screen. UI callbacks only enqueue
 * exits. The display service pauses LVGL before invoking session_cleanup(). */
typedef struct {
    QueueHandle_t commands;
    display_service_session_handle_t session;
    lv_obj_t *screen;
    atomic_uint generation;
} hanzi_runtime_t;

static hanzi_runtime_t s_runtime;
static const char *TAG = "hanzi_writer";

static void request_exit(void *ctx)
{
    hanzi_runtime_t *runtime = ctx;
    const hanzi_command_t command = {.generation = atomic_load(&runtime->generation)};
    if (xQueueSend(runtime->commands, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Exit queue busy; try closing again");
    }
}

static void session_exit(display_service_session_handle_t session, void *ctx)
{
    (void)session;
    request_exit(ctx);
}

static void session_cleanup(display_service_session_handle_t session, void *ctx)
{
    (void)session;
    hanzi_runtime_t *runtime = ctx;
    if (runtime->screen) {
        lv_obj_delete(runtime->screen);
        runtime->screen = NULL;
    }
}

static esp_err_t stop_view(hanzi_runtime_t *runtime)
{
    if (!runtime->session) {
        return ESP_OK;
    }
    esp_err_t err = display_service_close(runtime->session);
    if (err == ESP_OK) {
        runtime->session = NULL;
    }
    return err;
}

static esp_err_t show_view(hanzi_runtime_t *runtime, hanzi_request_t *request)
{
    if (!runtime->session && display_service_has_exclusive_session()) {
        snprintf(request->detail, sizeof(request->detail),
                 "Display is busy. Close the current display app, then retry.");
        return ESP_ERR_INVALID_STATE;
    }
    char *json = NULL;
    esp_err_t err = hanzi_download(request->character, &json);
    if (err != ESP_OK) {
        snprintf(request->detail, sizeof(request->detail),
                 "Download failed from %s: %s",
                 CONFIG_HANZI_WRITER_DATA_BASE_URL, esp_err_to_name(err));
        return err;
    }

    err = stop_view(runtime);
    if (err != ESP_OK) {
        free(json);
        return err;
    }
    const display_service_session_config_t config = {
        .owner_name = "hanzi_writer",
        .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
        .flags = DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE |
                 DISPLAY_SERVICE_SESSION_FLAG_HIDE_HOME_INDICATOR,
        .cleanup_cb = session_cleanup,
        .exit_request_cb = session_exit,
        .user_ctx = runtime,
    };
    err = display_service_open(&config, &runtime->session);
    if (err != ESP_OK) {
        snprintf(request->detail, sizeof(request->detail),
                 "Opening LVGL display session failed: %s", esp_err_to_name(err));
    } else {
        err = display_service_session_lock(runtime->session);
        if (err != ESP_OK) {
            snprintf(request->detail, sizeof(request->detail),
                     "Locking LVGL display session failed: %s", esp_err_to_name(err));
        } else {
            atomic_fetch_add(&runtime->generation, 1);
            runtime->screen = lv_obj_create(NULL);
            if (!runtime->screen) {
                err = ESP_ERR_NO_MEM;
                snprintf(request->detail, sizeof(request->detail),
                         "Out of memory creating Hanzi screen");
            } else {
                lv_obj_set_style_pad_all(runtime->screen, 0, 0);
                lv_obj_remove_flag(runtime->screen, LV_OBJ_FLAG_SCROLLABLE);
                if (!hanzi_view_create(runtime->screen, json,
                                        request_exit, runtime,
                                        request->detail, sizeof(request->detail))) {
                    err = ESP_FAIL;
                } else {
                    err = display_service_session_load_screen_locked(runtime->session, runtime->screen);
                    if (err != ESP_OK) {
                        snprintf(request->detail, sizeof(request->detail),
                                 "Loading Hanzi screen failed: %s", esp_err_to_name(err));
                    }
                }
            }
            display_service_session_unlock(runtime->session);
        }
    }
    free(json);
    if (err != ESP_OK) {
        esp_err_t close_err = stop_view(runtime);
        if (close_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore display after initialization: %s", esp_err_to_name(close_err));
        }
    }
    return err;
}

static void worker(void *ctx)
{
    hanzi_runtime_t *runtime = ctx;
    hanzi_command_t command;
    for (;;) {
        if (xQueueReceive(runtime->commands, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!command.request) {
            if (command.generation == atomic_load(&runtime->generation)) {
                esp_err_t err = stop_view(runtime);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to close display: %s", esp_err_to_name(err));
                }
            }
            continue;
        }
        hanzi_request_t *request = command.request;
        request->result = request->character[0] ? show_view(runtime, request) : stop_view(runtime);
        xSemaphoreGive(request->done);
    }
}

static esp_err_t runtime_init(void)
{
    if (s_runtime.commands) {
        return ESP_OK;
    }
    s_runtime.commands = xQueueCreate(HANZI_COMMAND_QUEUE_DEPTH, sizeof(hanzi_command_t));
    if (!s_runtime.commands) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(worker, "hanzi_writer", HANZI_WORKER_STACK,
                           &s_runtime, HANZI_WORKER_PRIORITY, NULL,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(s_runtime.commands);
        s_runtime.commands = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t submit(hanzi_request_t *request)
{
    if (!s_runtime.commands) {
        return ESP_ERR_INVALID_STATE;
    }
    request->done = xSemaphoreCreateBinary();
    if (!request->done) {
        return ESP_ERR_NO_MEM;
    }
    const hanzi_command_t command = {.request = request};
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xQueueSend(s_runtime.commands, &command, 0) == pdTRUE) {
        /* The worker owns request until completion; all network I/O is bounded.
         * Keeping the caller alive also keeps this native tool's call lease. */
        xSemaphoreTake(request->done, portMAX_DELAY);
        err = request->result;
    }
    vSemaphoreDelete(request->done);
    return err;
}

static esp_err_t group_stop(void)
{
    hanzi_request_t *request = calloc(1, sizeof(*request));
    if (!request) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = submit(request);
    free(request);
    return err;
}

static esp_err_t execute(const char *input_json, const claw_cap_call_context_t *ctx,
                          char *output, size_t output_size, bool show)
{
    (void)ctx;
    if (!output || !output_size) {
        return ESP_ERR_INVALID_ARG;
    }
    hanzi_request_t *request = calloc(1, sizeof(*request));
    if (!request) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *input = cJSON_Parse(input_json ? input_json : "{}");
    esp_err_t err = cJSON_IsObject(input) ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (show && err == ESP_OK) {
        const cJSON *character = cJSON_GetObjectItemCaseSensitive(input, "character");
        if (!cJSON_IsString(character) || !hanzi_character_valid(character->valuestring)) {
            err = ESP_ERR_INVALID_ARG;
            snprintf(request->detail, sizeof(request->detail), "character must be exactly one UTF-8 Han ideograph.");
        } else {
            strcpy(request->character, character->valuestring);
        }
    }
    cJSON_Delete(input);
    if (err == ESP_OK) {
        err = submit(request);
    }
    if (err != ESP_OK && !request->detail[0]) {
        snprintf(request->detail, sizeof(request->detail), "%s", esp_err_to_name(err));
    }
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        free(request);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(result, "status", err == ESP_OK ? (show ? "started" : "closed") : "error");
    if (show) {
        cJSON_AddStringToObject(result, "character", request->character);
    }
    if (err != ESP_OK) {
        cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
        cJSON_AddStringToObject(result, "detail", request->detail);
    }
    bool rendered = cJSON_PrintPreallocated(result, output, (int)output_size, false);
    cJSON_Delete(result);
    free(request);
    return rendered ? err : ESP_ERR_INVALID_SIZE;
}

static esp_err_t show_execute(const char *json, const claw_cap_call_context_t *ctx,
                               char *output, size_t size)
{
    return execute(json, ctx, output, size, true);
}

static esp_err_t stop_execute(const char *json, const claw_cap_call_context_t *ctx,
                               char *output, size_t size)
{
    return execute(json, ctx, output, size, false);
}

static const claw_cap_descriptor_t s_tools[] = {
    {
        .id = "hanzi_writer_show",
        .name = "hanzi_writer_show",
        .family = "hanzi_writer",
        .description = "Play one Chinese character's stroke-order animation once at 1.5x speed on the device screen (stays visible until tapped to close). "
            "Extract the single target glyph before calling: "
            "\"永怎么写\"/\"教我写'永'\"->\"永\"; "
            "\"饕餮的饕怎么写\"/\"饕餮第一个字怎么写\"->\"饕\"; "
            "\"饕餮的餮怎么写\"->\"餮\"; "
            "\"什么的什怎么写\"->\"什\" (not \"什么\"); "
            "\"的怎么写\"->\"的\". "
            "Preserve the user's exact simplified/traditional glyph (\"龍怎么写\"->\"龍\"). "
            "If ambiguous (only pinyin, or a multi-character word like \"饕餮怎么写\" without specifying which character), ask which single character to show first instead of guessing or calling multiple times. "
            "Say it is playing only when status is \"started\"; never fabricate stroke data or Lua drawing code.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
            "\"character\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1,\"description\":\"Exactly one Chinese character extracted from the user's request (e.g. 永, 饕, 餮, 什, 的, 龍).\"}},"
            "\"required\":[\"character\"],\"additionalProperties\":false}",
        .execute = show_execute,
    },
    {
        .id = "hanzi_writer_stop",
        .name = "hanzi_writer_stop",
        .family = "hanzi_writer",
        .description = "Close the Hanzi Writer lesson and restore the previous device UI. Does not close another app's display session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        .execute = stop_execute,
    },
};

esp_err_t cap_hanzi_writer_register_group(void)
{
    static const claw_cap_group_t group = {
        .group_id = "cap_hanzi_writer",
        .plugin_name = "Hanzi Writer",
        .version = "1.0.0",
        .descriptors = s_tools,
        .descriptor_count = sizeof(s_tools) / sizeof(s_tools[0]),
        .group_init = runtime_init,
        .group_stop = group_stop,
    };
    return claw_cap_register_group(&group);
}
