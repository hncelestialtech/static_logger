#include "static_log.h"

int main()
{
    static_log::preallocate();
    // constexpr int n_params = static_log::internal::utils::countFmtParams("%s"); 
    
    // /*** Very Important*** These must be 'static' so that we can save pointers 
    //  **/ 
    // static constexpr std::array<static_log::internal::ParamType, n_params> param_types = 
    //                             static_log::internal::utils::analyzeFormatString<n_params>("%s"); 
    // static constexpr static_log::internal::StaticInfo static_info =  
    //                         static_log::internal::StaticInfo(n_params, param_types.data(), "%s"); 
    // /* Triggers the GNU printf checker by passing it into a no-op function.
    //  * Trick: This call is surrounded by an if false so that the VA_ARGS don't
    //  * evaluate for cases like '++i'.*/ 
    // if (false) { static_log::internal::utils::checkFormat("%s", "hello world"); } /*NOLINT(cppcoreguidelines-pro-type-vararg, hicpp-vararg)*/
    
    // static size_t param_size[n_params]{};   
    // uint64_t previousPrecision = -1;   
    // size_t alloc_size = static_log::internal::utils::getArgSizes(param_types, previousPrecision,    
    //                         param_size, "hello world") + sizeof(static_log::internal::LogEntry);    
    // char *write_pos = static_log::details::StaticLogBackend::reserveAlloc(alloc_size);   
    
    // static_log::internal::LogEntry *log_entry = new(write_pos) static_log::internal::LogEntry(&static_info, param_size);    
    // write_pos += sizeof(static_log::internal::LogEntry);    
    // static_log::internal::utils::storeArguments(param_types, param_size, &write_pos, "hello world");    
    // log_entry->entry_size = static_log::internal::utils::downCast<uint32_t>(alloc_size);    
    // log_entry->timestamp = static_log::details::log_id++;  
    
    // static_log::details::StaticLogBackend::finishAlloc(alloc_size);  
    STATIC_LOG(static_log::LogLevels::kNOTICE, "%s", "hello world");
    static_log::setLogLevel(static_log::LogLevels::kNOTICE);
    STATIC_LOG(static_log::LogLevels::kDEBUG, "%s", "hello world");
    static_log::setLogFile("log_.txt");
    // STATIC_LOG(static_log::LogLevels::kNOTICE, "%s", "hello world");
    return 0;
}