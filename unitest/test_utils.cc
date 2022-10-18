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

TEST(test_utils, test_fmt_simple)
{
    constexpr int n_params = internal::utils::countFmtParams("HELLO WORLD %c %d %lf %f %s\n");
    ASSERT_EQ(n_params, 5);
    static constexpr std::array<internal::ParamType, n_params> param_types = internal::utils::analyzeFormatString<n_params>("HELLO WORLD %s %d %lf %f %s\n");
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
    ASSERT_EQ(alloc_size, 32);
}

int main(int argc,char**argv){

  testing::InitGoogleTest(&argc,argv);

  return RUN_ALL_TESTS();

}