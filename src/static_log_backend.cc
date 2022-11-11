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
#include <chrono>

#include "static_log_internal.h"
#include "tsc_clock.h"

namespace static_log {

namespace details {

static const char* log_level_str[] = {
    "non",
    "error",
    "warn",
    "notice",
    "debug"
};

volatile int StaticLogBackend::io_internal_ = 0;
__thread StagingBuffer *StaticLogBackend::staging_buffer_ = nullptr;
StaticLogBackend StaticLogBackend::logger_;
thread_local StaticLogBackend::StagingBufferDestroyer StaticLogBackend::destroyer_{};

#define DEFAULT_INTERVAL 10
uint32_t poll_interval_no_work = DEFAULT_INTERVAL;

#define DEFAULT_LOGFILE     "log.txt"
#define DEFALT_CACHE_SIZE 1024 * 1024

StaticLogBackend::StaticLogBackend():
    current_log_level_(LogLevels::kDEBUG),
    buffer_mutex_(),
    cond_mutex_(),
    wake_up_cond_(),
    next_buffer_id_(0),
    thread_buffers_(),
    is_stop_(false),
    is_exit_(false),
    outfd_(-1),
    log_buffer_(NULL),
    bufflen_(0)
{
    const char * logfile = DEFAULT_LOGFILE;
    outfd_ = open(logfile, O_RDWR|O_CREAT, 0666);
    if (outfd_ < 0) {
        fprintf(stderr, "Failed to open log file\n");
        exit(-1);
    }

    log_buffer_ = (char*)malloc(DEFALT_CACHE_SIZE);
    if (log_buffer_ == NULL) {
        fprintf(stderr, "Failed to create log buffer\n");
        exit(-1);
    }
    bufflen_ = DEFALT_CACHE_SIZE;

    fdflush_ = std::thread(&StaticLogBackend::ioPoll, this);
}

StaticLogBackend::~StaticLogBackend()
{
    is_stop_ = true;
    if(fdflush_.joinable())
        fdflush_.join();
    if (outfd_ != -1)
        close(outfd_);
    if (log_buffer_)
        free(log_buffer_);
    bufflen_ = 0;
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

#define TIEMSTAMP_PREFIX_LEN 31
// [xxxx-xx-xx-hh:mm:ss.xxxxxxxxx]
int
generateTimePrefix(uint64_t timestamp, char* raw_data) {
    // char* origin = raw_data;
    int prefix_len{0};
    const int nano_bits = 9;
    static_assert(TIEMSTAMP_PREFIX_LEN < DEFALT_CACHE_SIZE, "default buffer size is smaller than time prefix len");
    *raw_data = '[';    prefix_len += 1;
    raw_data++;
    uint64_t nano = timestamp % 1000000000;
    timestamp = timestamp / 1000000000;
    struct tm* tm_now = localtime((time_t*)&timestamp);
    memcpy(raw_data, std::to_string(tm_now->tm_year + 1900).c_str(), 4);
    raw_data += 4; prefix_len += 4;
    *raw_data++ = '-'; prefix_len += 1;
    convertInt2Str(tm_now->tm_mon + 1, raw_data);   prefix_len += 2;
    *raw_data++ = '-'; prefix_len += 1;
    convertInt2Str(tm_now->tm_mday, raw_data);  prefix_len += 2;
    *raw_data++ = '-';  prefix_len += 1;
    convertInt2Str(tm_now->tm_hour, raw_data);  prefix_len += 2;
    *raw_data++ = ':';  prefix_len += 1;
    convertInt2Str(tm_now->tm_min, raw_data);   prefix_len += 2;
    *raw_data++ = ':';  prefix_len += 1;
    convertInt2Str(tm_now->tm_sec, raw_data);   prefix_len += 2;
    *raw_data++ = '.';  prefix_len += 1;
    int nanolen = strlen(std::to_string(nano).c_str());
    memcpy(raw_data + nano_bits - nanolen , std::to_string(nano).c_str(), nanolen);
    if (nanolen < nano_bits) {
        int bits = nano_bits - nanolen;
        for(int i = 0; i < bits; ++i)
            *raw_data++ = '0';
        raw_data += nanolen;
    } else {
        raw_data += nano_bits;
    }
    prefix_len += nano_bits;
    *raw_data = ']';    prefix_len += 1;
    assert(prefix_len == TIEMSTAMP_PREFIX_LEN);
    return prefix_len;
}

/**
 * 63 significant initial characters in an internal identifier or a macro name
 * https://en.cppreference.com/w/c/language/identifier
 */
#define MAX_FUNC_NAME 63
#define MAX_LINE    128
static int
generateCallInfoPrefix(const StaticInfo* static_info, char* raw)
{
    constexpr int max_call_info_len = 1 + 5 + 2 + MAX_FUNC_NAME + 2 + MAX_LINE + 1; // [LEVEL][FUNC_NAME][LINE]
    static_assert(DEFALT_CACHE_SIZE - TIEMSTAMP_PREFIX_LEN > max_call_info_len, "log buffer is too small to store func infomation\n");
    int prefix_len = 0;
    *raw++ = '[';
    prefix_len++;
    auto level_len = strlen(log_level_str[(uint32_t)static_info->log_level]);
    memcpy(raw, log_level_str[(uint32_t)static_info->log_level], level_len);
    raw += level_len;
    prefix_len += level_len;
    *raw++ = ']';
    prefix_len++;
    *raw++ = '[';
    prefix_len++;
    auto fn_len = strlen(static_info->function_name);
    memcpy(raw, static_info->function_name, fn_len);
    raw += fn_len;
    prefix_len += strlen(static_info->function_name);
    *raw++ = ']';
    prefix_len++;
    *raw++ = '[';
    prefix_len++;
    auto line_len = strlen(std::to_string(static_info->line).c_str());
    memcpy(raw, std::to_string(static_info->line).c_str(), line_len);
    prefix_len += line_len;
    raw += line_len;
    *raw++ = ']';
    prefix_len++;
    return prefix_len;
}

static int
resize_log_buffer(char*&  log_buffer, size_t& old_size, size_t new_size)
{
    char* buffer = (char*)malloc(new_size);
    if (buffer == NULL) {
        fprintf(stderr, "Failed to resize log buffer, wanted to alloc %lu\n", new_size);
        return -1;
    }
    memcpy(buffer, log_buffer, old_size);
    free(log_buffer);
    log_buffer = buffer;
    old_size = new_size;
    return 0;
}

#define CHECK_LOG_BUFFER_REALLOC() do { \
    if (fmt_len >= reserved) {   \
        int ret = resize_log_buffer(log_buffer, log_buffer_len, (fmt_len << 1) + log_buffer_len - reserved);   \
        if (ret < 0) return -1; \
        reserved = log_buffer_len - start_pos;  \
        goto retry; \
    }   \
} while(0)

#pragma GCC diagnostic ignored "-Wformat"
static int
decodeNonStringFmt(
    char*& log_buffer, 
    size_t& log_buffer_len,
    size_t& reserved,
    size_t start_pos,
    char * fmt, 
    const char* param, 
    size_t param_size)
{
    size_t fmt_len = 0;
    char terminal_flag = fmt[strlen(fmt) - 1];
retry:
    switch (param_size) {
    case sizeof(char):
        {
            const char* param8 = param;
            if (terminal_flag == 'c') {
                fmt_len = snprintf(log_buffer + start_pos, reserved, fmt, *(char*)param8);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else if (terminal_flag == 'd' || terminal_flag == 'i') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(int8_t*)param8);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(uint8_t*)param8);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else 
                fprintf(stderr, "Failed to parse fmt with a one bytes long param\n");
            break;
        }
    case sizeof(uint16_t):
        {
            const uint16_t* param16 = (uint16_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(int16_t*)param16);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(uint16_t*)param16);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else 
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    case sizeof(uint32_t):
        {
            const uint32_t* param32 = (uint32_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(int32_t*)param32);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(uint32_t*)param32);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else if (terminal_flag == 'f' || terminal_flag == 'F' || terminal_flag == 'e' ||terminal_flag == 'e' || 
                terminal_flag == 'E' || terminal_flag == 'g' || terminal_flag == 'G'|| terminal_flag == 'a' ||terminal_flag == 'A') {
                static_assert(sizeof(float) == sizeof(uint32_t));
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(float*)param32);
                CHECK_LOG_BUFFER_REALLOC();
            }            
            else 
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    case sizeof(uint64_t):
        {
            const uint64_t* param64 = (uint64_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(int64_t*)param64);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(uint64_t*)param64);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else if (terminal_flag == 'f' || terminal_flag == 'F' || terminal_flag == 'e' ||terminal_flag == 'e' || 
                terminal_flag == 'E' || terminal_flag == 'g' || terminal_flag == 'G'|| terminal_flag == 'a' ||terminal_flag == 'A') {
                static_assert(sizeof(double) == sizeof(uint64_t));
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(double*)param64);
                CHECK_LOG_BUFFER_REALLOC();
            }
            else
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    default:
        fprintf(stderr, "Failed to decode fmt param, got size %lu\n", param_size);
        break;
    }
    return fmt_len;
}
#pragma GCC diagnostic pop

#define DEFAULT_PARAM_CACHE_SIZE 1024
static int
decodeStringFmt(char*& log_buffer, size_t& bufferlen, size_t& reserved, size_t start_pos, const char* param, size_t param_size, const char* fmt)
{
    assert((char*)log_buffer - (char*)start_pos == bufferlen - reserved);
    char string_param_cache[DEFAULT_PARAM_CACHE_SIZE];
    bool dynamic_alloc = false;
    char* param_buffer = string_param_cache;
    int ret = 0;
    if (param_size >= DEFAULT_PARAM_CACHE_SIZE) {
        param_buffer = (char*)malloc(param_size + 1); // strlen(str) + '\0'
        if (param_buffer == NULL) {
            fprintf(stderr, "Failed to alloc param buffer\n");
            return -1;
        }
        dynamic_alloc = true;
    }
    memcpy(param_buffer, param, param_size);
    param_buffer[param_size] = '\0';
    size_t needed_fmt_len = snprintf(log_buffer + start_pos, reserved, fmt, param_buffer);
    if (needed_fmt_len > reserved) {
        size_t new_log_buflen = needed_fmt_len + 1 + bufferlen - reserved;
        char* tmp = (char*)malloc(new_log_buflen);
        if (tmp == NULL) {
            fprintf(stderr, "Failed to realloc log buffer, needed alloc size %lu\n", needed_fmt_len);
            ret = -1;
            goto out;
        }
        reserved = new_log_buflen - bufferlen + reserved;
        bufferlen = new_log_buflen;
        memcpy(tmp, log_buffer, start_pos);
        snprintf(tmp + start_pos, needed_fmt_len + 1, fmt, param_buffer);
        free(log_buffer);
        log_buffer = tmp;
    }
    ret = needed_fmt_len;
out:
    if (dynamic_alloc)
        free(param_buffer);
    return ret;
}

static int
process_fmt(
        const char* fmt, 
        const int num_params, 
        const ParamType* param_types,
        size_t* param_size_list,
        const char* param_list, 
        char*& log_buffer, size_t& buflen, size_t start_pos)
{
    char* log_pos = log_buffer + start_pos;
    size_t fmt_list_len = strlen(fmt);
    size_t reserved = buflen - start_pos;
    size_t pos = 0;
    int param_idx = 0;
    bool success = true;
    while (pos < fmt_list_len) {
        if (fmt[pos] != '%') {
            *log_pos++ = fmt[pos++];
            reserved--;
            continue;
        } else {
            ++pos;
            int fmt_single_len = 1;
            if (fmt[pos] == '%') {
                *log_pos++ = '%';
                reserved--;
                ++pos;
                continue;
            } else {
                int fmt_start_pos = pos - 1;
                while (!isTerminal(fmt[pos])) {
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
                int log_fmt_len = 0;

                if (param_idx < num_params) {
                    if (param_types[param_idx] > ParamType::kNON_STRING) {
                        uint32_t string_size = *(uint32_t*)param_list;
                        param_list += sizeof(uint32_t);
                        log_fmt_len = decodeStringFmt(log_buffer, buflen, reserved, log_pos - log_buffer, param_list, string_size, fmt_single);
                        param_list = param_list + sizeof(uint32_t) + string_size;
                    }
                    else {
                        log_fmt_len = decodeNonStringFmt(log_buffer, buflen, reserved, log_pos - log_buffer, fmt_single, param_list, param_size_list[param_idx]);
                        if (log_fmt_len == -1)  {
                            success = false;
                            break;
                        }
                        param_list += param_size_list[param_idx];
                    }
                    log_pos += log_fmt_len;
                    reserved -= log_fmt_len;
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
    return success? buflen - reserved: -1;
}

void 
StaticLogBackend::processLogBuffer(StagingBuffer* stagingbuffer)
{
    uint64_t bytes_available = 0;
    char* raw_data = stagingbuffer->peek(&bytes_available);
    if (bytes_available > 0) {
        LogEntry *log_entry = (LogEntry *)raw_data;
        log_entry->timestamp = rdns();
        auto prefix_ts_len = generateTimePrefix(log_entry->timestamp, log_buffer_);
        // log_content_cache[prefix_len] = '\0';
        auto prefix_callinfo_len = generateCallInfoPrefix(log_entry->static_info, log_buffer_ + prefix_ts_len);
        const char* fmt = log_entry->static_info->format;
        int len = process_fmt(fmt, 
                    log_entry->static_info->num_params, 
                    log_entry->static_info->param_types,
                    (size_t*)log_entry->param_size,
                    (char*)log_entry + sizeof(LogEntry),
                    log_buffer_, bufflen_, prefix_ts_len + prefix_callinfo_len);
        char* log = log_buffer_ + prefix_ts_len + prefix_callinfo_len + len;
        *log = '\n';
        if (len != -1 && outfd_ != -1) {
            write(StaticLogBackend::logger_.outfd_, log_buffer_, prefix_ts_len + prefix_callinfo_len + len + 1);
            stagingbuffer->consume(log_entry->entry_size);
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

    // walkLogBuffer();
    std::unique_lock<std::mutex> guard(buffer_mutex_);
    while(!is_stop_ || !thread_buffers_.empty()) {
        if (is_exit_) return;
        guard.unlock();
        std::pair<uint64_t, static_log::details::StagingBuffer *> earliest_thead_buffer{UINT64_MAX, nullptr};
        guard.lock();
        for(size_t i = 0; i < thread_buffers_.size(); ++i) {
            auto thread_buffer = thread_buffers_[i];
            if (!thread_buffer->checkCanDelete()) {
                uint64_t bytes_available = 0;
                char* raw_data = thread_buffer->peek(&bytes_available);
                if (bytes_available > 0) {
                    LogEntry *log_entry = (LogEntry *)raw_data;
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
        } else {
            wake_up_cond_.wait_for(guard, std::chrono::microseconds(getIOInternal()));
        }
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
    const char *end_of_buffer = storage_ + kSTAGING_BUFFER_SIZE;

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