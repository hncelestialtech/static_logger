#include "static_log_backend.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include <chrono>

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

void StaticLogBackend::io_poll_backend()
{
    while (!is_stop_) {
        while(!thread_buffers_.empty()) {
            
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