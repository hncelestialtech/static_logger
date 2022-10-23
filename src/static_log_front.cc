#include "static_log_front.h"

namespace static_log {

namespace details {

volatile uint64_t log_id __attribute__ ((aligned (128))) = 0;

} // details

} // static_log