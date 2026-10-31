#include "hanzi.h"
#include <math.h>
#include <string.h>

void hw_player_init(hw_player_t * p, const hw_character_t * c)
{
    memset(p, 0, sizeof(*p));
    p->character = c;
    p->speed = 1.0f;
    p->state = HW_READY;
}

void hw_player_restart(hw_player_t * p, bool play)
{
    p->completed = 0;
    p->progress = 0;
    p->gap_ms = 0;
    p->state = play ? HW_PLAYING : HW_READY;
}

void hw_player_toggle(hw_player_t * p)
{
    if (p->state == HW_PLAYING) {
        p->state = HW_PAUSED;
    } else if (p->state == HW_FINISHED) {
        hw_player_restart(p, true);
    } else {
        p->state = HW_PLAYING;
    }
}

void hw_player_next(hw_player_t * p)
{
    if (p->completed < p->character->stroke_count) {
        ++p->completed;
    }
    p->progress = 0;
    p->gap_ms = 0;
    p->state = p->completed == p->character->stroke_count ? HW_FINISHED : HW_PAUSED;
}

void hw_player_update(hw_player_t * p, uint32_t elapsed_ms)
{
    if (p->state != HW_PLAYING) {
        return;
    }
    float remaining = (float)elapsed_ms * p->speed;
    while (remaining > 0) {
        if (p->gap_ms > 0) {
            float consumed = fminf(remaining, p->gap_ms);
            p->gap_ms -= consumed;
            remaining -= consumed;
            if (p->gap_ms > 0) {
                break;
            }
        }
        if (p->completed == p->character->stroke_count) {
            if (!p->loop) {
                p->state = HW_FINISHED;
                break;
            }
            p->completed = 0;
            p->progress = 0;
        }
        float length = hw_stroke_length(&p->character->strokes[p->completed]);
        float duration = fmaxf(350, length * 1000 / 650);
        float need = (1 - p->progress) * duration;
        if (remaining < need) {
            p->progress += remaining / duration;
            break;
        }
        remaining -= need;
        p->progress = 0;
        ++p->completed;
        if (p->completed == p->character->stroke_count && !p->loop) {
            p->state = HW_FINISHED;
            break;
        }
        p->gap_ms = p->completed == p->character->stroke_count ? 1000.0f : 220.0f;
    }
}
