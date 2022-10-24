#include "static_log.h"
#include <thread>
#include <atomic>

std::atomic<int> id;

void test_log() {
    for(int i = 0; i < 10000; ++i)
    {
        STATIC_LOG(static_log::LogLevels::kNOTICE, "%s %d", "hello world", id++);
    }
}

int main() {
    static_log::preallocate();
    id = 1;
    std::thread t1 = std::thread(test_log);
    std::thread t2 = std::thread(test_log);
    std::thread t3 = std::thread(test_log);
    std::thread t4 = std::thread(test_log);
    t1.join();
    t2.join();
    t3.join();
    t4.join();
    return 0;
}