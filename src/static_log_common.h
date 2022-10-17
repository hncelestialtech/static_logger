#ifndef STATIC_LOG_COMMON_H
#define STATIC_LOG_COMMON_H

namespace static_log
{

#if defined(__GNUC__)
#define STATICLOG_PRINTF_FORMAT
#define STATICLOG_PRINTF_FORMAT_ATTR(string_index, first_to_check) \
  __attribute__((__format__(__printf__, string_index, first_to_check)))
#else
#define STATICLOG_PRINTF_FORMAT
#define STATICLOG_PRINTF_FORMAT_ATTR(string_index, first_to_check)
#endif

#define BYTES_PER_CACHE_LINE 64

static const uint32_t STAGING_BUFFER_SIZE = 1048576U;

} // namespace static_log

#endif // STATIC_LOG_COMMON_H