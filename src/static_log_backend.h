#ifndef STATIC_LOG_BACKEND_H
#define STATIC_LOG_BACKEND_H

#include "static_log.h"
#include "static_log_common.h"

namespace static_log {
namespace details{

/**
 * Implements a circular FIFO producer/consumer byte queue that is used
 * to hold the dynamic information of a NanoLog log statement (producer)
 * as it waits for compression via the NanoLog background thread
 * (consumer). There exists a StagingBuffer for every thread that uses
 * the NanoLog system.
 */
class StagingBuffer {
public:
    /**
     * Attempt to reserve contiguous space for the producer without
     * making it visible to the consumer. The caller should invoke
     * finishReservation() before invoking reserveProducerSpace()
     * again to make the bytes reserved visible to the consumer.
     *
     * This mechanism is in place to allow the producer to initialize
     * the contents of the reservation before exposing it to the
     * consumer. This function will block behind the consumer if
     * there's not enough space.
     *
     * \param nbytes
     *      Number of bytes to allocate
     *
     * \return
     *      Pointer to at least nbytes of contiguous space
     */
    inline char *
    reserveProducerSpace(size_t nbytes) {
        ++num_allocations_;

        // Fast in-line path
        if (nbytes < min_free_space_)
            return producer_pos_;

        // Slow allocation
        return reserveSpaceInternal(nbytes);
    }

    /**
     * Complement to reserveProducerSpace that makes nbytes starting
     * from the return of reserveProducerSpace visible to the consumer.
     *
     * \param nbytes
     *      Number of bytes to expose to the consumer
     */
    inline void
    finishReservation(size_t nbytes) {
        assert(nbytes < min_free_space_);
        assert(producer_pos_ + nbytes <
                storage_ + STAGING_BUFFER_SIZE);

        min_free_space_ -= nbytes;
        producer_pos_ += nbytes;
    }

    char *peek(uint64_t *bytesAvailable);

    /**
     * Consumes the next nbytes in the StagingBuffer and frees it back
     * for the producer to reuse. nbytes must be less than what is
     * returned by peek().
     *
     * \param nbytes
     *      Number of bytes to return back to the producer
     */
    inline void
    consume(uint64_t nbytes) {
        consumer_pos_ += nbytes;
    }

    /**
     * Returns true if it's safe for the compression thread to delete
     * the StagingBuffer and remove it from the global vector.
     *
     * \return
     *      true if its safe to delete the StagingBuffer
     */
    bool
    checkCanDelete() {
        return should_deallocate_ && consumer_pos_ == producer_pos_;
    }


    uint32_t getId() const {
        return id_;
    }

    StagingBuffer(uint32_t bufferId)
            : producer_pos_(storage_)
            , end_of_recorded_space_(storage_
                                    + STAGING_BUFFER_SIZE)
            , min_free_space_(STAGING_BUFFER_SIZE)
            , cycles_producer_blocked_(0)
            , num_times_producer_blocked_(0)
            , num_allocations_(0)
            , consumer_pos_(storage_)
            , should_deallocate_(false)
            , id_(bufferId)
            , storage_() {
    }

    ~StagingBuffer() {
    }

    StagingBuffer(const StagingBuffer&)=delete;
    StagingBuffer& operator=(const StagingBuffer&)=delete;
    StagingBuffer(StagingBuffer&&)=delete;
    StagingBuffer& operator=(StagingBuffer&&)=delete;

private:

    char *reserveSpaceInternal(size_t nbytes, bool blocking = true);

    // Position within storage[] where the producer may place new data
    char *producer_pos_;

    // Marks the end of valid data for the consumer. Set by the producer
    // on a roll-over
    char *end_of_recorded_space_;

    // Lower bound on the number of bytes the producer can allocate w/o
    // rolling over the producer_pos_ or stalling behind the consumer
    uint64_t min_free_space_;

    // Number of cycles producer was blocked while waiting for space to
    // free up in the StagingBuffer for an allocation.
    uint64_t cycles_producer_blocked_;

    // Number of times the producer was blocked while waiting for space
    // to free up in the StagingBuffer for an allocation
    uint32_t num_times_producer_blocked_;

    // Number of alloc()'s performed
    uint64_t num_allocations_;

    // An extra cache-line to separate the variables that are primarily
    // updated/read by the producer (above) from the ones by the
    // consumer(below)
    char cacheline_spacer_[2*BYTES_PER_CACHE_LINE];

    // Position within the storage buffer where the consumer will consume
    // the next bytes from. This value is only updated by the consumer.
    char* volatile consumer_pos_;

    // Indicates that the thread owning this StagingBuffer has been
    // destructed (i.e. no more messages will be logged to it) and thus
    // should be cleaned up once the buffer has been emptied by the
    // compression thread.
    bool should_deallocate_;

    // Uniquely identifies this StagingBuffer for this execution. It's
    // similar to ThreadId, but is only assigned to threads that NANO_LOG).
    uint32_t id_;

    // Backing store used to implement the circular queue
    char storage_[STAGING_BUFFER_SIZE];

    friend class StaticLogBackend;
};


class StaticLogBackend {
public:
    static LogLevels::LogLevel getLogLevel() {
        return {};
    }

    /**
     * Allocate thread-local space for the generated C++ code to store an
     * uncompressed log message, but do not make it available for compression
     * yet. The caller should invoke finishAlloc() to make the space visible
     * to the compression thread and this function shall not be invoked
     * again until the corresponding finishAlloc() is invoked first.
     *
     * Note this will block of the buffer is full.
     *
     * \param nbytes
     *      number of bytes to allocate in the
     *
     * \return
     *      pointer to the allocated space
     */
    static inline char *
    reserveAlloc(size_t nbytes) {
        if (staging_buffer_ == nullptr)
            nanoLogSingleton.ensureStagingBufferAllocated();

        return staging_buffer_->reserveProducerSpace(nbytes);
    }

    /**
     * Complement to reserveAlloc, makes the bytes previously
     * reserveAlloc()-ed visible to the compression/output thread.
     *
     * \param nbytes
     *      Number of bytes to make visible
     */
    static inline void
    finishAlloc(size_t nbytes) {
        staging_buffer_->finishReservation(nbytes);
    }

private:
    class StagingBuffer;
    static __thread StagingBuffer *staging_buffer_;
};

} // details

} // static_log

#endif // STATIC_LOG_BACKEND_H