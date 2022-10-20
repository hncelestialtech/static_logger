#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <string>

#include<gtest/gtest.h>

#include "static_log_internal.h"
#include "static_log_utils.h"
using namespace static_log;

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

int 
generateTimePrefix(uint64_t timestamp, char* raw_data) {
    const int prefix_len = 32;
    const int nano_bits = 9;
    *raw_data = '[';
    raw_data++;
    uint64_t nano = timestamp % 1000000000;
    timestamp = timestamp / 1000000000;
    struct tm* tm_now = localtime((time_t*)&timestamp);
    memcpy(raw_data, std::to_string(tm_now->tm_year + 1900).c_str(), 4);
    raw_data += 4;
    *raw_data = '-';
    convertInt2Str(tm_now->tm_mon + 1, raw_data);
    *raw_data++ = '-';
    convertInt2Str(tm_now->tm_mday, raw_data);
    *raw_data++ = '-';
    convertInt2Str(tm_now->tm_hour, raw_data);
    *raw_data++ = ':';
    convertInt2Str(tm_now->tm_min, raw_data);
    *raw_data++ = ':';
    convertInt2Str(tm_now->tm_sec, raw_data);
    *raw_data++ = '.';
    int nanolen = strlen(std::to_string(nano).c_str());
    memcpy(raw_data, std::to_string(nano).c_str(), nanolen);
    raw_data += nanolen;
    if (nanolen < nano_bits) {
        int bits = nano_bits - nanolen;
        for(int i = 0; i < bits; ++i)
            *raw_data++ = '0';
    }
    *raw_data = ']';
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
    generateTimePrefix(tsns, test_buf);
    printf("%s\n", test_buf);
    ASSERT_EQ(strlen(test_buf), 30);
}

static int
process_fmt(
        const char* fmt, 
        const int num_params, 
        const internal::ParamType* param_types, 
        const char* param_list, 
        char* log_buffer, size_t buflen)
{
    int origin_len = buflen;
    size_t fmt_list_len = strlen(fmt);
    int pos = 0;
    int param_idx = 0;
    bool success = true;
    while (pos < fmt_list_len) {
        if (fmt[pos] != '%') {
            *log_buffer++ = *fmt;
            buflen--;
            continue;
        } else {
            ++pos;
            int fmt_single_len = 1;
            if (fmt[pos] == '%') {
                *log_buffer++ = '%';
                buflen--;
                ++pos;
                continue;
            } else {
                while (!internal::utils::isTerminal(fmt[pos]))
                    fmt_single_len++;
                char* fmt_single;
                char static_fmt_cache[100];
                bool dynamic_fmt = false;
                if (fmt_single_len >= 100) {
                    memset(static_fmt_cache, 0, 100);
                    fmt_single = static_fmt_cache;
                } else {
                    fmt_single = (char*)malloc(fmt_single_len);
                    dynamic_fmt = true;
                }
                memcpy(fmt_single, fmt + pos, fmt_single_len);
                pos += fmt_single_len;
                size_t log_fmt_len = 0;

                if (param_idx < num_params) {
                    if (param_types[param_idx] > internal::ParamType::NON_STRING) {
                        uint32_t string_size = *(uint32_t*)param_list;
                        param_list += sizeof(uint32_t);
                        char *param_str = (char*)malloc(string_size);
                        memcpy(param_str, param_list, string_size);
                        log_fmt_len = snprintf(log_buffer, buflen, fmt_single, param_str);
                        free(param_str);
                    }
                    else {
#pragma GCC diagnostic ignored "-Wformat"
                        uint64_t param = *(uint64_t*)param_list;
                        param_list += sizeof(uint64_t);
                        log_fmt_len = snprintf(log_buffer, buflen, fmt_single, param);
#pragma GCC diagnostic pop
                    }
                    log_buffer += log_fmt_len;
                    buflen -= log_fmt_len;
                } else {
                    fprintf(stderr, "Failed to fmt log");
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
    return success? origin_len - buflen: -1;
}

#pragma GCC diagnostic ignored "-Wformat"
static int
decodeNonStringFmt(
    char* __restrict__ log_buffer, 
    size_t log_buffer_len, 
    char * fmt, 
    const char* param, 
    size_t param_size)
{
    int fmt_len = 0;
    char terminal_flag = fmt[strlen(fmt) - 1];
    switch (param_size) {
    case sizeof(char):
        {
            const char* param8 = param;
            if (terminal_flag == 'c')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int8_t*)param8);
            else if (terminal_flag == 'd' || terminal_flag == 'i')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int8_t*)param8);
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(uint8_t*)param8);
            else 
                fprintf(stderr, "Failed to parse fmt with a one bytes long param\n");
            break;
        }
    case sizeof(uint16_t):
        {
            const uint16_t* param16 = (uint16_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int16_t*)param16);
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(uint16_t*)param16);
            else 
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *param16);
            break;
        }
    case sizeof(uint32_t):
        {
            const uint32_t* param32 = (uint32_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int32_t*)param32);
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(uint32_t*)param32);
            else if (terminal_flag == 'f' || terminal_flag == 'F' || terminal_flag == 'e' ||terminal_flag == 'e' || 
                terminal_flag == 'E' || terminal_flag == 'g' || terminal_flag == 'G'|| terminal_flag == 'a' ||terminal_flag == 'A') {
                static_assert(sizeof(float) == sizeof(uint32_t));
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(float*)param32);
            }            
            else 
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    case sizeof(uint64_t):
        {
            const uint64_t* param64 = (uint64_t*)param;
            if (terminal_flag == 'd' || terminal_flag == 'i')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(int64_t*)param64);
            else if (terminal_flag == 'u' || terminal_flag == 'o' || terminal_flag == 'x' || terminal_flag == 'X')
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(uint64_t*)param64);
            else if (terminal_flag == 'f' || terminal_flag == 'F' || terminal_flag == 'e' ||terminal_flag == 'e' || 
                terminal_flag == 'E' || terminal_flag == 'g' || terminal_flag == 'G'|| terminal_flag == 'a' ||terminal_flag == 'A') {
                static_assert(sizeof(double) == sizeof(uint64_t));
                fmt_len = snprintf(log_buffer, log_buffer_len, fmt, *(double*)param64);
            }
            else
                fprintf(stderr, "Failed to parse fmt with a two bytes long param\n");
            break;
        }
    default:
        fprintf(stderr, "Failed to decode fmt param, got size %ld\n", param_size);
        break;
    }
    return fmt_len;
}
#pragma GCC diagnostic pop

static int
process_fmt(
        const char* fmt, 
        const int num_params, 
        const internal::ParamType* param_types,
        size_t* param_size_list,
        const char* param_list, 
        char* log_buffer, size_t buflen)
{
    char* origin_buffer = log_buffer;
    int origin_len = buflen;
    size_t fmt_list_len = strlen(fmt);
    int pos = 0;
    int param_idx = 0;
    bool success = true;
    while (pos < fmt_list_len) {
        if (fmt[pos] != '%') {
            *log_buffer++ = fmt[pos++];
            buflen--;
            continue;
        } else {
            ++pos;
            int fmt_single_len = 1;
            if (fmt[pos] == '%') {
                *log_buffer++ = '%';
                buflen--;
                ++pos;
                continue;
            } else {
                int fmt_start_pos = pos - 1;
                while (!internal::utils::isTerminal(fmt[pos])) {
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
                    if (param_types[param_idx] > internal::ParamType::NON_STRING) {
                        uint32_t string_size = *(uint32_t*)param_list;
                        param_list += sizeof(uint32_t);
                        char *param_str = (char*)malloc(string_size);
                        memcpy(param_str, param_list, string_size);
                        param_str[string_size] = '\0';
                        log_fmt_len = snprintf(log_buffer, buflen, fmt_single, param_str);
                        free(param_str);
                    }
                    else {
                        log_fmt_len = decodeNonStringFmt(log_buffer, buflen, fmt_single, param_list, param_size_list[param_idx]);
                        param_list += param_size_list[param_idx];
                    }
                    log_buffer += log_fmt_len;
                    buflen -= log_fmt_len;
                    param_idx++;
                } else {
                    fprintf(stderr, "Failed to fmt log");
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
    return success? origin_len - buflen: -1;
}

TEST(test_utils, test_fmt_simple)
{
    constexpr int n_params = internal::utils::countFmtParams("HELLO WORLD %c %d %lf %f %s\n");
    ASSERT_EQ(n_params, 5);
    
    static constexpr std::array<internal::ParamType, n_params> param_types = internal::utils::analyzeFormatString<n_params>("HELLO WORLD %c %d %lf %f %s\n");
    size_t len = strlen("HELLO WORLD %c %d %lf %f %s\n");
    char c_parma = 'a';
    char* s_param = "hello world";
    int int_param = 1;
    double fl_param = 3.14;
    float f_param = 3.22;
    size_t string_sizes[5 + 1] = {};
    uint64_t previousPrecision = -1;
    size_t alloc_size = internal::utils::getArgSizes(param_types, previousPrecision,
                            string_sizes, c_parma, int_param, fl_param, f_param, s_param);
    uint64_t store_bufsize = sizeof(char) + sizeof(int) + sizeof(double) + sizeof(float) + sizeof(uint32_t) + 11;
    ASSERT_EQ(alloc_size, store_bufsize);
    
    static std::array<size_t, n_params> param_size = internal::utils::analyzeFmtParamSize(c_parma, int_param, fl_param, f_param, s_param);
    ASSERT_EQ(param_size[0], 1);
    ASSERT_EQ(param_size[1], 4);
    ASSERT_EQ(param_size[2], 8);
    ASSERT_EQ(param_size[3], 4);
    ASSERT_EQ(param_size[4], 8);
    
    char log_buffer[1024 * 1024];
    char * param_list = (char*)malloc(alloc_size);
    char * param_list_orig = param_list;
    internal::utils::storeArguments(param_types, string_sizes, &param_list, c_parma, int_param, fl_param, f_param, s_param);
    
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
        5, &param_types[0], &param_size[0], param_list_orig, log_buffer, 1024*1024
    );
    log_buffer[plen] = '\0';
    fprintf(stdout, "%s\n", log_buffer);
    // free(write_buf);
}


int main(int argc,char**argv){

  testing::InitGoogleTest(&argc,argv);

  return RUN_ALL_TESTS();

}