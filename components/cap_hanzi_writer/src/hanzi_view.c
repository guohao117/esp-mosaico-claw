// SPDX-License-Identifier: Apache-2.0
#include "hanzi_view.h"
#include "hanzi_render.h"

#include <stdio.h>
#include <stdlib.h>

struct hanzi_view {
    lv_obj_t *root;
    lv_obj_t *canvas;
    lv_draw_buf_t *buffer;
    hw_character_t *character;
    hw_renderer_t *renderer;
    hw_player_t player;
    lv_timer_t *timer;
    uint32_t last_tick;
    hanzi_view_exit_fn on_exit;
    void *exit_ctx;
};

static void refresh(hanzi_view_t *view)
{
    hw_renderer_draw(view->renderer, view->player.completed,
                      view->player.progress, true, view->buffer);
    lv_draw_buf_flush_cache(view->buffer, NULL);
    lv_obj_invalidate(view->canvas);
}

static void clicked(lv_event_t *event)
{
    hanzi_view_t *view = lv_event_get_user_data(event);
    if (view->player.state == HW_FINISHED) {
        view->on_exit(view->exit_ctx);
    }
}

static void tick(lv_timer_t *timer)
{
    hanzi_view_t *view = lv_timer_get_user_data(timer);
    uint32_t now = lv_tick_get();
    hw_player_update(&view->player, now - view->last_tick);
    view->last_tick = now;
    refresh(view);
    if (view->player.state == HW_FINISHED) {
        lv_timer_delete(timer);
        view->timer = NULL;
        /* The final canvas is self-contained; replay controls do not exist. */
        hw_renderer_destroy(view->renderer);
        view->renderer = NULL;
        hw_character_destroy(view->character);
        view->character = NULL;
        view->player.character = NULL;
    }
}

static void free_view(hanzi_view_t *view)
{
    if (view->timer) {
        lv_timer_delete(view->timer);
    }
    hw_renderer_destroy(view->renderer);
    if (view->canvas) {
        lv_obj_delete(view->canvas);
    }
    if (view->buffer) {
        lv_draw_buf_destroy(view->buffer);
    }
    hw_character_destroy(view->character);
    free(view);
}

static void destroy(lv_event_t *event)
{
    free_view(lv_event_get_user_data(event));
}

hanzi_view_t *hanzi_view_create(lv_obj_t *parent, const char *json,
                               hanzi_view_exit_fn on_exit, void *ctx,
                               char *error, size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }
    hanzi_view_t *view = calloc(1, sizeof(*view));
    if (!view) {
        if (error && error_size) {
            snprintf(error, error_size, "Out of memory allocating Hanzi view");
        }
        return NULL;
    }
    view->on_exit = on_exit;
    view->exit_ctx = ctx;
    view->character = hw_character_parse(json, error, error_size);
    if (!view->character) {
        free(view);
        return NULL;
    }
    const char *failure = "Out of memory creating Hanzi root";
    view->root = lv_obj_create(parent);
    if (!view->root) {
        goto bad;
    }
    lv_obj_add_event_cb(view->root, destroy, LV_EVENT_DELETE, view);
    lv_obj_add_event_cb(view->root, clicked, LV_EVENT_CLICKED, view);
    lv_obj_remove_style_all(view->root);
    lv_obj_set_size(view->root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(view->root, lv_color_hex(0xf2f0e9), 0);
    lv_obj_set_style_bg_opa(view->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(view->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->root, LV_OBJ_FLAG_CLICKABLE);

    lv_display_t *display = lv_obj_get_display(parent);
    int width = lv_display_get_horizontal_resolution(display);
    int height = lv_display_get_vertical_resolution(display);
    int diameter = width < height ? width : height;
    /* Keep the centered canvas inside the circular screen. */
    unsigned size = (unsigned)(diameter * 70 / 100);
    failure = "Out of memory creating Hanzi draw buffer";
    view->buffer = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!view->buffer) {
        goto bad;
    }
    failure = "Out of memory creating Hanzi canvas";
    view->canvas = lv_canvas_create(view->root);
    if (!view->canvas) {
        goto bad;
    }
    lv_canvas_set_draw_buf(view->canvas, view->buffer);
    lv_obj_center(view->canvas);
    lv_obj_remove_flag(view->canvas, LV_OBJ_FLAG_CLICKABLE);
    failure = "Out of memory creating Hanzi renderer";
    view->renderer = hw_renderer_create(view->character, size, error, error_size);
    if (!view->renderer) {
        goto bad;
    }
    hw_player_init(&view->player, view->character);
    view->player.speed = 1.5f;
    hw_player_restart(&view->player, true);
    refresh(view);
    view->last_tick = lv_tick_get();
    failure = "Out of memory creating Hanzi animation timer";
    view->timer = lv_timer_create(tick, 33, view);
    if (!view->timer) {
        goto bad;
    }
    return view;
bad:
    if (error && error_size && !error[0]) {
        snprintf(error, error_size, "%s", failure);
    }
    if (view->root) {
        lv_obj_delete(view->root);
    } else {
        free_view(view);
    }
    return NULL;
}
