#ifndef STATIC_LOGGER_H
#define STATIC_LOGGER_H

#include <assert.h>

#include "static_log.h"
#include "static_log_internal.h"
#include "static_log_utils.h"
#include "static_log_backend.h"

#include "tsc_clock.h"

namespace static_log {

namespace details {

/**
 * Logs a log message in the NanoLog system given all the static and dynamic
 * information associated with the log message. This function is meant to work
 * in conjunction with the #define-d NANO_LOG() and expects the caller to
 * maintain a permanent mapping of logId to static information once it's
 * assigned by this function.
 *
 * \tparam N
 *      length of the format string (automatically deduced)
 * \tparam M
 *      length of the paramTypes array (automatically deduced)
 * \tparam Ts
 *      Types of the arguments passed in for the log (automatically deduced)
 *
 * \param logId[in/out]
 *      LogId that should be permanently associated with the static information.
 *      An input value of -1 indicates that NanoLog should persist the static
 *      log information and assign a new, globally unique identifier.
 * \param filename
 *      Name of the file containing the log invocation
 * \param linenum
 *      Line number within filename of the log invocation.
 * \param severity
 *      LogLevel severity of the log invocation
 * \param format
 *      Static printf format string associated with the log invocation
 * \param num_nibbles
 *      Number of nibbles needed to store all the arguments (derived from
 *      the format string).
 * \param param_types
 *      An array indicating the type of the n-th format parameter associated
 *      with the format string to be processed.
 *      *** THIS VARIABLE MUST HAVE A STATIC LIFETIME AS PTRS WILL BE SAVED ***
 * \param args
 *      Argument pack for all the arguments for the log invocation
 */
template<long unsigned int N, int M, typename... Ts>
inline void
log(const char *filename,
    const int linenum,
    const LogLevels::LogLevel severity,
    const char (&format)[M],
    const int num_nibbles,
    const std::array<internal::ParamType, N>& param_types,
    Ts... args)
{
    assert(N == static_cast<uint32_t>(sizeof...(Ts)));

    uint64_t previousPrecision = -1;
    uint64_t timestamp = rdtsc();
    size_t stringSizes[N + 1] = {}; //HACK: Zero length arrays are not allowed
    size_t alloc_size = internal::utils::getArgSizes(param_types, previousPrecision,
                            stringSizes, args...) + sizeof(internal::LogEntry);
    
    char *write_pos = StaticLogBackend::reserveAlloc(alloc_size);
    auto original_write_pos = write_pos;
    
    internal::LogEntry *log_entry = new(write_pos) internal::LogEntry(N, &param_types);
    write_pos += sizeof(internal::LogEntry);

    internal::utils::storeArguments(param_types, stringSizes, &write_pos, args...);

    log_entry->timestamp = timestamp;
    log_entry->entry_size = internal::utils::downCast<uint32_t>(alloc_size);

    assert(alloc_size == internal::utils::downCast<uint32_t>((write_pos - original_write_pos)));
    StaticLogBackend::finishAlloc(alloc_size);
}

} // details

/**
 * STATIC_LOG macro used for logging.
 *
 * \param severity
 *      The LogLevel of the log invocation (must be constant)
 * \param format
 *      printf-like format string (must be literal)
 * \param ...UNASSIGNED_LOGID
 *      Log arguments associated with the printf-like string.
 */
#define STATIC_LOG(severity, format, ...) do { \
    constexpr int numNibbles = internal::utils::getNumNibblesNeeded(format); \
    constexpr int nParams = internal::utils::countFmtParams(format); \
    \
    /*** Very Important*** These must be 'static' so that we can save pointers
     * to these variables and have them persist beyond the invocation.
     * The static logId is used to forever associate this local scope (tied
     * to an expansion of #NANO_LOG) with an id and the paramTypes array is
     * used by the compression function, which is invoked in another thread
     * at a much later time. */ \
    static constexpr std::array<internal::ParamType, nParams> paramTypes = \
                                internal::utils::analyzeFormatString<nParams>(format); \
    \
    if (static_log::severity > static_log::getLogLevel()) \
        break; \
    \
    /* Triggers the GNU printf checker by passing it into a no-op function.
     * Trick: This call is surrounded by an if false so that the VA_ARGS don't
     * evaluate for cases like '++i'.*/ \
    if (false) { internal::utils::checkFormat(format, ##__VA_ARGS__); } /*NOLINT(cppcoreguidelines-pro-type-vararg, hicpp-vararg)*/\
    \
    details::log(logId, __FILE__, __LINE__, NanoLog::severity, format, \
                            numNibbles, paramTypes, ##__VA_ARGS__); \
} while(0)

} // static_log

#endif // STATIC_LOGGER_H