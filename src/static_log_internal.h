#ifndef STATIC_LOG_INTERNAL_H
#define STATIC_LOG_INTERNAL_H

#include <stdint.h>
#include <stdint.h>
#include <string.h>

#include <cstddef>
#include <array>
#include <utility>
#include <stdexcept>
#include <limits>
#include <iostream>

#include "static_log.h"
#include "static_log_common.h"

namespace static_log {

namespace details {

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
    kINVALID = -6,

    // Indicates a dynamic width (i.e. the '*' in  %*.d)
    kDYNAMIC_WIDTH = -5,

    // Indicates dynamic precision (i.e. the '*' in %.*d)
    kDYNAMIC_PRECISION = -4,

    // Indicates that the parameter is not a string type (i.e. %d, %lf)
    kNON_STRING = -3,

    // Indicates the parameter is a string and has a dynamic precision
    // (i.e. '%.*s' )
    kSTRING_WITH_DYNAMIC_PRECISION = -2,

    // Indicates a string with no precision specified (i.e. '%s' )
    kSTRING_WITH_NO_PRECISION = -1,

    // All non-negative values indicate a string with a precision equal to its
    // enum value casted as an int32_t
    kSTRING = 0
};

/**
 * Describes the type of static information that will be printed in log
 * 
 */
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

    // log level
    static_log::LogLevels::LogLevel log_level;    
    
    // function name
    const char* function_name;

    // Log print line number
    const uint64_t line;
};

struct LogEntry {
    LogEntry(const StaticInfo* static_info, const size_t* param_size): 
        static_info(static_info),
        param_size(param_size)
    {}

    // The time at which the log was recorded
    //  Since it is an asynchronous log, this 
    // time is not the call log time, but the 
    // time of background processing
    uint64_t timestamp;
    // log buffer size
    uint64_t entry_size;

    const StaticInfo* static_info;
    // parameter size array
    const size_t* param_size;
};

/**
 * No-Op function that triggers the GNU preprocessor's format checker for
 * printf format strings and argument parameters.
 *
 * \param format
 *      printf format string
 * \param ...
 *      format parameters
 */
static void
STATICLOG_PRINTF_FORMAT_ATTR(1, 2)
checkFormat(STATICLOG_PRINTF_FORMAT const char *, ...) {}

/**
 * Checks whether a character is in the set of characters that specifies
 * a flag according to the printf specification:
 * http://www.cplusplus.com/reference/cstdio/printf/
 *
 * \param c
 *      character to check
 * \return
 *      true if the character is in the set
 */
constexpr inline bool
isFlag(char c)
{
    return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0';
}

/**
 * Checks whether a character is a digit (0-9) or not.
 *
 * \param c
 *      character to check
 * \return
 *      true if the character is a digit
 */
constexpr inline bool
isDigit(char c) {
    return (c >= '0' && c <= '9');
}

/**
 * Checks whether a character is in the set of characters that specifies
 * a length field according to the printf specification:
 * http://www.cplusplus.com/reference/cstdio/printf/
 *
 * \param c
 *      character to check
 * \return
 *      true if the character is in the set
 */
constexpr inline bool
isLength(char c)
{
    return c == 'h' || c == 'l' || c == 'j'
            || c == 'z' ||  c == 't' || c == 'L';
}

/**
 * Checks whether a character is with the terminal set of format specifier
 * characters according to the printf specification:
 * http://www.cplusplus.com/reference/cstdio/printf/
 *
 * \param c
 *      character to check
 * \return
 *      true if the character is in the set, indicating the end of the specifier
 */
constexpr inline bool
isTerminal(char c)
{
    return c == 'd' || c == 'i'
                || c == 'u' || c == 'o'
                || c == 'x' || c == 'X'
                || c == 'f' || c == 'F'
                || c == 'e' || c == 'E'
                || c == 'g' || c == 'G'
                || c == 'a' || c == 'A'
                || c == 'c' || c == 'p'
                || c == '%' || c == 's'
                || c == 'n';
}

/**
 * Analyzes a static printf style format string and extracts type information
 * about the p-th parameter that would be used in a corresponding NANO_LOG()
 * invocation.
 *
 * \tparam N
 *      Length of the static format string (automatically deduced)
 * \param fmt
 *      Format string to parse
 * \param param_num
 *      p-th parameter to return type information for (starts from zero)
 * \return
 *      Returns an ParamType enum describing the type of the parameter
 */
template<int N>
constexpr inline ParamType
getParamInfo(const char (&fmt)[N],
             int param_num=0)
{
    int pos = 0;
    while (pos < N - 1) {

        // The code below searches for something that looks like a printf
        // specifier (i.e. something that follows the format of
        // %<flags><width>.<precision><length><terminal>). We only care
        // about precision and type, so everything else is ignored.
        if (fmt[pos] != '%') {
            ++pos;
            continue;
        } else {
            // Note: gcc++ 5,6,7,8 seems to hang whenever one uses the construct
            // "if (...) {... continue; }" without an else in constexpr
            // functions. Hence, we have the code here wrapped in an else {...}
            // I reported this bug to the developers here
            // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=86767
            ++pos;

            // Two %'s in a row => Comment
            if (fmt[pos] == '%') {
                ++pos;
                continue;
            } else {

                // Consume flags
                while (isFlag(fmt[pos]))
                    ++pos;

                // Consume width
                if (fmt[pos] == '*') {
                    if (param_num == 0)
                        return ParamType::kDYNAMIC_WIDTH;

                    --param_num;
                    ++pos;
                } else {
                    while (isDigit(fmt[pos]))
                        ++pos;
                }

                // Consume precision
                bool hasDynamicPrecision = false;
                int precision = -1;
                if (fmt[pos] == '.') {
                    ++pos;  // consume '.'

                    if (fmt[pos] == '*') {
                        if (param_num == 0)
                            return ParamType::kDYNAMIC_PRECISION;

                        hasDynamicPrecision = true;
                        --param_num;
                        ++pos;
                    } else {
                        precision = 0;
                        while (isDigit(fmt[pos])) {
                            precision = 10*precision + (fmt[pos] - '0');
                            ++pos;
                        }
                    }
                }

                // consume length
                while (isLength(fmt[pos]))
                    ++pos;

                // Consume terminal
                if (!isTerminal(fmt[pos])) {
                    throw std::invalid_argument(
                            "Unrecognized format specifier after %");
                }

                // Fail on %n specifiers (i.e. store position to address) since
                // we cannot know the position without formatting.
                if (fmt[pos] == 'n') {
                    throw std::invalid_argument(
                            "%n specifiers are not support in NanoLog!");
                }

                if (param_num != 0) {
                    --param_num;
                    ++pos;
                    continue;
                } else {
                    if (fmt[pos] != 's')
                        return ParamType::kNON_STRING;

                    if (hasDynamicPrecision)
                        return ParamType::kSTRING_WITH_DYNAMIC_PRECISION;

                    if (precision == -1)
                        return ParamType::kSTRING_WITH_NO_PRECISION;
                    else
                        return ParamType(precision);
                }
            }
        }
    }

    return ParamType::kINVALID;
}

/**
 * Counts the number of parameters that need to be passed in for a particular
 * printf style format string.
 *
 * One subtle point is that we are counting parameters, not specifiers, so a
 * specifier of "%*.*d" will actually count as 3 since the two '*" will result
 * in a parameter being passed in each.
 *
 * \tparam N
 *      length of the printf style format string (automatically deduced)
 *
 * \param fmt
 *      printf style format string to analyze
 *
 * @return
 */
template<int N>
constexpr inline int
countFmtParams(const char (&fmt)[N])
{
    int count = 0;
    while (getParamInfo(fmt, count) != ParamType::kINVALID)
        ++count;
    return count;
}

template<unsigned int N>
constexpr int
getNumNibblesNeeded(const char (&fmt)[N])
{
    int num_nibbles = 0;
    for (int i = 0; i < countFmtParams(fmt); ++i) {
        ParamType t = getParamInfo(fmt, i);
        if (t == kNON_STRING || t == kDYNAMIC_PRECISION || t == kDYNAMIC_WIDTH)
            ++num_nibbles;
    }

    return num_nibbles;
}

/**
 * Helper to analyzeFormatString. This level of indirection is needed to
 * unpack the index_sequence generated in analyzeFormatString and
 * use the sequence as indices for calling getParamInfo.
 *
 * \tparam N
 *      Length of the format string (automatically deduced)
 * \tparam Indices
 *      An index sequence from [0, N) where N is the number of parameters in
 *      the format string (automatically deduced)
 *
 * \param fmt
 *      printf format string to analyze
 *
 * \return
 *      An std::array describing the types at each index (zero based).
 */
template<int N, std::size_t... Indices>
constexpr std::array<ParamType, sizeof...(Indices)>
analyzeFormatStringHelper(const char (&fmt)[N], std::index_sequence<Indices...>)
{
    return {{ getParamInfo(fmt, Indices)... }};
}

/**
 * Computes a ParamType array describing the parameters that would be used
 * with the provided printf style format string. The indices of the array
 * correspond with the parameter position in the variable args portion of
 * the invocation.
 *
 * \template NParams
 *      The number of additional format parameters that follow the format
 *      string in a printf-like function. For example printf("%*.*d", 9, 8, 7)
 *      would have NParams = 3
 * \template N
 *      length of the printf style format string (automatically deduced)
 *
 * \param fmt
 *      Format string to generate the array for
 *
 * \return
 *      An std::array where the n-th index indicates that the
 *      n-th format parameter is a "%s" or not.
 */
template<int NParams, size_t N>
constexpr std::array<ParamType, NParams>
analyzeFormatString(const char (&fmt)[N])
{
    return analyzeFormatStringHelper(fmt, std::make_index_sequence<NParams>{});
}

template<typename... T>
constexpr std::array<size_t, sizeof...(T)>
analyzeFmtParamSizeHelper(T... arg)
{
    return {{sizeof(T)...}};
}

template<typename... T>
constexpr std::array<size_t, sizeof...(T)>
analyzeFmtParamSize(T... args)
{
    if constexpr(sizeof...(T) == 0) {
        return {};
    }
    else {
        return analyzeFmtParamSizeHelper(args...);
    }
}

/**
 * Special templated function that takes in an argument T and attempts to
 * convert it to a uint64_t. If the type T is incompatible, than a value
 * of 0 is returned.
 *
 * This function is primarily to hack around
 *
 * \tparam T
 *      Type of the input parameter (automatically deduced)
 *
 * \param t
 *      Parameter to try to convert to a uint64_t
 *
 * \return
 *      t as a uint64_t if it's convertible, otherwise a 0.
 */
template<typename T>
inline
typename std::enable_if<std::is_convertible<T, uint64_t>::value
                        && !std::is_floating_point<T>::value
                        , uint64_t>::type
as_uint64_t(T t) {
    return t;
}

template<typename T>
inline
typename std::enable_if<!std::is_convertible<T, uint64_t>::value
                        || std::is_floating_point<T>::value
                        , uint64_t>::type
as_uint64_t(T t) {
    return 0;
}

/**
 * For a single non-string, non-void pointer argument, return the number
 * of bytes needed to represent the full-width type without compression.
 *
 * \tparam T
 *      Actual type of the argument (automatically deduced)
 *
 * \param fmt_type
 *      Type of the argument according to the original printf-like format
 *      string (needed to disambiguate 'const char*' types from being
 *      '%p' or '%s' and for precision info)
 * \param[in/out] previousPrecision
 *      Store the last 'precision' format specifier type encountered
 *      (as dictated by the fmtType)
 * \param previous_precision
 *      Byte length of the current argument, if it is a string, else, undefined
 * \param arg
 *      Argument to compute the size for
 *
 * \return
 *      Size of the full-width argument without compression
 */
template<typename T>
inline
typename std::enable_if<!std::is_same<T, const wchar_t*>::value
                        && !std::is_same<T, const char*>::value
                        && !std::is_same<T, char*>::value
                        , size_t>::type
getArgSize(const ParamType fmt_type,
           uint64_t &previous_precision,
           size_t &string_size,
           T arg)
{
    if (fmt_type == ParamType::kDYNAMIC_PRECISION)
        previous_precision = as_uint64_t(arg);

    string_size = sizeof(T);
    return sizeof(T);
}


/**
 * String specialization for getArgSize. Returns the number of bytes needed
 * to represent a string (with consideration for any 'precision' specifiers
 * in the original format string and) without a NULL terminator and with a
 * uint32_t length.
 *
 * \param fmt_type
 *      Type of the argument according to the original printf-like format
 *      string (needed to disambiguate 'const char*' types from being
 *      '%p' or '%s' and for precision info)
 * \param previous_precision
 *      Store the last 'precision' format specifier type encountered
 *      (as dictated by the fmtType)
 * \param string_size
 *      Byte length of the current argument, if it is a string, else, undefined
 * \param str
 *      String to compute the length for
 * \return
 *      Length of the string str with a uint32_t length and no NULL terminator
 */
inline size_t
getArgSize(const ParamType fmt_type,
           uint64_t &previous_precision,
           size_t &string_size,
           const char* str)
{
    if (fmt_type <= ParamType::kNON_STRING)
        return sizeof(void*);
    
    string_size = strlen(str);
    uint32_t fmt_length = static_cast<uint32_t>(string_size);

    // Strings with static length specifiers (ex %.10s), have non-negative
    // ParamTypes equal to the static length. Thus, we use that value to
    // truncate the string as necessary.
    if (fmt_type >= ParamType::kSTRING && string_size > fmt_length)
        string_size = fmt_length;

    // If the string had a dynamic precision specified (i.e. %.*s), use
    // the previous parameter as the precision and truncate as necessary.
    else if (fmt_type == ParamType::kSTRING_WITH_DYNAMIC_PRECISION &&
                string_size > previous_precision)
        string_size = previous_precision;

    return string_size + sizeof(uint32_t);
}

/**
 * Specialization for getArgSizes when there are no arguments, i.e. it is
 * the end of the recursion. (See above for documentation)
 */
template<int arg_num = 0, unsigned long N, int M>
inline size_t
getArgSizes(const std::array<ParamType, N>&, uint64_t &, size_t (&)[M])
{
    return 0;
}

/**
 * Given a variable number of printf arguments and type information deduced
 * from the original format string, compute the amount of space needed to
 * store all the arguments.
 *
 * For the most part, all non-string arguments will be calculated as full
 * width and the all string arguments will have a 32-bit length descriptor
 * and no NULL terminator.
 *
 * \tparam arg_num
 *      Internal counter for which n-th argument we're processing, aka
 *      the recursion depth.
 * \tparam N
 *      Length of argFmtTypes array (automatically deduced)
 * \tparam M
 *      Length of the stringSizes array (automatically deduced)
 * \tparam T1
 *      Type of the head of the arguments (automatically deduced)
 * \tparam Ts
 *      Types of the tail of the argument pack (automatically deduced)
 *
 * \param arg_fmt_types
 *      Types of the arguments according to the original printf-like format
 *      string.
 * \param previous_precision
 *      Internal parameter that stores the last dynamic 'precision' format
 *      argument encountered (as dictated by argFmtTypes).
 * \param[out] string_sizes
 *      Stores the lengths of string arguments without a NULL terminator
 *      and with a 32-bit length descriptor
 * \param head
 *      First of the argument pack
 * \param rest
 *      Rest of the argument pack
 * \return
 *      Total number of bytes needed to represent all arguments with no
 *      compression in the NanoLog system.
 */
template<int arg_num = 0, unsigned long N, int M, typename T1, typename... Ts>
inline size_t
getArgSizes(const std::array<ParamType, N>& arg_fmt_types,
            uint64_t &previous_precision,
            size_t (&string_sizes)[M],
            T1 head, Ts... rest)
{
    return getArgSize(arg_fmt_types[arg_num], previous_precision,
                                                    string_sizes[arg_num], head)
           + getArgSizes<arg_num + 1>(arg_fmt_types, previous_precision,
                                                    string_sizes, rest...);
}


/**
 * Stores a single printf argument into a buffer and bumps the buffer pointer.
 *
 * Non-string types are stored (full-width) and string types are stored
 * with a uint32_t header describing the string length in bytes followed
 * by the string itself with no NULL terminator.
 *
 * Note: This is the non-string specialization of the function
 * (hence the std::enable_if below), so it contains extra
 * parameters that are unused.
 *
 * \tparam T
 *      Type to store (automatically deduced)
 *
 * \param[in/out] storage
 *      Buffer to store the argument into
 * \param arg
 *      Argument to store
 * \param param_type
 *      Type information deduced from the format string about this
 *      argument (unused here)
 * \param string_size
 *      Stores the byte length of the argument, if it is a string (unused here)
 */
template<typename T>
inline
typename std::enable_if<!std::is_same<T, const wchar_t*>::value
                        && !std::is_same<T, const char*>::value
                        && !std::is_same<T, wchar_t*>::value
                        && !std::is_same<T, char*>::value
                        , void>::type
storeArgument(char **storage,
               T arg,
               ParamType param_type,
               size_t string_size)
{
    memcpy(*storage, &arg, sizeof(T));
    *storage += sizeof(T);

    #ifdef ENABLE_DEBUG_PRINTING
        printf("\tRBasic  [%p]= ", dest);
        std::cout << *dest << "\r\n";
    #endif
}

// string specialization of the above
template<typename T>
inline
typename std::enable_if<std::is_same<T, const wchar_t*>::value
                        || std::is_same<T, const char*>::value
                        || std::is_same<T, wchar_t*>::value
                        || std::is_same<T, char*>::value
                        , void>::type
storeArgument(char **storage,
               T arg,
               const ParamType param_type,
               const size_t string_size)
{
    // If the printf style format string's specifier says the arg is not
    // a string, we save it as a pointer instead
    if (param_type <= ParamType::kNON_STRING) {
        storeArgument<const void*>(storage, static_cast<const void*>(arg),
                                    param_type, string_size);
        return;
    }

    // Since we've already paid the cost to find the string length earlier,
    // might as well save it in the stream so that the compression function
    // can later avoid another strlen/wsclen invocation.
    if(string_size > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("Strings larger than std::numeric_limits<uint32_t>::max() are unsupported");
    }
    auto size = static_cast<uint32_t>(string_size);
    memcpy(*storage, &size, sizeof(uint32_t));
    *storage += sizeof(uint32_t);

#ifdef ENABLE_DEBUG_PRINTING
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"
#pragma GCC diagnostic ignored "-Wformat"
        if (sizeof(typename std::remove_pointer<T>::type) == 1) {
            printf("\tRString[%p-%u]= %s\r\n", *buffer, size, arg);
        } else {
            printf("\tRWString[%p-%u]= %ls\r\n", *buffer, size, arg);
        }
#pragma GCC diagnostic pop
#endif

    memcpy(*storage, arg, string_size);
    *storage += string_size;
    return;
}

/**
 * Specialization of store_arguments that processes no arguments, i.e. this
 * is the end of the head/rest recursion. See above for full documentation.
 */
template<int arg_num = 0, unsigned long N, int M>
inline void
storeArguments(const std::array<ParamType, N>&,
                size_t (&string_sizes)[M],
                char **)
{
    // No arguments, do nothing.
}

/**
 * Given a variable number of arguments to a NANO_LOG (i.e. printf-like)
 * statement, recursively unpack the arguments, store them to a buffer, and
 * bump the buffer pointer.
 *
 * \tparam argNum
 *      Internal counter indicating which parameter we're storing
 *      (aka the recursion depth).
 * \tparam N
 *      Size of the isArgString array (automatically deduced)
 * \tparam M
 *      Size of the stringSizes array (automatically deduced)
 * \tparam T1
 *      Type of the Head of the remaining variable number of arguments (deduced)
 * \tparam Ts
 *      Type of the Rest of the remaining variable number of arguments (deduced)
 *
 * \param param_types
 *      Type information deduced from the printf format string about the
 *      n-th argument to be processed.
 * \param[in/out] string_sizes
 *      Stores the byte length of the n-th argument, if it is a string
 *      (if not, it is undefined).
 * \param[in/out] storage
 *      Buffer to store the arguments to
 * \param head
 *      Head of the remaining number of variable arguments
 * \param rest
 *      Rest of the remaining variable number of arguments
 */
template<int arg_num = 0, unsigned long N, int M, typename T1, typename... Ts>
inline void
storeArguments(const std::array<ParamType, N>& param_types,
                size_t (&string_sizes)[M],
                char **storage,
                T1 head,
                Ts... rest)
{
    // Peel off one argument to store, and then recursively process rest
    storeArgument(storage, head, param_types[arg_num], string_sizes[arg_num]);
    storeArguments<arg_num + 1>(param_types, string_sizes, storage, rest...);
}

#include <cassert>
/**
 * Cast one size of int down to another one.
 * Asserts that no precision is lost at runtime.
 */
template<typename Small, typename Large>
inline Small
downCast(const Large& large)
{
    Small small = static_cast<Small>(large);
    // The following comparison (rather than "large==small") allows
    // this method to convert between signed and unsigned values.
    assert(large-small == 0);
    return small;
}

} // details

} // namespace static_log

#endif // STATIC_LOG_INTERNAL_H