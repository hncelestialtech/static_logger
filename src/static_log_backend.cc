#include "static_log_backend.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <iostream>

#include "static_log_internal.h"
#include "static_log_utils.h"
#include "tsc_clock.h"

namespace static_log {

namespace details {
__thread StagingBuffer *StaticLogBackend::staging_buffer_ = nullptr;
StaticLogBackend StaticLogBackend::logger_;
thread_local StaticLogBackend::StagingBufferDestroyer StaticLogBackend::destroyer_{};
#define DEFAULT_INTERVAL 10
uint32_t poll_interval_no_work = DEFAULT_INTERVAL;

#define DEFAULT_LOGFILE     "log.txt"

StaticLogBackend::StaticLogBackend():
    current_log_level_(LogLevels::kDEBUG),
    buffer_mutex_(),
    cond_mutex_(),
    wake_up_cond_(),
    next_buffer_id_(0),
    thread_buffers_(),
    is_stop_(false),
    outfd_(-1)
{
    const char * logfile = DEFAULT_LOGFILE;
    outfd_ = open(logfile, O_RDWR|O_CREAT, 0666);
    if (outfd_ < 0) {
        fprintf(stderr, "Failed to open log file\n");
        exit(-1);
    }

    fdflush_ = std::thread(&StaticLogBackend::ioPoll, this);

}

StaticLogBackend::~StaticLogBackend()
{
    is_stop_ = true;
    fdflush_.join();
}

static void
convertInt2Str(int ts, char*& raw) {
    if (ts < 10) {
        *raw = '0';
        raw++;
        *raw = std::to_string(ts).c_str()[0];
        raw++;
    } else {
        memcpy(raw, std::to_string(ts).c_str(), 2);
        raw+=2;
    }
}

// [xxxx-xx-xx-hh:mm:ss.xxxxxxxxx]
int 
generateTimePrefix(uint64_t timestamp, char* raw_data) {
    const int prefix_len = 31;
    const int nano_bits = 9;
    *raw_data = '[';
    raw_data++;
    uint64_t nano = timestamp % 1000000000;
    timestamp = timestamp / 1000000000;
    struct tm* tm_now = localtime((time_t*)&timestamp);
    memcpy(raw_data, std::to_string(tm_now->tm_year + 1900).c_str(), 4);
    raw_data += 4;
    *raw_data++ = '-';
    convertInt2Str(tm_now->tm_mon + 1, raw_data);
    *raw_data++ = '-';
    convertInt2Str(tm_now->tm_mday, raw_data);
    *raw_data++ = '-';
    convertInt2Str(tm_now->tm_hour, raw_data);
    *raw_data++ = ':';
    convertInt2Str(tm_now->tm_min, raw_data);
    *raw_data++ = ':';
    convertInt2Str(tm_now->tm_sec, raw_data);
    *raw_data++ = '.';
    int nanolen = strlen(std::to_string(nano).c_str());
    memcpy(raw_data, std::to_string(nano).c_str(), nanolen);
    raw_data += nanolen;
    if (nanolen < nano_bits) {
        int bits = nano_bits - nanolen;
        for(int i = 0; i < bits; ++i)
            *raw_data++ = '0';
    }
    *raw_data = ']';
    return prefix_len;
}

#pragma GCC diagnostic ignored "-Wformat"
static int
decodeNonStringFmt(
    char* __restrict__ log_buffer, 
    size_t log_buffer_len, 
    char * fmt, 
    const char* param, 
    size_t param_size)
{
    int fmt_len = 0;
    char terminal_flag = fmt[strlen(fmt) - 1];
    switch (param_size) {
    case sizeof(char):
        {
            const char* param8 = param;
            if (terminal_flag == 'c')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int8_t*)param8);
            else if (terminal_flag == 'd' || terminal_flag == 'i')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int8_t*)param8);
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(uint8_t*)param8);
            else 
                fprintf(stderr, "Failed to parse fmt with a one bytes long param\n");
            break;
        }
    case sizeof(uint16_t):
        {
            const uint16_t* param16 = (uint16_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int16_t*)param16);
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(uint16_t*)param16);
            else 
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *param16);
            break;
        }
    case sizeof(uint32_t):
        {
            const uint32_t* param32 = (uint32_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int32_t*)param32);
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(uint32_t*)param32);
            else if (terminal_flag == 'f' || terminal_flag == 'F' || terminal_flag == 'e' ||terminal_flag == 'e' || 
                terminal_flag == 'E' || terminal_flag == 'g' || terminal_flag == 'G'|| terminal_flag == 'a' ||terminal_flag == 'A') {
                static_assert(sizeof(float) == sizeof(uint32_t));
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(float*)param32);
            }            
            else 
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    case sizeof(uint64_t):
        {
            const uint64_t* param64 = (uint64_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int64_t*)param64);
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(uint64_t*)param64);
            else if (terminal_flag == 'f' || terminal_flag == 'F' || terminal_flag == 'e' ||terminal_flag == 'e' || 
                terminal_flag == 'E' || terminal_flag == 'g' || terminal_flag == 'G'|| terminal_flag == 'a' ||terminal_flag == 'A') {
                static_assert(sizeof(double) == sizeof(uint64_t));
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(double*)param64);
            }
            else
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    default:
        fprintf(stderr, "Failed to decode fmt param, got size %ld\n", param_size);
        break;
    }
    return fmt_len;
}
#pragma GCC diagnostic pop

static int
process_fmt(
        const char* fmt, 
        const int num_params, 
        const internal::ParamType* param_types,
        size_t* param_size_list,
        const char* param_list, 
        char* log_buffer, size_t buflen)
{
    char* origin_buffer = log_buffer;
    int origin_len = buflen;
    size_t fmt_list_len = strlen(fmt);
    int pos = 0;
    int param_idx = 0;
    bool success = true;
    while (pos < fmt_list_len) {
        if (fmt[pos] != '%') {
            *log_buffer++ = fmt[pos++];
            buflen--;
            continue;
        } else {
            ++pos;
            int fmt_single_len = 1;
            if (fmt[pos] == '%') {
                *log_buffer++ = '%';
                buflen--;
                ++pos;
                continue;
            } else {
                int fmt_start_pos = pos - 1;
                while (!internal::utils::isTerminal(fmt[pos])) {
                    fmt_single_len++;
                    pos++;
                }
                fmt_single_len++;
                pos++;
                char* fmt_single;
                char static_fmt_cache[100];
                bool dynamic_fmt = false;
                if (fmt_single_len < 100) {
                    memset(static_fmt_cache, 0, 100);
                    fmt_single = static_fmt_cache;
                } else {
                    fmt_single = (char*)malloc(fmt_single_len);
                    dynamic_fmt = true;
                }
                memcpy(fmt_single, fmt + fmt_start_pos, fmt_single_len);
                size_t log_fmt_len = 0;

                if (param_idx < num_params) {
                    if (param_types[param_idx] > internal::ParamType::NON_STRING) {
                        uint32_t string_size = *(uint32_t*)param_list;
                        param_list += sizeof(uint32_t);
                        char *param_str = (char*)malloc(string_size);
                        memcpy(param_str, param_list, string_size);
                        param_str[string_size] = '\0';
                        log_fmt_len = snprintf(log_buffer, buflen, fmt_single, param_str);
                        free(param_str);
                    }
                    else {
                        log_fmt_len = decodeNonStringFmt(log_buffer, buflen, fmt_single, param_list, param_size_list[param_idx]);
                        param_list += param_size_list[param_idx];
                    }
                    log_buffer += log_fmt_len;
                    buflen -= log_fmt_len;
                    param_idx++;
                } else {
                    fprintf(stderr, "Failed to fmt log\n");
                    success = false;
                    if (dynamic_fmt) 
                        free(fmt_single);
                    break;
                }
                if (dynamic_fmt) 
                    free(fmt_single);
            }
        }
    }
    return success? origin_len - buflen: -1;
}

#define DEFALT_CACHE_SIZE 1024 * 1024
void 
StaticLogBackend::processLogBuffer(StagingBuffer* stagingbuffer)
{
    char log_content_cache[DEFALT_CACHE_SIZE];
    int reserved = DEFALT_CACHE_SIZE;
    uint64_t bytes_available = 0;
    char* raw_data = stagingbuffer->peek(&bytes_available);
    if (bytes_available > 0) {
        internal::LogEntry *log_entry = (internal::LogEntry *)raw_data;
        log_entry->timestamp = rdns();
        auto prefix_len = generateTimePrefix(log_entry->timestamp, log_content_cache);
        log_content_cache[prefix_len] = '\0';
        reserved -= prefix_len;
        const char* fmt = log_entry->static_info->format;
        int len = process_fmt(fmt, 
                    log_entry->static_info->num_params, 
                    log_entry->static_info->param_types,
                    (size_t*)log_entry->param_size,
                    (char*)log_entry + sizeof(internal::LogEntry),
                    log_content_cache + prefix_len, reserved - prefix_len);
        char* log = log_content_cache + prefix_len + len;
        *log = '\n';
        if (len != -1) {
            write(StaticLogBackend::logger_.outfd_, log_content_cache, prefix_len + len + 1);
        }
        stagingbuffer->consume(log_entry->entry_size);
    }
}

void
StaticLogBackend::walkLogBuffer()
{
    while(!is_stop_ || !thread_buffers_.empty()) {
        std::pair<uint64_t, static_log::details::StagingBuffer *> earliest_thead_buffer{UINT64_MAX, nullptr};
        for(int i = 0; i < thread_buffers_.size(); ++i) {
            auto thread_buffer = thread_buffers_[i];
            if (thread_buffer->isAlive()) {
                uint64_t bytes_available = 0;
                char* raw_data = thread_buffer->peek(&bytes_available);
                if (bytes_available > 0) {
                    internal::LogEntry *log_entry = (internal::LogEntry *)raw_data;
                    if (log_entry->timestamp < earliest_thead_buffer.first) {
                        earliest_thead_buffer.first = log_entry->timestamp;
                        earliest_thead_buffer.second = thread_buffer;
                    }
                }
            }
            else {
                delete thread_buffer;
                thread_buffers_.erase(thread_buffers_.begin() + i);
                if (thread_buffers_.empty())
                    break;
                --i;
            }
        }
        if (earliest_thead_buffer.first != UINT64_MAX) {
            processLogBuffer(earliest_thead_buffer.second);
        }
    }
}

static int
threadBindCore(int i)
{  
    cpu_set_t mask;  
    CPU_ZERO(&mask);  
  
    CPU_SET(i,&mask);  
  
    if(-1 == pthread_setaffinity_np(pthread_self() ,sizeof(mask),&mask))  
    {  
        fprintf(stderr, "pthread_setaffinity_np erro\n");  
        return -1;  
    }  
    return 0;  
} 

void 
StaticLogBackend::ioPoll()
{
    threadBindCore(1);

    walkLogBuffer();
}

/**
* Attempt to reserve contiguous space for the producer without making it
* visible to the consumer (See reserveProducerSpace).
*
* This is the slow path of reserveProducerSpace that checks for free space
* within storage[] that involves touching variable shared with 
* thread and thus causing potential cache-coherency delays.
*
* \param nbytes
*      Number of contiguous bytes to reserve.
*
* \param blocking
*      Test parameter that indicates that the function should
*      return with a nullptr rather than block when there's
*      not enough space.
*
* \return
*      A pointer into storage[] that can be written to by the producer for
*      at least nbytes.
*/
char *
StagingBuffer::reserveSpaceInternal(size_t nbytes, bool blocking) 
{
    const char *end_of_buffer = storage_ + STAGING_BUFFER_SIZE;

    // There's a subtle point here, all the checks for remaining
    // space are strictly < or >, not <= or => because if we allow
    // the record and print positions to overlap, we can't tell
    // if the buffer either completely full or completely empty.
    // Doing this check here ensures that == means completely empty.
    while (min_free_space_ <= nbytes) {
        // Since consumerPos can be updated in a different thread, we
        // save a consistent copy of it here to do calculations on
        char *cached_consumer_pos = consumer_pos_;

        if (cached_consumer_pos <= producer_pos_) {
            min_free_space_ = end_of_buffer - producer_pos_;

            if (min_free_space_ > nbytes)
                break;

            // Not enough space at the end of the buffer; wrap around
            end_of_recorded_space_ = producer_pos_;

            // Prevent the roll over if it overlaps the two positions because
            // that would imply the buffer is completely empty when it's not.
            if (cached_consumer_pos != storage_) {
                producer_pos_ = storage_;
                min_free_space_ = cached_consumer_pos - producer_pos_;
            }
        } else {
            min_free_space_ = cached_consumer_pos - producer_pos_;
        }

#ifdef BENCHMARK_DISCARD_ENTRIES_AT_STAGINGBUFFER
        // If we are discarding entries anwyay, just reset space to the head
        producer_pos_ = storage_;
        min_free_space_ = end_of_buffer - storage_;
#endif

        // Needed to prevent infinite loops in tests
        if (!blocking && min_free_space_ <= nbytes)
            return nullptr;
    }

    return producer_pos_;
}

/**
* Peek at the data available for consumption within the stagingBuffer.
* The consumer should also invoke consume() to release space back
* to the producer. This can and should be done piece-wise where a
* large peek can be consume()-ed in smaller pieces to prevent blocking
* the producer.
*
* \param[out] bytes_available
*      Number of bytes consumable
* \return
*      Pointer to the consumable space
*/
char *
StagingBuffer::peek(uint64_t *bytes_available) {
    // Save a consistent copy of producerPos
    char *cached_producer_pos = producer_pos_;

    if (cached_producer_pos < consumer_pos_) {
        *bytes_available = end_of_recorded_space_ - consumer_pos_;

        if (*bytes_available > 0)
            return consumer_pos_;

        // Roll over
        consumer_pos_ = storage_;
    }

    *bytes_available = cached_producer_pos - consumer_pos_;
    return consumer_pos_;
}


} // details

} // static_log