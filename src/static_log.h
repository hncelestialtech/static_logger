#ifndef STATIC_LOG_H
#define STATIC_LOG_H

#include <stdint.h>

namespace static_log {

// This extra namespace allows users to import only the LogLevel namespace
// via the "using" directive (Ex: using namespace static_log;)
namespace LogLevels {
    /**
     * The levels of verbosity for messages logged with #NANO_LOG.
     */
    enum LogLevel {
        // Keep this in sync with logLevelNames defined inside Log.cc.
                kSILENT_LOG_LEVEL = 0,
        /**
         * Bad stuff that shouldn't happen. The system broke its contract to
         * users in some way or some major assumption was violated.
         */
                kERROR,
        /**
         * Messages at the WARNING level indicate that, although something went
         * wrong or something unexpected happened, it was transient and
         * recoverable.
         */
                kWARNING,
        /**
         * Somewhere in between WARNING and DEBUG...
         */
                kNOTICE,
        /**
         * Messages at the DEBUG level don't necessarily indicate that anything
         * went wrong, but they could be useful in diagnosing problems.
         */
                kDEBUG,
        kNUM_LOG_LEVELS // must be the last element in the enum
    };
};

extern uint32_t io_internal;

// User API

/**
 * Preallocate the thread-local data structures needed by the
 * StaticLog system for the current thread. Although optional, it is
 * recommended to invoke this function in every thread that will use the
 * StaticLog system before the first log message.
 */
void preallocate();

/**
 * Sets the file location for the StaticLog output. All NANO_LOG statements
 * invoked after this function returns are guaranteed to be in the new file
 * location.
 *
 * An exception will be thrown if the new log file cannot be opened/created
 *
 * \param filename
 *      Where to place the log file
 */
void setLogFile(const char* filename);

/**
 * Sets the minimum logging severity level in the system. All log statements
 * of a lower log severity will be dropped completely.
 *
 * \param logLevel
 *      New Log level to set
 */
void setLogLevel(LogLevels::LogLevel logLevel);

/**
 * Returns the current minimum log severity level enforced by StaticLog
 */
LogLevels::LogLevel getLogLevel();

/**
 * Waits until all pending log statements are persisted to disk. Note that if
 * there is another logging thread continually adding new pending log
 * statements, this function may not return until all threads stop logging and
 * all the new log statements are also persisted.
 */
void sync();

/**
 * STATIC_LOG macro used for logging.
 *
 * \param severity
 *      The LogLevel of the log invocation (must be constant)
 * \param format
 *      printf-like format string (must be literal)
 * \param ...
 *      Log arguments associated with the printf-like string.
 */
#define STATIC_LOG(severity, format, ...) do { \
    constexpr int n_params = static_log::details::countFmtParams(format); \
    \
    /*** Very Important*** These must be 'static' so that we can save pointers 
     **/ \
    static constexpr std::array<static_log::details::ParamType, n_params> param_types = \
                                static_log::details::analyzeFormatString<n_params>(format); \
    static constexpr static_log::details::StaticInfo static_info =  \
                            static_log::details::StaticInfo(n_params, param_types.data(), format, severity, __FUNCTION__, __LINE__); \
    \
    if (severity > static_log::getLogLevel()) \
        break; \
    \
    /* Triggers the GNU printf checker by passing it into a no-op function.
     * Trick: This call is surrounded by an if false so that the VA_ARGS don't
     * evaluate for cases like '++i'.*/ \
    if (false) { static_log::details::checkFormat(format, ##__VA_ARGS__); } /*NOLINT(cppcoreguidelines-pro-type-vararg, hicpp-vararg)*/\
    \
    static size_t param_size[n_params]{};   \
    uint64_t previousPrecision = -1;   \
    size_t alloc_size = static_log::details::getArgSizes(param_types, previousPrecision,    \
                            param_size, ##__VA_ARGS__) + sizeof(static_log::details::LogEntry);    \
    char *write_pos = static_log::details::StaticLogBackend::reserveAlloc(alloc_size);   \
    \
    static_log::details::LogEntry *log_entry = new(write_pos) static_log::details::LogEntry(&static_info, param_size);    \
    write_pos += sizeof(static_log::details::LogEntry);    \
    static_log::details::storeArguments(param_types, param_size, &write_pos, ##__VA_ARGS__);    \
    log_entry->entry_size = static_log::details::downCast<uint32_t>(alloc_size);    \
    log_entry->timestamp = rdtsc_();  \
    \
    static_log::details::StaticLogBackend::finishAlloc(alloc_size);  \
} while(0)

} // namespace static_log

#include "static_log_front.h"

#endif // STATIC_LOG_H