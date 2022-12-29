#include "static_log_cycles.h"
#include <time.h>

int64_t 
get_nanotime()
{
    struct timespec clock;
    clock_gettime(CLOCK_REALTIME, &clock);
    return clock.tv_sec * 1000000000 + clock.tv_nsec;
}