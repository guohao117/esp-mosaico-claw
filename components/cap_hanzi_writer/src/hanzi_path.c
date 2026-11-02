// SPDX-License-Identifier: Apache-2.0
#include "hanzi_path.h"
#include "hanzi.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CURVE_TOLERANCE 0.2f
#define CURVE_MAX_DEPTH 12
#define SCAN_SAMPLES 4

typedef struct {
    float x, y_min, y_max, dx_dy;
    int winding;
} path_edge_t;

typedef struct {
    path_edge_t *edges;
    size_t count, capacity;
    float min_x, min_y, max_x, max_y;
} outline_t;

typedef struct {
    float x;
    int winding;
} crossing_t;

static hw_point_t midpoint(hw_point_t a, hw_point_t b)
{
    return (hw_point_t){(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
}

static bool add_edge(outline_t *outline, hw_point_t a, hw_point_t b)
{
    outline->min_x = fminf(outline->min_x, fminf(a.x, b.x));
    outline->min_y = fminf(outline->min_y, fminf(a.y, b.y));
    outline->max_x = fmaxf(outline->max_x, fmaxf(a.x, b.x));
    outline->max_y = fmaxf(outline->max_y, fmaxf(a.y, b.y));
    if (a.y == b.y) {
        return true;
    }
    if (outline->count == outline->capacity) {
        size_t capacity = outline->capacity ? outline->capacity * 2 : 64;
        path_edge_t *edges = realloc(outline->edges, capacity * sizeof(*edges));
        if (!edges) {
            return false;
        }
        outline->edges = edges;
        outline->capacity = capacity;
    }
    path_edge_t *edge = &outline->edges[outline->count++];
    edge->winding = b.y > a.y ? 1 : -1;
    edge->dx_dy = (b.x - a.x) / (b.y - a.y);
    edge->x = b.y > a.y ? a.x : b.x;
    edge->y_min = fminf(a.y, b.y);
    edge->y_max = fmaxf(a.y, b.y);
    return true;
}

static bool control_is_flat(hw_point_t start, hw_point_t control, hw_point_t end)
{
    float dx = end.x - start.x;
    float dy = end.y - start.y;
    float cx = control.x - start.x;
    float cy = control.y - start.y;
    float length_squared = dx * dx + dy * dy;
    if (length_squared == 0) {
        return cx * cx + cy * cy <= CURVE_TOLERANCE * CURVE_TOLERANCE;
    }
    float projection = (cx * dx + cy * dy) / length_squared;
    projection = fmaxf(0, fminf(1, projection));
    cx -= projection * dx;
    cy -= projection * dy;
    return cx * cx + cy * cy <= CURVE_TOLERANCE * CURVE_TOLERANCE;
}

static bool quadratic(outline_t *outline, hw_point_t a, hw_point_t b,
                       hw_point_t c, unsigned depth)
{
    if (depth == CURVE_MAX_DEPTH || control_is_flat(a, b, c)) {
        return add_edge(outline, a, c);
    }
    hw_point_t ab = midpoint(a, b);
    hw_point_t bc = midpoint(b, c);
    hw_point_t middle = midpoint(ab, bc);
    return quadratic(outline, a, ab, middle, depth + 1) &&
           quadratic(outline, middle, bc, c, depth + 1);
}

static bool cubic(outline_t *outline, hw_point_t a, hw_point_t b,
                   hw_point_t c, hw_point_t d, unsigned depth)
{
    if (depth == CURVE_MAX_DEPTH ||
        (control_is_flat(a, b, d) && control_is_flat(a, c, d))) {
        return add_edge(outline, a, d);
    }
    hw_point_t ab = midpoint(a, b);
    hw_point_t bc = midpoint(b, c);
    hw_point_t cd = midpoint(c, d);
    hw_point_t abc = midpoint(ab, bc);
    hw_point_t bcd = midpoint(bc, cd);
    hw_point_t middle = midpoint(abc, bcd);
    return cubic(outline, a, ab, abc, middle, depth + 1) &&
           cubic(outline, middle, bcd, cd, d, depth + 1);
}

static void skip_separators(const char **cursor)
{
    while (isspace((unsigned char)**cursor) || **cursor == ',') {
        ++*cursor;
    }
}

static bool read_point(const char **cursor, float scale, hw_point_t *point)
{
    skip_separators(cursor);
    char *end;
    float x = strtof(*cursor, &end);
    if (end == *cursor || !isfinite(x)) {
        return false;
    }
    *cursor = end;
    skip_separators(cursor);
    float y = strtof(*cursor, &end);
    if (end == *cursor || !isfinite(y)) {
        return false;
    }
    *cursor = end;
    *point = (hw_point_t){(x + 32) * scale, (932 - y) * scale};
    return true;
}

static bool read_outline(const char *path, float scale, outline_t *outline,
                          const char **failure)
{
    hw_point_t current = {0};
    hw_point_t start = {0};
    bool contour_open = false;
    char command = 0;
    for (;;) {
        skip_separators(&path);
        if (!*path) {
            return !contour_open || add_edge(outline, current, start);
        }
        if (isalpha((unsigned char)*path)) {
            command = *path++;
        }
        hw_point_t a, b, c;
        switch (command) {
        case 'M':
            if (!read_point(&path, scale, &a)) {
                return false;
            }
            if (contour_open && !add_edge(outline, current, start)) {
                *failure = "Out of memory flattening stroke";
                return false;
            }
            current = start = a;
            contour_open = true;
            command = 'L';
            continue;
        case 'L':
            if (!contour_open || !read_point(&path, scale, &a)) {
                return false;
            }
            if (!add_edge(outline, current, a)) {
                break;
            }
            current = a;
            continue;
        case 'Q':
            if (!contour_open || !read_point(&path, scale, &a) || !read_point(&path, scale, &b)) {
                return false;
            }
            if (!quadratic(outline, current, a, b, 0)) {
                break;
            }
            current = b;
            continue;
        case 'C':
            if (!contour_open || !read_point(&path, scale, &a) ||
                !read_point(&path, scale, &b) || !read_point(&path, scale, &c)) {
                return false;
            }
            if (!cubic(outline, current, a, b, c, 0)) {
                break;
            }
            current = c;
            continue;
        case 'Z':
            if (!contour_open) {
                return false;
            }
            if (!add_edge(outline, current, start)) {
                break;
            }
            current = start;
            contour_open = false;
            command = 0;
            continue;
        default:
            *failure = "Unsupported stroke path command (expected M/L/Q/C/Z)";
            return false;
        }
        *failure = "Out of memory flattening stroke";
        return false;
    }
}

static int crossing_compare(const void *left, const void *right)
{
    float a = ((const crossing_t *)left)->x;
    float b = ((const crossing_t *)right)->x;
    return (a > b) - (a < b);
}

static void cover_span(float *coverage, const hw_path_mask_t *mask, float left, float right)
{
    left = fmaxf(left, (float)mask->x);
    right = fminf(right, (float)(mask->x + mask->w));
    if (right <= left) {
        return;
    }
    for (int x = (int)floorf(left); x < (int)ceilf(right); ++x) {
        coverage[x - mask->x] += fminf(right, (float)x + 1) - fmaxf(left, (float)x);
    }
}

bool hw_path_rasterize(const char *path, unsigned size, hw_path_mask_t *mask,
                       char *error, size_t error_size)
{
    *mask = (hw_path_mask_t){0};
    outline_t outline = {.min_x = INFINITY, .min_y = INFINITY,
                         .max_x = -INFINITY, .max_y = -INFINITY};
    crossing_t *crossings = NULL;
    float *coverage = NULL;
    const char *failure = "Invalid stroke path";
    if (!read_outline(path, (float)size / 1088, &outline, &failure)) {
        goto bad;
    }
    if (!outline.count) {
        failure = "Empty stroke path";
        goto bad;
    }
    int left = (int)fminf((float)size, fmaxf(0, floorf(outline.min_x)));
    int top = (int)fminf((float)size, fmaxf(0, floorf(outline.min_y)));
    int right = (int)fmaxf(0, fminf((float)size, ceilf(outline.max_x)));
    int bottom = (int)fmaxf(0, fminf((float)size, ceilf(outline.max_y)));
    if (right <= left || bottom <= top) {
        failure = "Empty stroke path";
        goto bad;
    }
    mask->x = (unsigned)left;
    mask->y = (unsigned)top;
    mask->w = (unsigned)(right - left);
    mask->h = (unsigned)(bottom - top);
    mask->alpha = malloc((size_t)mask->w * mask->h);
    crossings = malloc(outline.count * sizeof(*crossings));
    coverage = malloc(mask->w * sizeof(*coverage));
    if (!mask->alpha || !crossings || !coverage) {
        failure = "Out of memory rasterizing stroke";
        goto bad;
    }
    for (unsigned y = 0; y < mask->h; ++y) {
        memset(coverage, 0, mask->w * sizeof(*coverage));
        for (unsigned sample = 0; sample < SCAN_SAMPLES; ++sample) {
            float scan_y = mask->y + y + (sample + 0.5f) / SCAN_SAMPLES;
            size_t count = 0;
            for (size_t i = 0; i < outline.count; ++i) {
                const path_edge_t *edge = &outline.edges[i];
                if (scan_y >= edge->y_min && scan_y < edge->y_max) {
                    crossings[count++] = (crossing_t){
                        edge->x + (scan_y - edge->y_min) * edge->dx_dy, edge->winding
                    };
                }
            }
            qsort(crossings, count, sizeof(*crossings), crossing_compare);
            int winding = 0;
            float previous = 0;
            for (size_t i = 0; i < count; ++i) {
                if (winding) {
                    cover_span(coverage, mask, previous, crossings[i].x);
                }
                winding += crossings[i].winding;
                previous = crossings[i].x;
            }
        }
        for (unsigned x = 0; x < mask->w; ++x) {
            unsigned alpha = (unsigned)(coverage[x] * (255.0f / SCAN_SAMPLES) + 0.5f);
            mask->alpha[(size_t)y * mask->w + x] = (uint8_t)(alpha > 255 ? 255 : alpha);
        }
    }
    free(coverage);
    free(crossings);
    free(outline.edges);
    return true;
bad:
    if (error && error_size) {
        snprintf(error, error_size, "%s", failure);
    }
    free(mask->alpha);
    mask->alpha = NULL;
    free(coverage);
    free(crossings);
    free(outline.edges);
    return false;
}
