#include "static_log.h"
#include "static_log_front.h"
#include "static_log_backend.h"

namespace static_log
{
LogLevels::LogLevel getLogLevel() {
    return StaticLogBackend::getLogLevel();
}
} // namespace static_log
