#include "static_log.h"

#include <time.h>
#include <iostream>

#include "spdlog/logger.h"
#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
// #include "NanoLogCpp17.h"

void
perf_static_log()
{
    static_log::preallocate();
    struct timespec begin{}, end{};
    clock_gettime(CLOCK_REALTIME, &begin);
    for(int i = 0; i < 1000; ++i) {
        STATIC_LOG(static_log::LogLevels::kNOTICE, "%s %i %i %i %i %i %i %i %i %i %i ", "hello world", i, i, i ,i , i ,i ,i ,i ,i ,i);
    } 

    clock_gettime(CLOCK_REALTIME, &end);
    uint64_t duration = end.tv_sec * 1000000000 + end.tv_nsec - (begin.tv_sec * 1000000000 + begin.tv_nsec);
    std::cout<<__FUNCTION__<<":"<<duration/1000<<std::endl;
}

void perf_spdlog()
{
    auto logger = spdlog::basic_logger_mt<spdlog::async_factory>(__FUNCTION__, "spdlog.txt");
    logger->set_level(spdlog::level::debug);
    struct timespec begin{}, end{};
    const char* helloworld = "hello world";
    clock_gettime(CLOCK_REALTIME, &begin);
    for(int i = 0; i < 1000; ++i) {
        logger->info("{} {} {} {} {} {} {} {} {} {} {}", helloworld, i, i, i, i, i, i, i, i, i, i);
    }
    clock_gettime(CLOCK_REALTIME, &end);
    uint64_t duration = end.tv_sec * 1000000000 + end.tv_nsec - (begin.tv_sec * 1000000000 + begin.tv_nsec);
    std::cout<<__FUNCTION__<<":"<<duration/1000<<std::endl;
}

void perf_static_log_fmt1()
{
    static_log::preallocate();
    struct timespec begin{}, end{};
    clock_gettime(CLOCK_REALTIME, &begin);
    for(int i = 0; i < 1000; ++i) {
        STATIC_LOG(static_log::LogLevels::kNOTICE, "%s", "hello world");
    } 

    clock_gettime(CLOCK_REALTIME, &end);
    uint64_t duration = end.tv_sec * 1000000000 + end.tv_nsec - (begin.tv_sec * 1000000000 + begin.tv_nsec);
    std::cout<<__FUNCTION__<<":"<<duration/1000<<std::endl;
}

void perf_spdlog_fmt1()
{
    auto logger = spdlog::basic_logger_mt<spdlog::async_factory>(__FUNCTION__, "spdlog.txt");
    logger->set_level(spdlog::level::debug);
    struct timespec begin{}, end{};
    const char* helloworld = "hello world";
    clock_gettime(CLOCK_REALTIME, &begin);
    for(int i = 0; i < 1000; ++i) {
        logger->info("{}", helloworld);
    }
    clock_gettime(CLOCK_REALTIME, &end);
    uint64_t duration = end.tv_sec * 1000000000 + end.tv_nsec - (begin.tv_sec * 1000000000 + begin.tv_nsec);
    std::cout<<__FUNCTION__<<":"<<duration/1000<<std::endl;
}

void perf_static_log_float_fmt()
{
    static_log::preallocate();
    struct timespec begin{}, end{};
    const double param = 3.14;
    clock_gettime(CLOCK_REALTIME, &begin);
    for(int i = 0; i < 1000; ++i) {
        STATIC_LOG(static_log::LogLevels::kNOTICE, "%s %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf ", "hello world", param, param, param, param, param, param, param, param, param, param);
    } 

    clock_gettime(CLOCK_REALTIME, &end);
    uint64_t duration = end.tv_sec * 1000000000 + end.tv_nsec - (begin.tv_sec * 1000000000 + begin.tv_nsec);
    std::cout<<__FUNCTION__<<":"<<duration/1000<<std::endl;
}

void perf_spdlog_float_fmt1()
{
    auto logger = spdlog::basic_logger_mt<spdlog::async_factory>(__FUNCTION__, "spdlog.txt");
    logger->set_level(spdlog::level::debug);
    struct timespec begin{}, end{};
    const char* helloworld = "hello world";
    const double param = 3.14;
    clock_gettime(CLOCK_REALTIME, &begin);
    for(int i = 0; i < 1000; ++i) {
        logger->info("{} {} {} {} {} {} {} {} {} {} {}", helloworld, param, param, param, param, param, param, param, param, param, param);
    }
    clock_gettime(CLOCK_REALTIME, &end);
    uint64_t duration = end.tv_sec * 1000000000 + end.tv_nsec - (begin.tv_sec * 1000000000 + begin.tv_nsec);
    std::cout<<__FUNCTION__<<":"<<duration/1000<<std::endl;
}

// void perf_nanolog()
// {
//     struct timespec begin{}, end{};
//     const char* helloworld = "hello world";
//     NanoLog::preallocate();
//     NanoLog::setLogLevel(NanoLog::LogLevels::NOTICE);
//     clock_gettime(CLOCK_REALTIME, &begin);
//     for(int i = 0; i < 1000; ++i) {
//         NANO_LOG(DEBUG, "%s", "hello world");
//     }
//     clock_gettime(CLOCK_REALTIME, &end);
//     uint64_t duration = end.tv_sec * 1000000000 + end.tv_nsec - (begin.tv_sec * 1000000000 + begin.tv_nsec);
//     std::cout<<__FUNCTION__<<":"<<duration/1000<<std::endl;
// }

int main()
{
    perf_static_log();
    perf_spdlog();
    perf_static_log_fmt1();
    perf_spdlog_fmt1();
    perf_static_log_float_fmt();
    perf_spdlog_float_fmt1();
    return 0;
}