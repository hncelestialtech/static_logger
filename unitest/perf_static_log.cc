#include "static_log.h"
#include "static_log_front.h"

#include <time.h>
#include <iostream>

void
perf_str_log()
{
    struct timespec begin{}, end{};
    clock_gettime(CLOCK_REALTIME, &begin);
    for(int i = 0; i < 1000; ++i) {
        STATIC_LOG(static_log::LogLevels::kNOTICE, "%s", "hello world");
    } 

    clock_gettime(CLOCK_REALTIME, &end);
    uint64_t duration = end.tv_sec * 1000000000 + end.tv_nsec - (begin.tv_sec * 1000000000 + begin.tv_nsec);
    std::cout<<__FUNCTION__<<":"<<duration<<std::endl;
}

int main()
{
    perf_str_log();
    return 0;
}