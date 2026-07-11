/**
  ******************************************************************************
  * @file    test_util.h
  * @brief   Minimal single-file test harness (no external deps). Each test
  *          executable prints its failures and returns non-zero for ctest.
  ******************************************************************************
  */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int g_test_failures = 0;
static int g_test_checks   = 0;

#define CHECK(cond) do { \
    g_test_checks++; \
    if(!(cond)){ \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_test_failures++; \
    } \
} while(0)

#define CHECK_EQ_U32(got, want) do { \
    g_test_checks++; \
    unsigned long _g = (unsigned long)(got), _w = (unsigned long)(want); \
    if(_g != _w){ \
        printf("FAIL %s:%d: %s == 0x%lx, want 0x%lx\n", \
               __FILE__, __LINE__, #got, _g, _w); \
        g_test_failures++; \
    } \
} while(0)

#define CHECK_MEM(got, want, n) do { \
    g_test_checks++; \
    if(memcmp((got), (want), (n)) != 0){ \
        printf("FAIL %s:%d: memcmp(%s, %s, %d)\n", \
               __FILE__, __LINE__, #got, #want, (int)(n)); \
        const unsigned char *_g = (const unsigned char *)(got); \
        const unsigned char *_w = (const unsigned char *)(want); \
        for(int _i = 0; _i < (int)(n); _i++){ \
            if(_g[_i] != _w[_i]){ \
                printf("  byte %d: got %02x want %02x\n", _i, _g[_i], _w[_i]); \
            } \
        } \
        g_test_failures++; \
    } \
} while(0)

#define CHECK_STR(got, want) do { \
    g_test_checks++; \
    if(strcmp((got), (want)) != 0){ \
        printf("FAIL %s:%d: %s == \"%s\", want \"%s\"\n", \
               __FILE__, __LINE__, #got, (got), (want)); \
        g_test_failures++; \
    } \
} while(0)

static int test_report(const char *name){
    if(g_test_failures){
        printf("%s: %d/%d checks FAILED\n", name, g_test_failures, g_test_checks);
        return 1;
    }
    printf("%s: %d checks OK\n", name, g_test_checks);
    return 0;
}

#endif /* TEST_UTIL_H */
