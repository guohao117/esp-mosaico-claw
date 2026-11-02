// SPDX-License-Identifier: Apache-2.0
#ifndef HANZI_PATH_H
#define HANZI_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned x, y, w, h;
    uint8_t *alpha;
} hw_path_mask_t;

/* hanzi-writer-data uses absolute M/L/Q/C/Z paths in a 1024-unit space.
 * On success the caller owns mask->alpha. */
bool hw_path_rasterize(const char *path, unsigned size, hw_path_mask_t *mask,
                       char *error, size_t error_size);

#endif
