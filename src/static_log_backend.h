#ifndef STATIC_LOG_BACKEND_H
#define STATIC_LOG_BACKEND_H

#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <memory>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <thread>
#include <iostream>
#include <atomic>

#include "static_log.h"
#include "static_log_common.h"

namespace static_log {
namespace details{

extern uint32_t poll_interval_no_work;

class StagingBufferDestroyer;

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
                storage_ + kSTAGING_BUFFER_SIZE);

        min_free_space_ -= nbytes;
        producer_pos_ += nbytes;
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
                                    + kSTAGING_BUFFER_SIZE)
            , min_free_space_(kSTAGING_BUFFER_SIZE)
            , cycles_producer_blocked_(0)
            , num_times_producer_blocked_(0)
            , num_allocations_(0)
            , consumer_pos_(storage_)
            , should_deallocate_(false)
            , id_(bufferId)
            , storage_() {
    }

    ~StagingBuffer() {
        should_deallocate_ = true;
    }

    StagingBuffer(const StagingBuffer&)=delete;
    StagingBuffer& operator=(const StagingBuffer&)=delete;
    StagingBuffer(StagingBuffer&&)=delete;
    StagingBuffer& operator=(StagingBuffer&&)=delete;

private:
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
    char storage_[kSTAGING_BUFFER_SIZE];

    friend class StaticLogBackend;
    friend class StagingBufferDestroyer;
};

class StaticLogBackend {
public:
    ~StaticLogBackend();

    /**
    * The write cache work queue is allocated in advance, and if the function
    * is not called, the request of the queue will be postponed until the first 
    * write log 
    */
    static void preallocate()
    {
        logger_.ensureStagingBufferAllocated();
    }

    static LogLevels::LogLevel getLogLevel()
    {
        return logger_.current_log_level_;
    }

    /**
    * Set up a log write file
    * 
    * Generally, it is called after the logger is initialized, and since the initialized
    * logger will create an file named 'log.txt', there will be multiple log files if 
    * the function is called
    * 
    * \param log_file
    *   new log file path
    */
    static void setLogFile(const char* log_file)
    {
        std::unique_lock<std::mutex> lock(logger_.buffer_mutex_);
        logger_.is_stop_ = true;
        logger_.is_exit_ = true;
        lock.unlock();
        logger_.fdflush_.join();
        lock.lock();
        logger_.is_exit_ = false;
        if (logger_.outfd_ != -1) {
            close(logger_.outfd_);
        }
        logger_.outfd_ = -1;
        logger_.outfd_ = open(log_file, O_RDWR|O_CREAT, 0666);
        if(logger_.outfd_ == -1) {
            fprintf(stderr, "%s: Failed to open file %s\n", __FUNCTION__, log_file);
            return;
        }
        lock.unlock();
        logger_.fdflush_ = std::move(std::thread(&StaticLogBackend::ioPoll, &logger_));
    }

    // Wake up backend worker
    static void sync()
    {
        std::unique_lock<std::mutex> lock(logger_.buffer_mutex_);
        logger_.wake_up_cond_.notify_one();
    }

    /**
    * Sets the minimum log level new NANO_LOG messages will have to meet before
    * they are saved. Anything lower will be dropped.
    *
    * \param log_level
    *      LogLevel enum that specifies the minimum log level.
    */
    static void setLogLevel(LogLevels::LogLevel log_level) {
        if (log_level < 0)
            log_level = static_cast<LogLevels::LogLevel>(0);
        else if (log_level >= LogLevels::LogLevel::kNUM_LOG_LEVELS)
            log_level = static_cast<LogLevels::LogLevel>(LogLevels::LogLevel::kNUM_LOG_LEVELS - 1);
        logger_.current_log_level_ = log_level;
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
            logger_.ensureStagingBufferAllocated();

        return logger_.staging_buffer_->reserveProducerSpace(nbytes);
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
        logger_.staging_buffer_->finishReservation(nbytes);
    }

    void processLogBuffer(StagingBuffer* stagingbuffer);

private:
    StaticLogBackend();
    StaticLogBackend(const StaticLogBackend&)=delete;
    StaticLogBackend& operator=(const StaticLogBackend&)=delete;
    StaticLogBackend(StaticLogBackend&&)=delete;
    StaticLogBackend& operator=(StaticLogBackend&&)=delete;

    /**
     * Allocates thread-local structures if they weren't already allocated.
     * This is used by the generated C++ code to ensure it has space to
     * log uncompressed messages to and by the user if they wish to
     * preallocate the data structures on thread creation.
     */
    inline void ensureStagingBufferAllocated()
    {
        if (staging_buffer_ == nullptr) {
            std::unique_lock<std::mutex> guard(buffer_mutex_);
            uint32_t bufferId = next_buffer_id_++;

            // Unlocked for the expensive StagingBuffer allocation
            guard.unlock();
            staging_buffer_ = new StagingBuffer(bufferId);
            guard.lock();

            thread_buffers_.push_back(staging_buffer_);
            destroyer_.createDestroyer();
        }
    }
    
    /**
    * Traverse the log buffer queue and write to the acquired logs, 
    * all using periodic timing behavior
    */
    void ioPoll();

private:
    static __thread StagingBuffer *staging_buffer_;

    class StagingBufferDestroyer {
    public:
        StagingBufferDestroyer() {}
        ~StagingBufferDestroyer() {
            if (StaticLogBackend::staging_buffer_ != nullptr) {
                StaticLogBackend::staging_buffer_->should_deallocate_ = true;
            }
        }
        void createDestroyer() {}
    };
    static thread_local StagingBufferDestroyer destroyer_;

    // Minimum log level that RuntimeLogger will accept. Anything lower will
    // be dropped.
    LogLevels::LogLevel current_log_level_;

    // Used to synchonize the log buffer
    std::mutex buffer_mutex_;
    // Used to synchonize the backend worker
    std::mutex cond_mutex_;
    std::condition_variable wake_up_cond_;
    uint32_t   next_buffer_id_;

    // Globally the thread-local stagingBuffers
    std::vector<StagingBuffer *> thread_buffers_;

    // Flag signaling the thread to stop running.
    std::atomic<bool> is_stop_;

    // Only used in setlogFile
    std::atomic<bool> is_exit_;

    // Backend worker who really sync the message into file
    std::thread fdflush_;

    // The file fd which sync log message to disk
    int     outfd_;

    // Stores the formatted log content
    char*   log_buffer_;
    size_t  bufflen_;
private:
    static StaticLogBackend logger_;
};

} // details

} // static_log

#endif // STATIC_LOG_BACKEND_H