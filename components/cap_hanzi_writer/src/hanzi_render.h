#ifndef HANZI_RENDER_H
#define HANZI_RENDER_H

#include "lvgl.h"
#include "hanzi.h"

typedef struct hw_renderer hw_renderer_t;

hw_renderer_t *hw_renderer_create(const hw_character_t *character, unsigned size,
                                   char *error, size_t error_size);
void hw_renderer_destroy(hw_renderer_t *renderer);
void hw_renderer_draw(hw_renderer_t *renderer, size_t completed, float progress,
                       bool guide, lv_draw_buf_t *output);

#endif
