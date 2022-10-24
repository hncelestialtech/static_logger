#ifndef STATIC_LOG_FRONT_H
#define STATIC_LOG_FRONT_H

#include <assert.h>
#include <stdint.h>

#include "static_log_internal.h"
// #include "static_log.h"
// #include "static_log_utils.h"
#include "static_log_backend.h"

namespace static_log {

namespace details {
/**
 * The ID used to record each log, since each thread has only
 * one queue, the log order between each thread is not guaranteed 
 * to increase, so there needs to be a global ID to number the log
 */
extern volatile uint64_t log_id;

} // details

} // static_log

#endif // STATIC_LOG_FRONT_H