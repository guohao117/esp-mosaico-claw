#ifndef HANZI_H
#define HANZI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HW_MAX_STROKES 64

typedef struct {
    float x, y;
} hw_point_t;

typedef struct {
    char * path;
    hw_point_t * median;
    size_t median_count;
    float length; /* Cached median length in character coordinates. */
    bool radical;
} hw_stroke_t;

typedef struct {
    size_t stroke_count;
    hw_stroke_t strokes[HW_MAX_STROKES];
} hw_character_t;

/* Input is a NUL-terminated hanzi-writer-data JSON document. Caller owns result. */
hw_character_t * hw_character_parse(const char * json, char * error, size_t error_size);
void hw_character_destroy(hw_character_t * character);
float hw_stroke_length(const hw_stroke_t * stroke);

typedef enum {
    HW_READY,
    HW_PLAYING,
    HW_PAUSED,
    HW_FINISHED
} hw_state_t;

typedef struct {
    const hw_character_t * character;
    hw_state_t state;
    size_t completed;
    float progress, speed, gap_ms;
    bool loop;
} hw_player_t;

void hw_player_init(hw_player_t * player, const hw_character_t * character);
void hw_player_restart(hw_player_t * player, bool play);
void hw_player_toggle(hw_player_t * player);
void hw_player_next(hw_player_t * player);
void hw_player_update(hw_player_t * player, uint32_t elapsed_ms);
#endif
