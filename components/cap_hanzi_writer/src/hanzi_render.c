// SPDX-License-Identifier: Apache-2.0
#include "hanzi_render.h"
#include "hanzi_path.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    hw_point_t point;
    float segment_length;
} brush_point_t;

typedef struct {
    hw_path_mask_t mask;
    brush_point_t *brush;
    size_t brush_count;
} stroke_t;

struct hw_renderer {
    const hw_character_t *character;
    float scale;
    lv_draw_buf_t *base_buf;
    uint8_t *brush_alpha;
    size_t base_completed;
    bool base_guide;
    stroke_t strokes[HW_MAX_STROKES];
};

static hw_point_t external_point(const hw_renderer_t *renderer, hw_point_t point)
{
    return (hw_point_t){(point.x + 32) * renderer->scale,
                        (932 - point.y) * renderer->scale};
}

static hw_point_t extend(hw_point_t start, hw_point_t toward)
{
    float dx = start.x - toward.x;
    float dy = start.y - toward.y;
    float length = hypotf(dx, dy);
    return (hw_point_t){start.x + dx * 100 / length, start.y + dy * 100 / length};
}

static bool prepare_brush(hw_renderer_t *renderer, size_t index)
{
    const hw_stroke_t *stroke = &renderer->character->strokes[index];
    stroke_t *prepared = &renderer->strokes[index];
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
    prepared->brush_count = stroke->median_count + 2;
    prepared->brush = malloc(prepared->brush_count * sizeof(*prepared->brush));
    if (!prepared->brush) {
        return false;
    }
    prepared->brush[0].point = extend(start, stroke->median[first]);
    prepared->brush[0].segment_length = 0;
    for (size_t i = 0; i < stroke->median_count; ++i) {
        prepared->brush[i + 1].point = stroke->median[i];
    }
    prepared->brush[prepared->brush_count - 1].point = extend(end, stroke->median[last]);
    for (size_t i = 1; i < prepared->brush_count; ++i) {
        hw_point_t a = prepared->brush[i - 1].point;
        hw_point_t b = prepared->brush[i].point;
        prepared->brush[i].segment_length = hypotf(b.x - a.x, b.y - a.y);
    }
    return true;
}

hw_renderer_t *hw_renderer_create(const hw_character_t *character, unsigned size,
                                   char *error, size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }
    hw_renderer_t *renderer = calloc(1, sizeof(*renderer));
    if (!renderer) {
        if (error && error_size) {
            snprintf(error, error_size, "Out of memory creating Hanzi renderer");
        }
        return NULL;
    }
    renderer->character = character;
    renderer->scale = (float)size / 1088;
    renderer->base_completed = SIZE_MAX;
    renderer->base_buf = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!renderer->base_buf) {
        goto bad;
    }
    size_t brush_capacity = 0;
    for (size_t i = 0; i < character->stroke_count; ++i) {
        stroke_t *stroke = &renderer->strokes[i];
        if (!prepare_brush(renderer, i) ||
            !hw_path_rasterize(character->strokes[i].path, size, &stroke->mask, error, error_size)) {
            goto bad;
        }
        size_t area = (size_t)stroke->mask.w * stroke->mask.h;
        if (area > brush_capacity) {
            brush_capacity = area;
        }
    }
    renderer->brush_alpha = malloc(brush_capacity);
    if (!renderer->brush_alpha) {
        goto bad;
    }
    return renderer;
bad:
    if (error && error_size && !error[0]) {
        snprintf(error, error_size, "Out of memory preparing Hanzi strokes");
    }
    hw_renderer_destroy(renderer);
    return NULL;
}

void hw_renderer_destroy(hw_renderer_t *renderer)
{
    if (!renderer) {
        return;
    }
    if (renderer->base_buf) {
        lv_draw_buf_destroy(renderer->base_buf);
    }
    free(renderer->brush_alpha);
    for (size_t i = 0; i < HW_MAX_STROKES; ++i) {
        free(renderer->strokes[i].mask.alpha);
        free(renderer->strokes[i].brush);
    }
    free(renderer);
}

/* A union of capsules gives the median brush round caps and round joins. */
static void draw_capsule(hw_renderer_t *renderer, const hw_path_mask_t *mask,
                          hw_point_t a, hw_point_t b)
{
    float radius = 100 * renderer->scale;
    float outer = radius + 0.5f;
    float inner_squared = (radius - 0.5f) * (radius - 0.5f);
    float outer_squared = outer * outer;
    int left = (int)fmaxf((float)mask->x, floorf(fminf(a.x, b.x) - outer));
    int top = (int)fmaxf((float)mask->y, floorf(fminf(a.y, b.y) - outer));
    int right = (int)fminf((float)(mask->x + mask->w), ceilf(fmaxf(a.x, b.x) + outer));
    int bottom = (int)fminf((float)(mask->y + mask->h), ceilf(fmaxf(a.y, b.y) + outer));
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float length_squared = dx * dx + dy * dy;
    float inverse_length = length_squared > 0 ? 1 / length_squared : 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            size_t offset = (size_t)(y - mask->y) * mask->w + x - mask->x;
            if (!mask->alpha[offset] || renderer->brush_alpha[offset] == 255) {
                continue;
            }
            float px = x + 0.5f - a.x;
            float py = y + 0.5f - a.y;
            float projection = fmaxf(0, fminf(1, (px * dx + py * dy) * inverse_length));
            px -= projection * dx;
            py -= projection * dy;
            float distance_squared = px * px + py * py;
            if (distance_squared >= outer_squared) {
                continue;
            }
            uint8_t alpha = distance_squared <= inner_squared ? 255 :
                (uint8_t)((outer - sqrtf(distance_squared)) * 255 + 0.5f);
            if (alpha > renderer->brush_alpha[offset]) {
                renderer->brush_alpha[offset] = alpha;
            }
        }
    }
}

static void draw_brush(hw_renderer_t *renderer, size_t index, float progress)
{
    const stroke_t *stroke = &renderer->strokes[index];
    memset(renderer->brush_alpha, 0, (size_t)stroke->mask.w * stroke->mask.h);
    float remaining = (hw_stroke_length(&renderer->character->strokes[index]) + 200) * progress;
    for (size_t i = 1; i < stroke->brush_count && remaining > 0; ++i) {
        hw_point_t start = stroke->brush[i - 1].point;
        hw_point_t end = stroke->brush[i].point;
        float length = stroke->brush[i].segment_length;
        if (length < 0.001f) {
            continue;
        }
        float fraction = fminf(1, remaining / length);
        hw_point_t point = {start.x + fraction * (end.x - start.x),
                            start.y + fraction * (end.y - start.y)};
        draw_capsule(renderer, &stroke->mask, external_point(renderer, start),
                      external_point(renderer, point));
        remaining -= length;
    }
}

static uint16_t rgb565(uint32_t color)
{
    return (uint16_t)(((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 31));
}

static uint16_t blend(uint16_t dst, uint16_t src, unsigned alpha)
{
    unsigned inv = 255 - alpha;
    unsigned r = (((src >> 11) * alpha + (dst >> 11) * inv + 127) / 255) << 11;
    unsigned g = ((((src >> 5) & 63) * alpha + ((dst >> 5) & 63) * inv + 127) / 255) << 5;
    unsigned b = ((src & 31) * alpha + (dst & 31) * inv + 127) / 255;
    return (uint16_t)(r | g | b);
}

static void paint_mask(const hw_renderer_t *renderer, size_t index, uint16_t color,
                        bool brush, lv_draw_buf_t *output)
{
    const hw_path_mask_t *mask = &renderer->strokes[index].mask;
    for (unsigned y = 0; y < mask->h; ++y) {
        uint16_t *row = (uint16_t *)(output->data + (mask->y + y) * output->header.stride);
        for (unsigned x = 0; x < mask->w; ++x) {
            size_t offset = (size_t)y * mask->w + x;
            unsigned alpha = mask->alpha[offset];
            if (brush) {
                alpha = (alpha * renderer->brush_alpha[offset] + 127) / 255;
            }
            if (alpha) {
                row[mask->x + x] = blend(row[mask->x + x], color, alpha);
            }
        }
    }
}

static void draw_base(hw_renderer_t *renderer, size_t completed, bool guide)
{
    const unsigned size = renderer->base_buf->header.w;
    uint16_t grid = rgb565(0xe7e0d5);
    for (unsigned y = 0; y < size; ++y) {
        uint16_t *row = (uint16_t *)(renderer->base_buf->data + y * renderer->base_buf->header.stride);
        for (unsigned x = 0; x < size; ++x) {
            bool center = (x == size / 2 && (y / 5) % 2 == 0) ||
                          (y == size / 2 && (x / 5) % 2 == 0);
            bool diagonal = (x == y || x + y == size - 1) && (x / 5) % 2 == 0;
            row[x] = center || diagonal ? grid : rgb565(0xfffdf8);
        }
    }
    if (guide) {
        for (size_t i = 0; i < renderer->character->stroke_count; ++i) {
            paint_mask(renderer, i, rgb565(0xdedfd9), false, renderer->base_buf);
        }
    }
    for (size_t i = 0; i < completed; ++i) {
        paint_mask(renderer, i, rgb565(0x253b42), false, renderer->base_buf);
    }
    renderer->base_completed = completed;
    renderer->base_guide = guide;
}

void hw_renderer_draw(hw_renderer_t *renderer, size_t completed, float progress,
                       bool guide, lv_draw_buf_t *output)
{
    if (renderer->base_completed != completed || renderer->base_guide != guide) {
        draw_base(renderer, completed, guide);
    }
    lv_draw_buf_copy(output, NULL, renderer->base_buf, NULL);
    if (completed < renderer->character->stroke_count && progress > 0) {
        if (progress < 1) {
            draw_brush(renderer, completed, progress);
        }
        paint_mask(renderer, completed, rgb565(0x168b78), progress < 1, output);
    }
}
