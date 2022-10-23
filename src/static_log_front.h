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

extern volatile uint64_t log_id;

} // details

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
    constexpr int n_params = static_log::internal::utils::countFmtParams(format); \
    \
    /*** Very Important*** These must be 'static' so that we can save pointers 
     **/ \
    static constexpr std::array<static_log::internal::ParamType, n_params> param_types = \
                                static_log::internal::utils::analyzeFormatString<n_params>(format); \
    static constexpr static_log::internal::StaticInfo static_info =  \
                            static_log::internal::StaticInfo(n_params, param_types.data(), format, severity, __FUNCTION__, __LINE__); \
    \
    if (severity > static_log::getLogLevel()) \
        break; \
    \
    /* Triggers the GNU printf checker by passing it into a no-op function.
     * Trick: This call is surrounded by an if false so that the VA_ARGS don't
     * evaluate for cases like '++i'.*/ \
    if (false) { static_log::internal::utils::checkFormat(format, ##__VA_ARGS__); } /*NOLINT(cppcoreguidelines-pro-type-vararg, hicpp-vararg)*/\
    \
    static size_t param_size[n_params]{};   \
    uint64_t previousPrecision = -1;   \
    size_t alloc_size = static_log::internal::utils::getArgSizes(param_types, previousPrecision,    \
                            param_size, ##__VA_ARGS__) + sizeof(static_log::internal::LogEntry);    \
    char *write_pos = static_log::details::StaticLogBackend::reserveAlloc(alloc_size);   \
    \
    static_log::internal::LogEntry *log_entry = new(write_pos) static_log::internal::LogEntry(&static_info, param_size);    \
    write_pos += sizeof(static_log::internal::LogEntry);    \
    static_log::internal::utils::storeArguments(param_types, param_size, &write_pos, ##__VA_ARGS__);    \
    log_entry->entry_size = static_log::internal::utils::downCast<uint32_t>(alloc_size);    \
    log_entry->timestamp = static_log::details::log_id++;  \
    \
    static_log::details::StaticLogBackend::finishAlloc(alloc_size);  \
} while(0)

} // static_log

#endif // STATIC_LOGGER_H