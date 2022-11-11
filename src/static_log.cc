#include "static_log.h"

#include "static_log_front.h"
#include "static_log_backend.h"

namespace static_log
{

void preallocate()
{
    details::StaticLogBackend::preallocate();
}

void setLogFile(const char* filename)
{
    details::StaticLogBackend::setLogFile(filename);
}

LogLevels::LogLevel getLogLevel() 
{
    return details::StaticLogBackend::getLogLevel();
}

void setLogLevel(LogLevels::LogLevel log_level) 
{
    details::StaticLogBackend::setLogLevel(log_level);
}

void sync()
{
    details::StaticLogBackend::sync();
}

} // namespace static_log
