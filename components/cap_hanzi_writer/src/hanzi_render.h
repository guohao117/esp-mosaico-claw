#ifndef HANZI_RENDER_H
#define HANZI_RENDER_H

#include "lvgl.h"
#include "hanzi.h"

typedef struct hw_svg_renderer hw_svg_renderer_t;

hw_svg_renderer_t * hw_svg_renderer_create(lv_obj_t * parent, const hw_character_t * character,
                                          unsigned size, char * error, size_t error_size);
void hw_svg_renderer_destroy(hw_svg_renderer_t * renderer);
bool hw_svg_renderer_draw(hw_svg_renderer_t * renderer, size_t completed, float progress,
                          bool guide, lv_draw_buf_t * output);

#endif
