#include "hanzi.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char * p;
    const char * error;
} json_t;

static void ws(json_t * j)
{
    while (isspace((unsigned char)*j->p)) {
        ++j->p;
    }
}

static bool fail(json_t * j, const char * message)
{
    if (!j->error) {
        j->error = message;
    }
    return false;
}

static bool take(json_t * j, char c)
{
    ws(j);
    if (*j->p != c) {
        return fail(j, "Unexpected JSON token");
    }
    ++j->p;
    return true;
}

static bool peek(json_t * j, char c)
{
    ws(j);
    return *j->p == c;
}

/* Local hanzi-writer-data files use plain ASCII keys and unescaped SVG paths. */
static char * string_value(json_t * j)
{
    if (!take(j, '"')) {
        return NULL;
    }
    const char * end = strchr(j->p, '"');
    if (!end) {
        fail(j, "Unterminated JSON string");
        return NULL;
    }
    size_t size = (size_t)(end - j->p);
    char * text = malloc(size + 1);
    if (!text) {
        fail(j, "Out of memory");
        return NULL;
    }
    memcpy(text, j->p, size);
    text[size] = 0;
    j->p = end + 1;
    return text;
}

static bool number(json_t * j, float * value)
{
    ws(j);
    char * end;
    *value = strtof(j->p, &end);
    if (end == j->p || !isfinite(*value)) {
        return fail(j, "Expected number");
    }
    j->p = end;
    return true;
}

static bool read_strokes(json_t * j, hw_character_t * c, size_t * count)
{
    if (!take(j, '[')) {
        return false;
    }
    do {
        if (*count == HW_MAX_STROKES) {
            return fail(j, "Too many strokes");
        }
        hw_stroke_t * stroke = &c->strokes[*count];
        stroke->path = string_value(j);
        if (!stroke->path) {
            return false;
        }
        if (!stroke->path[0]) {
            return fail(j, "Empty SVG path");
        }
        ++*count;
        if (peek(j, ']')) {
            break;
        }
    } while (take(j, ','));
    return !j->error && take(j, ']');
}

static bool read_median(json_t * j, hw_stroke_t * stroke)
{
    if (!take(j, '[')) {
        return false;
    }
    size_t capacity = 0;
    do {
        if (stroke->median_count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 8;
            hw_point_t * points = realloc(stroke->median, new_capacity * sizeof(*points));
            if (!points) {
                return fail(j, "Out of memory");
            }
            stroke->median = points;
            capacity = new_capacity;
        }
        hw_point_t * point = &stroke->median[stroke->median_count];
        if (!take(j, '[') || !number(j, &point->x) || !take(j, ',') ||
            !number(j, &point->y) || !take(j, ']')) {
            return false;
        }
        if (stroke->median_count > 0) {
            const hw_point_t * previous = point - 1;
            stroke->length += hypotf(point->x - previous->x, point->y - previous->y);
        }
        ++stroke->median_count;
        if (peek(j, ']')) {
            break;
        }
    } while (take(j, ','));
    if (j->error || !take(j, ']')) {
        return false;
    }
    if (stroke->median_count < 2 || stroke->length <= 0) {
        return fail(j, "A median needs two distinct points");
    }
    return true;
}

static bool read_medians(json_t * j, hw_character_t * c, size_t * count)
{
    if (!take(j, '[')) {
        return false;
    }
    do {
        if (*count == HW_MAX_STROKES) {
            return fail(j, "Too many medians");
        }
        if (!read_median(j, &c->strokes[*count])) {
            return false;
        }
        ++*count;
        if (peek(j, ']')) {
            break;
        }
    } while (take(j, ','));
    return !j->error && take(j, ']');
}

static bool read_radicals(json_t * j, hw_character_t * c)
{
    if (!take(j, '[')) {
        return false;
    }
    if (!peek(j, ']')) {
        do {
            float index;
            if (!number(j, &index)) {
                return false;
            }
            if (index < 0 || index >= HW_MAX_STROKES || floorf(index) != index) {
                return fail(j, "Invalid radical stroke index");
            }
            c->strokes[(size_t)index].radical = true;
            if (peek(j, ']')) {
                break;
            }
        } while (take(j, ','));
    }
    return !j->error && take(j, ']');
}

void hw_character_destroy(hw_character_t * c)
{
    if (!c) {
        return;
    }
    for (size_t i = 0; i < HW_MAX_STROKES; ++i) {
        free(c->strokes[i].path);
        free(c->strokes[i].median);
    }
    free(c);
}

hw_character_t * hw_character_parse(const char * json, char * error, size_t error_size)
{
    const char * input = json ? json : "";
    json_t j = {input, NULL};
    hw_character_t * c = calloc(1, sizeof(*c));
    size_t paths = 0;
    size_t medians = 0;
    if (error && error_size) {
        error[0] = 0;
    }
    if (!c) {
        fail(&j, "Out of memory");
        goto bad;
    }
    if (!take(&j, '{')) {
        goto bad;
    }
    do {
        char * key = string_value(&j);
        if (!key) {
            goto bad;
        }
        bool strokes = strcmp(key, "strokes") == 0;
        bool median = strcmp(key, "medians") == 0;
        bool radical = strcmp(key, "radStrokes") == 0;
        free(key);
        if (!take(&j, ':')) {
            goto bad;
        }
        bool parsed;
        if (strokes) {
            parsed = read_strokes(&j, c, &paths);
        } else if (median) {
            parsed = read_medians(&j, c, &medians);
        } else if (radical) {
            parsed = read_radicals(&j, c);
        } else {
            parsed = fail(&j, "Unknown character data field");
        }
        if (!parsed) {
            goto bad;
        }
        if (peek(&j, '}')) {
            break;
        }
    } while (take(&j, ','));
    if (j.error || !take(&j, '}')) {
        goto bad;
    }
    ws(&j);
    if (*j.p || paths == 0 || paths != medians) {
        fail(&j, "Stroke/median counts do not match or trailing JSON");
        goto bad;
    }
    c->stroke_count = paths;
    for (size_t i = paths; i < HW_MAX_STROKES; ++i) {
        if (c->strokes[i].radical) {
            fail(&j, "Radical index exceeds stroke count");
            goto bad;
        }
    }
    return c;
bad:
    if (error && error_size) {
        snprintf(error, error_size, "%s (byte %lu)",
                 j.error ? j.error : "Invalid character data", (unsigned long)(j.p - input));
    }
    hw_character_destroy(c);
    return NULL;
}

float hw_stroke_length(const hw_stroke_t * stroke)
{
    return stroke->length;
}
