// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lvgl.h"

typedef struct hanzi_view hanzi_view_t;
typedef void (*hanzi_view_exit_fn)(void *ctx);

/* Caller holds the display session lock. Deleting the parent releases the view,
 * its timer, parsed character, masks and canvas buffers. */
hanzi_view_t *hanzi_view_create(lv_obj_t *parent, const char *json,
                               hanzi_view_exit_fn on_exit, void *ctx,
                               char *error, size_t error_size);
