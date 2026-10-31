#include "hanzi_render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    hw_point_t point;
    float segment_length;
} brush_point_t;

typedef struct {
    unsigned x, y, w, h;
    uint8_t * alpha;
    brush_point_t * brush;
    size_t brush_count;
} stroke_mask_t;

struct hw_svg_renderer {
    const hw_character_t * character;
    unsigned size;
    float scale, padding;
    lv_obj_t * scratch;
    lv_draw_buf_t * scratch_buf;
    lv_draw_buf_t * base_buf;
    size_t base_completed;
    bool base_guide;
    stroke_mask_t masks[HW_MAX_STROKES];
};

static uint8_t alpha_at(const hw_svg_renderer_t * r, unsigned x, unsigned y)
{
    const lv_color32_t * row =
        (const lv_color32_t *)(r->scratch_buf->data + y * r->scratch_buf->header.stride);
    return row[x].alpha;
}

static lv_fpoint_t external_point(const hw_svg_renderer_t * r, hw_point_t p)
{
    lv_fpoint_t result = {
        r->padding + p.x * r->scale,
        r->padding + (900 - p.y) * r->scale
    };
    return result;
}

static hw_point_t extend(hw_point_t start, hw_point_t toward)
{
    float dx = start.x - toward.x;
    float dy = start.y - toward.y;
    float length = hypotf(dx, dy);
    hw_point_t result = {start.x + dx * 100 / length, start.y + dy * 100 / length};
    return result;
}

static bool prepare_brush(hw_svg_renderer_t * r, size_t index)
{
    const hw_stroke_t * stroke = &r->character->strokes[index];
    stroke_mask_t * mask = &r->masks[index];
    size_t first = 1;
    size_t last = stroke->median_count - 2;
    hw_point_t start = stroke->median[0];
    hw_point_t end = stroke->median[stroke->median_count - 1];

    while (first + 1 < stroke->median_count &&
          hypotf(stroke->median[first].x - start.x, stroke->median[first].y - start.y) < 0.01f) {
        ++first;
    }
    while (last > 0 &&
          hypotf(stroke->median[last].x - end.x, stroke->median[last].y - end.y) < 0.01f) {
        --last;
    }

    mask->brush_count = stroke->median_count + 2;
    mask->brush = malloc(mask->brush_count * sizeof(*mask->brush));
    if (!mask->brush) {
        return false;
    }

    mask->brush[0].point = extend(start, stroke->median[first]);
    mask->brush[0].segment_length = 0;
    for (size_t i = 0; i < stroke->median_count; ++i) {
        mask->brush[i + 1].point = stroke->median[i];
    }
    mask->brush[mask->brush_count - 1].point = extend(end, stroke->median[last]);
    for (size_t i = 1; i < mask->brush_count; ++i) {
        hw_point_t a = mask->brush[i - 1].point;
        hw_point_t b = mask->brush[i].point;
        mask->brush[i].segment_length = hypotf(b.x - a.x, b.y - a.y);
    }
    return true;
}

static bool rasterize(hw_svg_renderer_t * r, size_t index, char * error, size_t error_size)
{
    const char * path = r->character->strokes[index].path;
    size_t capacity = strlen(path) + 256;
    char * svg = malloc(capacity);
    if (!svg) {
        return false;
    }
    int length = snprintf(svg, capacity,
        "<svg width=\"%u\" height=\"%u\" viewBox=\"-32 -32 1088 1088\">"
        "<g transform=\"matrix(1 0 0 -1 0 900)\"><path fill=\"white\" d=\"%s\"/></g></svg>",
        r->size, r->size, path);
    lv_svg_node_t * doc = lv_svg_load_data(svg, (uint32_t)length);
    free(svg);
    if (!doc) {
        if (error && error_size) {
            snprintf(error, error_size, "LVGL could not parse stroke %lu SVG", (unsigned long)index + 1);
        }
        return false;
    }

    lv_canvas_fill_bg(r->scratch, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(r->scratch, &layer);
    lv_draw_svg(&layer, doc);
    lv_canvas_finish_layer(r->scratch, &layer);
    lv_svg_node_delete(doc);

    unsigned min_x = r->size;
    unsigned min_y = r->size;
    unsigned max_x = 0;
    unsigned max_y = 0;
    for (unsigned y = 0; y < r->size; ++y) {
        for (unsigned x = 0; x < r->size; ++x) {
            if (!alpha_at(r, x, y)) {
                continue;
            }
            if (x < min_x) {
                min_x = x;
            }
            if (y < min_y) {
                min_y = y;
            }
            if (x > max_x) {
                max_x = x;
            }
            if (y > max_y) {
                max_y = y;
            }
        }
    }
    if (min_x == r->size) {
        if (error && error_size) {
            snprintf(error, error_size, "Stroke %lu SVG rendered empty", (unsigned long)index + 1);
        }
        return false;
    }

    stroke_mask_t * mask = &r->masks[index];
    mask->x = min_x;
    mask->y = min_y;
    mask->w = max_x - min_x + 1;
    mask->h = max_y - min_y + 1;
    mask->alpha = malloc((size_t)mask->w * mask->h);
    if (!mask->alpha) {
        return false;
    }
    for (unsigned y = 0; y < mask->h; ++y) {
        for (unsigned x = 0; x < mask->w; ++x) {
            mask->alpha[y * mask->w + x] = alpha_at(r, mask->x + x, mask->y + y);
        }
    }
    return true;
}

hw_svg_renderer_t * hw_svg_renderer_create(lv_obj_t * parent, const hw_character_t * c,
                                          unsigned size, char * error, size_t error_size)
{
    if (error && error_size) {
        error[0] = 0;
    }
    hw_svg_renderer_t * r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->character = c;
    r->size = size;
    r->base_completed = SIZE_MAX;
    r->scale = (float)size / 1088;
    r->padding = 32 * r->scale;
    r->base_buf = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!r->base_buf) {
        goto bad;
    }
    r->scratch_buf = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (!r->scratch_buf) {
        goto bad;
    }
    r->scratch = lv_canvas_create(parent);
    if (!r->scratch) {
        goto bad;
    }
    lv_canvas_set_draw_buf(r->scratch, r->scratch_buf);
    lv_obj_add_flag(r->scratch, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < c->stroke_count; ++i) {
        if (!prepare_brush(r, i) || !rasterize(r, i, error, error_size)) {
            goto bad;
        }
    }
    return r;
bad:
    if (error && error_size && !error[0]) {
        snprintf(error, error_size, "Out of memory creating SVG renderer");
    }
    hw_svg_renderer_destroy(r);
    return NULL;
}

void hw_svg_renderer_destroy(hw_svg_renderer_t * r)
{
    if (!r) {
        return;
    }
    if (r->scratch) {
        lv_obj_delete(r->scratch);
    }
    if (r->scratch_buf) {
        lv_draw_buf_destroy(r->scratch_buf);
    }
    if (r->base_buf) {
        lv_draw_buf_destroy(r->base_buf);
    }
    for (size_t i = 0; i < HW_MAX_STROKES; ++i) {
        free(r->masks[i].alpha);
        free(r->masks[i].brush);
    }
    free(r);
}

static bool draw_brush(hw_svg_renderer_t * r, size_t stroke, float progress)
{
    const stroke_mask_t * mask = &r->masks[stroke];
    float remaining = (hw_stroke_length(&r->character->strokes[stroke]) + 200) * progress;
    lv_canvas_fill_bg(r->scratch, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(r->scratch, &layer);
    lv_vector_path_t * path = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_HIGH);
    lv_draw_vector_dsc_t * draw = lv_draw_vector_dsc_create(&layer);
    if (!path || !draw) {
        if (draw) {
            lv_draw_vector_dsc_delete(draw);
        }
        if (path) {
            lv_vector_path_delete(path);
        }
        lv_canvas_finish_layer(r->scratch, &layer);
        return false;
    }
    lv_fpoint_t p = external_point(r, mask->brush[0].point);
    lv_vector_path_move_to(path, &p);
    for (size_t i = 1; i < mask->brush_count && remaining > 0; ++i) {
        hw_point_t start = mask->brush[i - 1].point;
        hw_point_t end = mask->brush[i].point;
        float length = mask->brush[i].segment_length;
        if (length < 0.001f) {
            continue;
        }
        float fraction = fminf(1, remaining / length);
        hw_point_t point = {
            start.x + fraction * (end.x - start.x),
            start.y + fraction * (end.y - start.y)
        };
        p = external_point(r, point);
        lv_vector_path_line_to(path, &p);
        remaining -= length;
    }
    lv_draw_vector_dsc_set_fill_opa(draw, LV_OPA_TRANSP);
    lv_draw_vector_dsc_set_stroke_color(draw, lv_color_white());
    lv_draw_vector_dsc_set_stroke_opa(draw, LV_OPA_COVER);
    lv_draw_vector_dsc_set_stroke_width(draw, 200 * r->scale);
    lv_draw_vector_dsc_set_stroke_cap(draw, LV_VECTOR_STROKE_CAP_ROUND);
    lv_draw_vector_dsc_set_stroke_join(draw, LV_VECTOR_STROKE_JOIN_ROUND);
    lv_draw_vector_dsc_add_path(draw, path);
    lv_draw_vector(draw);
    lv_canvas_finish_layer(r->scratch, &layer);
    lv_draw_vector_dsc_delete(draw);
    lv_vector_path_delete(path);
    return true;
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 31));
}

static uint16_t blend(uint16_t dst, uint16_t src, unsigned alpha)
{
    unsigned inv = 255 - alpha;
    unsigned r = (((src >> 11) * alpha + (dst >> 11) * inv + 127) / 255) << 11;
    unsigned g = ((((src >> 5) & 63) * alpha + ((dst >> 5) & 63) * inv + 127) / 255) << 5;
    unsigned b = ((src & 31) * alpha + (dst & 31) * inv + 127) / 255;
    return (uint16_t)(r | g | b);
}

static void paint_mask(const hw_svg_renderer_t * r, size_t index, uint16_t color,
                       bool brush, lv_draw_buf_t * output)
{
    const stroke_mask_t * mask = &r->masks[index];
    for (unsigned y = 0; y < mask->h; ++y) {
        uint16_t * row = (uint16_t *)(output->data + (mask->y + y) * output->header.stride);
        for (unsigned x = 0; x < mask->w; ++x) {
            unsigned alpha = mask->alpha[y * mask->w + x];
            if (brush) {
                alpha = (alpha * alpha_at(r, mask->x + x, mask->y + y) + 127) / 255;
            }
            if (alpha) {
                row[mask->x + x] = blend(row[mask->x + x], color, alpha);
            }
        }
    }
}

static void draw_base(hw_svg_renderer_t * r, size_t completed, bool guide)
{
    const unsigned size = r->size;
    uint16_t grid = rgb565(0xe7e0d5);
    for (unsigned y = 0; y < size; ++y) {
        uint16_t * row = (uint16_t *)(r->base_buf->data + y * r->base_buf->header.stride);
        for (unsigned x = 0; x < size; ++x) {
            bool center = (x == size / 2 && (y / 5) % 2 == 0) ||
                          (y == size / 2 && (x / 5) % 2 == 0);
            bool diagonal = (x == y || x + y == size - 1) && (x / 5) % 2 == 0;
            row[x] = center || diagonal ? grid : rgb565(0xfffdf8);
        }
    }
    if (guide) {
        for (size_t i = 0; i < r->character->stroke_count; ++i) {
            paint_mask(r, i, rgb565(0xdedfd9), false, r->base_buf);
        }
    }
    for (size_t i = 0; i < completed; ++i) {
        paint_mask(r, i, rgb565(0x253b42), false, r->base_buf);
    }
    r->base_completed = completed;
    r->base_guide = guide;
}

bool hw_svg_renderer_draw(hw_svg_renderer_t * r, size_t completed, float progress,
                         bool guide, lv_draw_buf_t * output)
{
    if (r->base_completed != completed || r->base_guide != guide) {
        draw_base(r, completed, guide);
    }
    lv_draw_buf_copy(output, NULL, r->base_buf, NULL);
    if (completed < r->character->stroke_count && progress > 0) {
        if (progress < 1 && !draw_brush(r, completed, progress)) {
            return false;
        }
        paint_mask(r, completed, rgb565(0x168b78), progress < 1, output);
    }
    return true;
}
