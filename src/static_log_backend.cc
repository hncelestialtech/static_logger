#include "static_log_backend.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <chrono>

#include "static_log_internal.h"
#include "static_log_utils.h"

namespace static_log {

namespace details {
__thread StagingBuffer *StaticLogBackend::staging_buffer_ = nullptr;
StaticLogBackend StaticLogBackend::logger_;

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

    fdflush_ = std::thread(&StaticLogBackend::io_poll_backend, this);
}

StaticLogBackend::~StaticLogBackend()
{
    is_stop_ = false;
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
    const int prefix_len = 30;
    const int nano_bits = 9;
    *raw_data = '[';
    raw_data++;
    uint64_t nano = timestamp % 1000000000;
    timestamp = timestamp / 1000000000;
    struct tm* tm_now = localtime((time_t*)&timestamp);
    memcpy(raw_data, std::to_string(tm_now->tm_year + 1900).c_str(), 4);
    raw_data += 4;
    *raw_data = '-';
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

static int
process_fmt(
        const char* fmt, 
        const int num_params, 
        const internal::ParamType* param_types, 
        const char* param_list, 
        char* log_buffer, size_t buflen)
{
    int origin_len = buflen;
    size_t fmt_list_len = strlen(fmt);
    int pos = 0;
    int param_idx = 0;
    bool success = true;
    while (pos < fmt_list_len) {
        if (fmt[pos] != '%') {
            *log_buffer++ = *fmt;
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
                while (!internal::utils::isTerminal(fmt[pos]))
                    fmt_single_len++;
                char* fmt_single;
                char static_fmt_cache[100];
                bool dynamic_fmt = false;
                if (fmt_single_len >= 100) {
                    memset(static_fmt_cache, 0, 100);
                    fmt_single = static_fmt_cache;
                } else {
                    fmt_single = (char*)malloc(fmt_single_len);
                    dynamic_fmt = true;
                }
                memcpy(fmt_single, fmt + pos, fmt_single_len);
                pos += fmt_single_len;
                size_t log_fmt_len = 0;

                if (param_idx < num_params) {
                    if (param_types[param_idx] > internal::ParamType::NON_STRING) {
                        uint32_t string_size = *(uint32_t*)param_list;
                        param_list += sizeof(uint32_t);
                        char *param_str = (char*)malloc(string_size);
                        memcpy(param_str, param_list, string_size);
                        log_fmt_len = snprintf(log_buffer, buflen, fmt_single, param_str);
                        free(param_str);
                    }
                    else {
#pragma GCC diagnostic ignored "-Wformat"
                        uint64_t param = *(uint64_t*)param_list;
                        param_list += sizeof(uint64_t);
                        log_fmt_len = snprintf(log_buffer, buflen, fmt_single, param);
#pragma GCC diagnostic pop
                    }
                    log_buffer += log_fmt_len;
                    buflen -= log_fmt_len;
                } else {
                    fprintf(stderr, "Failed to fmt log");
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
        auto prefix_len = generateTimePrefix(log_entry->timestamp, log_content_cache);
        reserved -= prefix_len;
        const char* fmt = log_entry->static_info->format;
        int len = process_fmt(fmt, log_entry->static_info->num_params, 
                    log_entry->static_info->param_types,
                    (char*)log_entry + sizeof(internal::LogEntry),
                    log_content_cache + prefix_len, reserved - prefix_len);
        char* log = log_content_cache + prefix_len + len;
        *log = '\n';
        if (len != -1) {
            write(StaticLogBackend::logger_.outfd_, log_content_cache, len + 1);
        }
    }
}

void 
StaticLogBackend::io_poll_backend()
{
    int bf_idx = 0;
    while (!is_stop_) {
        while(!thread_buffers_.empty()) {
            auto thread_buffer = thread_buffers_[bf_idx];
            if (thread_buffer->isAlive()) {
                processLogBuffer(thread_buffer);
            }
            else {
                thread_buffers_.erase(thread_buffers_.begin() + bf_idx);
                if (thread_buffers_.empty()) 
                    break;
                bf_idx--;
            }
            bf_idx++;
        }
        std::unique_lock<std::mutex> lock(cond_mutex_);
        wake_up_cond_.wait_for(lock, std::chrono::microseconds(poll_interval_no_work));
    }
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