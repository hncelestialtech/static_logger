#ifndef STATIC_LOG_H
#define STATIC_LOG_H

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

} // namespace static_log

#endif // STATIC_LOG_H