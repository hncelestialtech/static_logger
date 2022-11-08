#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <string>

#include<gtest/gtest.h>

#include "static_log_internal.h"
using namespace static_log;
using namespace static_log::details;
#define DEFALT_CACHE_SIZE 1024 * 1024

static void
convertInt2Str(int ts, char*& raw) {
    if (ts < 10) {
        *raw = '0';
        raw++;
        *raw = std::to_string(ts).c_str()[0];
        raw++;
    } else {
        memcpy(raw, std::to_string(ts).c_str(), 2);
        raw+=2;
    }
}

#define TIEMSTAMP_PREFIX_LEN 31
// [xxxx-xx-xx-hh:mm:ss.xxxxxxxxx]
int
generateTimePrefix(uint64_t timestamp, char* raw_data) {
    char* origin = raw_data;
    int prefix_len{0};
    const int nano_bits = 9;
    static_assert(TIEMSTAMP_PREFIX_LEN < DEFALT_CACHE_SIZE, "default buffer size is smaller than time prefix len");
    *raw_data = '[';    prefix_len += 1;
    raw_data++;
    uint64_t nano = timestamp % 1000000000;
    timestamp = timestamp / 1000000000;
    struct tm* tm_now = localtime((time_t*)&timestamp);
    memcpy(raw_data, std::to_string(tm_now->tm_year + 1900).c_str(), 4);
    raw_data += 4; prefix_len += 4;
    *raw_data++ = '-'; prefix_len += 1;
    convertInt2Str(tm_now->tm_mon + 1, raw_data);   prefix_len += 2;
    *raw_data++ = '-'; prefix_len += 1;
    convertInt2Str(tm_now->tm_mday, raw_data);  prefix_len += 2;
    *raw_data++ = '-';  prefix_len += 1;
    convertInt2Str(tm_now->tm_hour, raw_data);  prefix_len += 2;
    *raw_data++ = ':';  prefix_len += 1;
    convertInt2Str(tm_now->tm_min, raw_data);   prefix_len += 2;
    *raw_data++ = ':';  prefix_len += 1;
    convertInt2Str(tm_now->tm_sec, raw_data);   prefix_len += 2;
    *raw_data++ = '.';  prefix_len += 1;
    int nanolen = strlen(std::to_string(nano).c_str());
    memcpy(raw_data + nano_bits - nanolen , std::to_string(nano).c_str(), nanolen);
    if (nanolen < nano_bits) {
        int bits = nano_bits - nanolen;
        for(int i = 0; i < bits; ++i)
            *raw_data++ = '0';
        raw_data += nanolen;
    } else {
        raw_data += nano_bits;
    }
    prefix_len += nano_bits;
    *raw_data = ']';    prefix_len += 1;
    assert(prefix_len == TIEMSTAMP_PREFIX_LEN);
    return prefix_len;
}

TEST(test_utils, test_generateTimePrefix)
{
    char test_buf[1024*1024];
    memset(test_buf, 0, 1024* 1024);
    struct timespec ts;
    auto ret = clock_gettime(CLOCK_REALTIME, &ts);
    auto tsns = ts.tv_nsec + ts.tv_sec * 1000000000;
    printf("ts {%ld}\n", tsns);
    int len = generateTimePrefix(tsns, test_buf);
    ASSERT_EQ(len, 31);
    test_buf[len] = '\0';
    printf("test_generateTimePrefix: %s\n", test_buf);
    ASSERT_EQ(strlen(test_buf), 31);
}

TEST(test_utils, test_generateTimePrefixFormat)
{
    char test_buf[DEFALT_CACHE_SIZE];
    memset(test_buf, 0, DEFALT_CACHE_SIZE);
    int year = 1999;
    int month = 06;
    int day = 28;
    int hour = 8;
    int min = 0;
    int sec = 0;
    int nano = 8542;
    struct tm stm;
    stm.tm_year = year - 1900;
    stm.tm_mon = month - 1;
    stm.tm_mday = day;
    stm.tm_hour = hour + 1;
    stm.tm_min = min;
    stm.tm_sec = sec;
    auto ts = mktime(&stm);
    ts = ts * 1000000000 + nano;
    fprintf(stdout, "%ld\n", ts);
    auto len = generateTimePrefix(ts, test_buf);
    fprintf(stdout, "test_generateTimePrefixFormat: %d\n", len);
    test_buf[len] = '\0';
    fprintf(stdout, "test_generateTimePrefixFormat: %s\n", test_buf);
}

static int
resize_log_buffer(char*&  log_buffer, size_t& old_size, size_t new_size)
{
    char* buffer = (char*)malloc(new_size);
    if (buffer == NULL) {
        fprintf(stderr, "Failed to resize log buffer, wanted to alloc %lu\n", new_size);
        return -1;
    }
    memcpy(buffer, log_buffer, old_size);
    free(log_buffer);
    log_buffer = buffer;
    old_size = new_size;
    return 0;
}

#define CHECK_LOG_BUFFER_REALLOC() do { \
    if (fmt_len >= reserved) {   \
        int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   \
        reserved = log_buffer_len - start_pos;  \
        goto retry; \
    }   \
} while(0)

#pragma GCC diagnostic ignored "-Wformat"
static int
decodeNonStringFmt(
    char*& log_buffer, 
    size_t& log_buffer_len,
    size_t& reserved,
    size_t start_pos,
    char * fmt, 
    const char* param, 
    size_t param_size)
{
    int fmt_len = 0;
    char terminal_flag = fmt[strlen(fmt) - 1];
retry:
    switch (param_size) {
    case sizeof(char):
        {
            const char* param8 = param;
            if (terminal_flag == 'c') {
                fmt_len = snprintf(log_buffer + start_pos, reserved, fmt, *(char*)param8);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }   
            }
            else if (terminal_flag == 'd' || terminal_flag == 'i') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(int8_t*)param8);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(uint8_t*)param8);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else 
                fprintf(stderr, "Failed to parse fmt with a one bytes long param\n");
            break;
        }
    case sizeof(uint16_t):
        {
            const uint16_t* param16 = (uint16_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(int16_t*)param16);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(uint16_t*)param16);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else 
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    case sizeof(uint32_t):
        {
            const uint32_t* param32 = (uint32_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(int32_t*)param32);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(uint32_t*)param32);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else if (terminal_flag == 'f' || terminal_flag == 'F' || terminal_flag == 'e' ||terminal_flag == 'e' || 
                terminal_flag == 'E' || terminal_flag == 'g' || terminal_flag == 'G'|| terminal_flag == 'a' ||terminal_flag == 'A') {
                static_assert(sizeof(float) == sizeof(uint32_t));
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(float*)param32);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }            
            else 
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    case sizeof(uint64_t):
        {
            const uint64_t* param64 = (uint64_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(int64_t*)param64);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X') {
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(uint64_t*)param64);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1 + log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else if (terminal_flag == 'f' || terminal_flag == 'F' || terminal_flag == 'e' ||terminal_flag == 'e' || 
                terminal_flag == 'E' || terminal_flag == 'g' || terminal_flag == 'G'|| terminal_flag == 'a' ||terminal_flag == 'A') {
                static_assert(sizeof(double) == sizeof(uint64_t));
                fmt_len = snprintf(log_buffer + start_pos, log_buffer_len, fmt, *(double*)param64);
                if (fmt_len >= reserved) {   
                    int ret = resize_log_buffer(log_buffer, log_buffer_len, fmt_len << 1+ log_buffer_len - reserved);   
                    reserved = log_buffer_len - start_pos;  
                    goto retry; 
                }  
            }
            else
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    default:
        fprintf(stderr, "Failed to decode fmt param, got size %lu\n", param_size);
        break;
    }
    return fmt_len;
}
#pragma GCC diagnostic pop

#define DEFAULT_PARAM_CACHE_SIZE 1024
static int
decodeStringFmt(char* log_buffer, size_t& bufferlen, size_t& reserved, size_t start_pos, const char* param, size_t param_size, const char* fmt)
{
    assert((char*)log_buffer - (char*)start_pos == bufferlen - reserved);
    char string_param_cache[DEFAULT_PARAM_CACHE_SIZE];
    bool dynamic_alloc = false;
    char* param_buffer = string_param_cache;
    int ret = 0;
    if (param_size >= DEFAULT_PARAM_CACHE_SIZE) {
        param_buffer = (char*)malloc(param_size + 1); // strlen(str) + '\0'
        if (param_buffer == NULL) {
            fprintf(stderr, "Failed to alloc param buffer\n");
            return -1;
        }
        dynamic_alloc = true;
    }
    memcpy(param_buffer, param, param_size);
    param_buffer[param_size] = '\0';
    size_t needed_fmt_len = snprintf(log_buffer + start_pos, reserved, fmt, param_buffer);
    if (needed_fmt_len > reserved) {
        size_t new_log_buflen = needed_fmt_len + 1 + bufferlen - reserved;
        char* tmp = (char*)malloc(new_log_buflen);
        if (tmp == NULL) {
            fprintf(stderr, "Failed to realloc log buffer, needed alloc size %lu\n", needed_fmt_len);
            ret = -1;
            goto out;
        }
        reserved = new_log_buflen - bufferlen + reserved;
        bufferlen = new_log_buflen;
        memcpy(tmp, log_buffer, start_pos);
        snprintf(tmp + start_pos, needed_fmt_len + 1, fmt, param_buffer);
        free(log_buffer);
        log_buffer = tmp;
    }
    ret = needed_fmt_len;
out:
    if (dynamic_alloc)
        free(param_buffer);
    return ret;
}

static int
process_fmt(
        const char* fmt, 
        const int num_params, 
        const ParamType* param_types,
        size_t* param_size_list,
        const char* param_list, 
        char*& log_buffer, size_t& buflen, size_t start_pos)
{
    char* log_pos = log_buffer + start_pos;
    size_t fmt_list_len = strlen(fmt);
    size_t reserved = buflen - start_pos;
    int pos = 0;
    int param_idx = 0;
    bool success = true;
    while (pos < fmt_list_len) {
        if (fmt[pos] != '%') {
            *log_pos++ = fmt[pos++];
            reserved--;
            continue;
        } else {
            ++pos;
            int fmt_single_len = 1;
            if (fmt[pos] == '%') {
                *log_pos++ = '%';
                reserved--;
                ++pos;
                continue;
            } else {
                int fmt_start_pos = pos - 1;
                while (!isTerminal(fmt[pos])) {
                    fmt_single_len++;
                    pos++;
                }
                fmt_single_len++;
                pos++;
                char* fmt_single;
                char static_fmt_cache[100];
                bool dynamic_fmt = false;
                if (fmt_single_len < 100) {
                    memset(static_fmt_cache, 0, 100);
                    fmt_single = static_fmt_cache;
                } else {
                    fmt_single = (char*)malloc(fmt_single_len);
                    dynamic_fmt = true;
                }
                memcpy(fmt_single, fmt + fmt_start_pos, fmt_single_len);
                size_t log_fmt_len = 0;

                if (param_idx < num_params) {
                    if (param_types[param_idx] > ParamType::kNON_STRING) {
                        uint32_t string_size = *(uint32_t*)param_list;
                        param_list += sizeof(uint32_t);
                        log_fmt_len = decodeStringFmt(log_buffer, buflen, reserved, log_pos - log_buffer, param_list, string_size, fmt_single);
                        param_list = param_list + sizeof(uint32_t) + string_size;
                    }
                    else {
                        log_fmt_len = decodeNonStringFmt(log_buffer, buflen, reserved, log_pos - log_buffer, fmt_single, param_list, param_size_list[param_idx]);
                        if (log_fmt_len == -1)  {
                            success = false;
                            break;
                        }
                        param_list += param_size_list[param_idx];
                    }
                    log_pos += log_fmt_len;
                    reserved -= log_fmt_len;
                    param_idx++;
                } else {
                    fprintf(stderr, "Failed to fmt log\n");
                    success = false;
                    if (dynamic_fmt) 
                        free(fmt_single);
                    break;
                }
                if (dynamic_fmt) 
                    free(fmt_single);
            }
        }
    }
    return success? buflen - reserved: -1;
}

TEST(test_utils, test_fmt_simple)
{
    constexpr int n_params = countFmtParams("HELLO WORLD %c %d %lf %f %s\n");
    ASSERT_EQ(n_params, 5);
    
    static constexpr std::array<ParamType, n_params> param_types = analyzeFormatString<n_params>("HELLO WORLD %c %d %lf %f %s\n");
    size_t len = strlen("HELLO WORLD %c %d %lf %f %s\n");
    char c_parma = 'a';
    char* s_param = "hello world";
    int int_param = 1;
    double fl_param = 3.14;
    float f_param = 3.22;
    size_t string_sizes[5 + 1] = {};
    uint64_t previousPrecision = -1;
    size_t alloc_size = getArgSizes(param_types, previousPrecision,
                            string_sizes, c_parma, int_param, fl_param, f_param, s_param);
    uint64_t store_bufsize = sizeof(char) + sizeof(int) + sizeof(double) + sizeof(float) + sizeof(uint32_t) + 11;
    ASSERT_EQ(alloc_size, store_bufsize);
    
    ASSERT_EQ(string_sizes[0], 1);
    ASSERT_EQ(string_sizes[1], 4);
    ASSERT_EQ(string_sizes[2], 8);
    ASSERT_EQ(string_sizes[3], 4);
    ASSERT_EQ(string_sizes[4], 11);
    
    char* log_buffer = (char*)malloc(1024 * 1024);
    size_t buflen = 1024*1024;
    char * param_list = (char*)malloc(alloc_size);
    char * param_list_orig = param_list;
    storeArguments(param_types, string_sizes, &param_list, c_parma, int_param, fl_param, f_param, s_param);
    
    char* store_validate = (char*)malloc(alloc_size);
    store_validate[0] = c_parma;
    ASSERT_EQ(store_validate[0], param_list_orig[0]);
    store_validate++;
    *(int*)store_validate = int_param;
    ASSERT_EQ(*(int*)store_validate, *(int*)(param_list_orig + 1));
    store_validate += sizeof(int);
    *(double*)store_validate = fl_param;
    ASSERT_EQ(*(double*)store_validate, *(double*)(param_list_orig + 1 + sizeof(int)));

    int plen = process_fmt("HELLO WORLD %c %d %lf %f %s\n",
        n_params, &param_types[0], &string_sizes[0], param_list_orig, log_buffer, buflen, 0
    );
    log_buffer[plen] = '\0';
    fprintf(stdout, "%s\n", log_buffer);
    // free(write_buf);
}


TEST(test_fmt, test_int)
{
    char* log_buffer = (char*)malloc(1024 * 1024);
    size_t buflen = 1024*1024;
    constexpr int n_params = countFmtParams("HELLO WORLD %d %+010d %ld %+010ld %lu %+010lu %o %x %X\n");
    int iparam1 = -1;
    uint64_t iparam2 = (uint64_t)-1;
    static constexpr std::array<ParamType, n_params> param_types = analyzeFormatString<n_params>("HELLO WORLD %d %+010d %ld %+010ld %lu %+010lu %o %x %X\n");
    size_t string_sizes[9] = {};
    uint64_t previousPrecision = -1;
    size_t alloc_size = getArgSizes(param_types, previousPrecision,
                            string_sizes, iparam1, iparam1, iparam2, iparam2, iparam2, iparam2, iparam2, iparam2, iparam2);
    char * param_list = (char*)malloc(alloc_size);
    char * param_list_orig = param_list;
    storeArguments(param_types, string_sizes, &param_list, iparam1, iparam1, iparam2, iparam2, iparam2, iparam2, iparam2, iparam2, iparam2);
    int plen = process_fmt("HELLO WORLD %d %+010d %ld %+010ld %lu %+010lu %o %x %X\n",
        n_params, &param_types[0], &string_sizes[0], param_list_orig, log_buffer, buflen, 0
    );
    log_buffer[plen] = '\0';
    fprintf(stdout, "%s", log_buffer);
    fprintf(stdout, "HELLO WORLD %d %+010d %ld %+010ld %lu %+010lu %o %x %X\n", iparam1, iparam1, iparam2, iparam2, iparam2, iparam2, iparam2, iparam2, iparam2);
}

TEST(test_fmt, test_float)
{
    char* log_buffer = (char*)malloc(1024 * 1024);
    size_t buflen = 1024*1024;
    float f1 = 392.65;
    double f2 = 392.65;
    constexpr int n_params = countFmtParams("HELLO WORLD %f %E %g %G %a %A %f %E %g %G %a %A\n");
    static constexpr std::array<ParamType, n_params> param_types = analyzeFormatString<n_params>("HELLO WORLD %f %E %g %G %a %A %f %E %g %G %a %A\n");
    size_t string_sizes[n_params] = {};
    uint64_t previousPrecision = -1;
    size_t alloc_size = getArgSizes(param_types, previousPrecision,
                            string_sizes, f1, f1, f1, f1, f1, f1, f2, f2, f2, f2, f2, f2);
    char * param_list = (char*)malloc(alloc_size);
    char * param_list_orig = param_list;
    storeArguments(param_types, string_sizes, &param_list, f1, f1, f1, f1, f1, f1, f2, f2, f2, f2, f2, f2);
    int plen = process_fmt("HELLO WORLD %f %E %g %G %a %A %f %E %g %G %a %A\n",
        n_params, &param_types[0], &string_sizes[0], param_list_orig, log_buffer, buflen, 0
    );
    std::cout<<"plen "<<plen<<std::endl;
    log_buffer[plen] = '\0';
    fprintf(stdout, "%s", log_buffer);
    fprintf(stdout, "HELLO WORLD %f %E %g %G %a %A %f %E %g %G %a %A\n", f1, f1, f1, f1, f1, f1, f2, f2, f2, f2, f2, f2);
}

TEST(test_fmt, test_flag_float)
{
    char* log_buffer = (char*)malloc(1024 * 1024);
    size_t buflen = 1024*1024;
    float f1 = 3.141592657;
    constexpr int n_params = countFmtParams("HELLO WORLD %f %+#3.3f\n");
    static constexpr std::array<ParamType, n_params> param_types = analyzeFormatString<n_params>("HELLO WORLD %f %+#3.3f\n");
    size_t string_sizes[n_params] = {};
    uint64_t previousPrecision = -1;
    size_t alloc_size = getArgSizes(param_types, previousPrecision,
                            string_sizes, f1, f1);
    char * param_list = (char*)malloc(alloc_size);
    char * param_list_orig = param_list;
    storeArguments(param_types, string_sizes, &param_list, f1, f1);
    int plen = process_fmt("HELLO WORLD %f %+#3.3f\n",
        n_params, &param_types[0], &string_sizes[0], param_list_orig, log_buffer, buflen, 0
    );
    std::cout<<"plen "<<plen<<std::endl;
    log_buffer[plen] = '\0';
    fprintf(stdout, "%s", log_buffer);
    fprintf(stdout, "HELLO WORLD %f %+#3.3f\n", f1, f1);
}

TEST(test_fmt, test_decode_string_fmt)
{
    char* log_buffer = (char*)malloc(1);
    ParamType param_type[1] = {kSTRING};
    size_t string_size[1] = {11};
    size_t buflen = 1;
    char* param_list = (char*)malloc(sizeof(uint32_t) + 11);
    *(uint32_t*)param_list = 11;
    memcpy(param_list + sizeof(uint32_t), "hello world", 11);
    int plen = process_fmt("%s", 1, &param_type[0], &string_size[0], (char*)param_list, log_buffer, buflen, 0);
    ASSERT_EQ(plen, 11);
    log_buffer[plen] = '\0';
    fprintf(stdout, "%s\n", log_buffer);
}

TEST(test_fmt, test_decode_non_string_fmt)
{
    char* log_buffer = (char*)malloc(1);
    char* origin_buffer = log_buffer;
    ParamType param_type[1] = {kNON_STRING};
    size_t string_size[1] = {8};
    uint64_t param = 34;
    size_t buflen = 1;
    int plen = process_fmt("%lu", 1, &param_type[0], &string_size[0], (char*)&param, log_buffer, buflen, 0);
    ASSERT_EQ(plen, 2);
    log_buffer[plen] = '\0';
    fprintf(stdout, "%s\n", log_buffer);
}

int main(int argc,char**argv){

  testing::InitGoogleTest(&argc,argv);

  return RUN_ALL_TESTS();

}