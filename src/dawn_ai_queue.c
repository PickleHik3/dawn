// dawn_ai_queue.c

#include "dawn_ai_queue.h"

#if HAS_LIBAI

#include "ai.h"

static AiLane g_active_lane = AI_LANE_NONE;
static int32_t g_user_retry_count;

AiLane ai_queue_active_lane(void) { return g_active_lane; }

void ai_queue_set_lane(AiLane lane) { g_active_lane = lane; }

bool ai_queue_quiet_is_running(void) { return g_active_lane == AI_LANE_QUIET; }

bool ai_queue_quiet_may_start(void)
{
    if (g_active_lane != AI_LANE_NONE)
        return false;
    return ai_runtime_state() == AI_MODEL_LOADED;
}

void ai_queue_user_reset(void) { g_user_retry_count = 0; }

int32_t ai_queue_user_busy(void)
{
    static const int32_t delays_ms[] = { 1000, 2000, 4000 };
    if (g_user_retry_count >= (int32_t)(sizeof(delays_ms) / sizeof(delays_ms[0])))
        return -1;
    return delays_ms[g_user_retry_count++];
}

#endif // HAS_LIBAI
