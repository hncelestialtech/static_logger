#include <stdio.h>
#include <unistd.h>
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include "static_log.h"

struct log_stat {
    volatile size_t payload;
    char pad1[128 - sizeof(size_t)];
    volatile int start;
    volatile int end;
    char pad2[128 - sizeof(int)];
    volatile size_t bytes_in;
}  __attribute__((aligned(128)));

struct log_stat logger_stat;

#define full_barrier()  \
    __asm__ __volatile("lock; orl $0, (%%rsp)":::"memory")

#define pause() \
    __asm__ __volatile("pause":::"memory")

void stat_report(int sig __attribute__((unused)))
{
    full_barrier();
    struct timespec end{};
    clock_gettime(CLOCK_REALTIME, &end);
    logger_stat.end = end.tv_sec * 1000000000 + end.tv_nsec;
    float duration = (logger_stat.end - logger_stat.start) / (1000000000.0);
    float bytes_in = logger_stat.bytes_in / duration;
    fprintf(stdout, "payload %d, throughput: %lf B/s %lf KB/s %lf MB/s\n", logger_stat.payload, bytes_in, bytes_in/1024, bytes_in/(1024 * 1024));
    full_barrier();
    logger_stat.end = 1;
    return;
}

template<int payload>
void perf_benchmark() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        fprintf(stderr, "Failed to set thread affinity\n");
    }
    logger_stat.payload = payload;
    pthread_t reporter_tid;
    logger_stat.start = 0;
    logger_stat.end = 0;
    char log_buffer[payload + 1];
    for (int i = 0; i < payload; ++i)
    {
        log_buffer[i] = '1';
    }
    log_buffer[payload] = '\0';
    
    timespec start{};
    clock_gettime(CLOCK_REALTIME, &start);
    logger_stat.start = start.tv_nsec + start.tv_sec * 1000000000;
    full_barrier();
    while (!logger_stat.end) {
        STATIC_LOG(static_log::LogLevels::kWARNING, "%s", log_buffer);
        logger_stat.bytes_in += payload;
        full_barrier();
    }

}

int main()
{
    struct sigaction act, oldact;
    act.sa_handler = stat_report;
    sigemptyset(&act.sa_mask); 
    sigaddset(&act.sa_mask, SIGINT);
    sigaction(SIGINT, &act, &oldact);
    perf_benchmark<64>();
    perf_benchmark<128>();
}