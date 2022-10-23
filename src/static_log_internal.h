#ifndef STATIC_LOG_INTERNAL_H
#define STATIC_LOG_INTERNAL_H

#include <stdint.h>
#include <cstddef>
#include <array>

#include "static_log.h"

namespace static_log {

namespace internal {

/**
 * Describes the type of parameter that would be passed into a printf-like
 * function.
 *
 * These types are optimized to store enough information to determine
 * (a) whether a 'const char*' parameter indicates string (%s) or not (%p)
 * (b) if a string parameter (%s) needs to be truncated due to precision
 * (c) whether a parameter is a dynamic precision/width specifier
 */
enum ParamType:  int32_t {
    // Indicates that there is a problem with the parameter
    INVALID = -6,

    // Indicates a dynamic width (i.e. the '*' in  %*.d)
    DYNAMIC_WIDTH = -5,

    // Indicates dynamic precision (i.e. the '*' in %.*d)
    DYNAMIC_PRECISION = -4,

    // Indicates that the parameter is not a string type (i.e. %d, %lf)
    NON_STRING = -3,

    // Indicates the parameter is a string and has a dynamic precision
    // (i.e. '%.*s' )
    STRING_WITH_DYNAMIC_PRECISION = -2,

    // Indicates a string with no precision specified (i.e. '%s' )
    STRING_WITH_NO_PRECISION = -1,

    // All non-negative values indicate a string with a precision equal to its
    // enum value casted as an int32_t
    STRING = 0
};

struct StaticInfo {
    constexpr StaticInfo(
        const int num_params,
        const ParamType* param_types,
        const char* format,
        const static_log::LogLevels::LogLevel log_level,
        const char* function_name,
        const uint64_t line
    ):num_params(num_params),
    param_types(param_types),
    format(format),
    log_level(log_level),
    function_name(function_name),
    line(line)
    {}
    // Number of arguments required for the log invocation
    const int num_params;

    // Mapping of parameter index (i.e. order in which it appears in the
    // argument list starting at 0) to parameter type as inferred from the
    // printf log message invocation
    const ParamType* param_types;

    // printf format string associated with the log invocation
    const char* format;

    static_log::LogLevels::LogLevel log_level;    
    
    const char* function_name;

    const uint64_t line;
};

struct LogEntry {
    LogEntry(const StaticInfo* static_info, const size_t* param_size): 
        static_info(static_info),
        param_size(param_size)
    {}

    uint64_t timestamp;
    uint64_t entry_size;
    const StaticInfo* static_info;
    const size_t* param_size;
};

} // namespace internal

} // namespace static_log

#endif // STATIC_LOG_INTERNAL_H