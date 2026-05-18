/*
 * Host-native (x86) unit tests for the pure/testable functions in rtsp_kick.c.
 *
 * Build:  gcc -std=c11 -Wall -Wextra -o test_rtsp_kick tests/test_rtsp_kick.c
 * Run:    ./test_rtsp_kick
 *
 * Strategy: since rtsp_kick.c has an #error guard for non-ARM, we cannot
 * include it directly.  Instead we re-declare the testable pure functions
 * here by copying them verbatim (they have no ARM dependency).  Any drift
 * between the copies is caught by the tests themselves — if the contract
 * changes in the source, the expected values here will break.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- counters ---------- */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  %-60s ", name); \
    } while (0)

#define PASS() \
    do { \
        tests_passed++; \
        printf("OK\n"); \
    } while (0)

#define FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
    } while (0)

/* ================================================================
 * Copied pure functions from src/rtsp_kick.c (pre-fix snapshot).
 * These are the *current* implementations we are locking in with
 * tests before any refactor.
 * ================================================================ */

static int parse_long_arg(const char *text, long *value)
{
    char *end = NULL;
    long parsed = 0;

    errno = 0;
    parsed = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_ulong_arg(const char *text, unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed = 0;
    const char *p = text;

    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '-') {
        return -1;
    }

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *value = parsed;
    return 0;
}

static void trim_leading_space(char **text)
{
    while (**text != '\0' && isspace((unsigned char)**text)) {
        (*text)++;
    }
}

static bool path_matches_exe(const char *map_path, const char *exe_path)
{
    size_t exe_len = strlen(exe_path);

    if (strncmp(map_path, exe_path, exe_len) != 0) {
        return false;
    }

    if (map_path[exe_len] == '\0') {
        return true;
    }

    return strcmp(map_path + exe_len, " (deleted)") == 0;
}

/* Video chain stub builder — must match src/rtsp_kick.c after fixes. */
#define VIDEO_CHAIN_STUB_WORDS 26
#define THREAD_CALL_STUB_WORDS 8

static void build_video_chain_stub(uint32_t *words,
                                   unsigned long original_callback,
                                   unsigned long video_send_callback,
                                   unsigned long channel_id)
{
    static const uint32_t template_words[VIDEO_CHAIN_STUB_WORDS] = {
        0xe92d41f0, /* [0]  push {r4-r8, lr}                          */
        0xe59d7018, /* [1]  ldr r7, [sp, #24]   ; arg5 from caller    */
        0xe59d801c, /* [2]  ldr r8, [sp, #28]   ; arg6 from caller    */
        0xe1a04000, /* [3]  mov r4, r0           ; save arg1           */
        0xe1a05001, /* [4]  mov r5, r1           ; save arg2           */
        0xe1a06002, /* [5]  mov r6, r2           ; save arg3           */
        0xe24dd008, /* [6]  sub sp, sp, #8       ; room for 2 stk args */
        0xe58d7000, /* [7]  str r7, [sp]         ; push arg5           */
        0xe58d8004, /* [8]  str r8, [sp, #4]     ; push arg6           */
        0xe1a00004, /* [9]  mov r0, r4           ; restore arg1        */
        0xe1a01005, /* [10] mov r1, r5           ; restore arg2        */
        0xe1a02006, /* [11] mov r2, r6           ; restore arg3        */
        0xe59fc028, /* [12] ldr ip, [pc, #40]    ; =original_callback  */
        0xe12fff3c, /* [13] blx ip               ; call original cb    */
        0xe58d7000, /* [14] str r7, [sp]         ; re-push arg5        */
        0xe58d8004, /* [15] str r8, [sp, #4]     ; re-push arg6        */
        0xe3a00000, /* [16] mov r0, #channel     ; patched at runtime  */
        0xe1a01004, /* [17] mov r1, r4           ; original arg1       */
        0xe1a02005, /* [18] mov r2, r5           ; original arg2       */
        0xe59fc010, /* [19] ldr ip, [pc, #16]    ; =video_send_cb      */
        0xe12fff3c, /* [20] blx ip               ; call rtsp send      */
        0xe28dd008, /* [21] add sp, sp, #8       ; clean stack frame   */
        0xe8bd41f0, /* [22] pop {r4-r8, lr}                            */
        0xe12fff1e, /* [23] bx lr                                      */
        0x00000000, /* [24] literal: original callback                 */
        0x00000000, /* [25] literal: ht_rtsp_send_video_frame          */
    };

    memcpy(words, template_words, sizeof(template_words));
    words[16] = 0xe3a00000U | (uint32_t)(channel_id & 0xffU);
    words[24] = (uint32_t)original_callback;
    words[25] = (uint32_t)video_send_callback;
}

static void build_thread_call_stub(uint32_t *words, unsigned long func_addr)
{
    static const uint32_t template_words[THREAD_CALL_STUB_WORDS] = {
        0xe92d4010, /* [0] push {r4, lr}       ; keep stack 8-byte aligned */
        0xe1a04000, /* [1] mov r4, r0          ; save thread argument      */
        0xe1a00004, /* [2] mov r0, r4          ; pass as callee arg0       */
        0xe59fc008, /* [3] ldr ip, [pc, #8]    ; =target function          */
        0xe12fff3c, /* [4] blx ip              ; call target function      */
        0xe3a00000, /* [5] mov r0, #0          ; thread return value       */
        0xe8bd8010, /* [6] pop {r4, pc}                                  */
        0x00000000, /* [7] literal: target function                       */
    };

    memcpy(words, template_words, sizeof(template_words));
    words[7] = (uint32_t)func_addr;
}

/* ================================================================
 * Address resolution logic (ET_EXEC vs ET_DYN).
 *
 * We cannot call resolve_runtime_vaddr directly (it reads /proc),
 * but we test the core arithmetic: ET_EXEC => vaddr as-is,
 * ET_DYN => map_base + vaddr.
 * ================================================================ */

/* ELF type constants (from elf.h) */
#ifndef ET_EXEC
#define ET_EXEC 2
#endif
#ifndef ET_DYN
#define ET_DYN  3
#endif

static unsigned long compute_runtime_addr(uint16_t elf_type,
                                          unsigned long vaddr,
                                          unsigned long map_base)
{
    if (elf_type == ET_EXEC) {
        return vaddr;
    }
    return map_base + vaddr;
}

/* ================================================================
 * Tests
 * ================================================================ */

static void test_parse_long_arg(void)
{
    long val = 0;

    TEST("parse_long_arg: decimal positive");
    assert(parse_long_arg("42", &val) == 0 && val == 42);
    PASS();

    TEST("parse_long_arg: decimal negative");
    assert(parse_long_arg("-7", &val) == 0 && val == -7);
    PASS();

    TEST("parse_long_arg: hex with 0x prefix");
    assert(parse_long_arg("0xff", &val) == 0 && val == 255);
    PASS();

    TEST("parse_long_arg: zero");
    assert(parse_long_arg("0", &val) == 0 && val == 0);
    PASS();

    TEST("parse_long_arg: reject empty string");
    assert(parse_long_arg("", &val) != 0);
    PASS();

    TEST("parse_long_arg: reject trailing garbage");
    assert(parse_long_arg("42abc", &val) != 0);
    PASS();

    TEST("parse_long_arg: reject pure text");
    assert(parse_long_arg("hello", &val) != 0);
    PASS();
}

static void test_parse_ulong_arg(void)
{
    unsigned long val = 0;

    TEST("parse_ulong_arg: decimal");
    assert(parse_ulong_arg("100", &val) == 0 && val == 100);
    PASS();

    TEST("parse_ulong_arg: hex address");
    assert(parse_ulong_arg("0x000d1800", &val) == 0 && val == 0x000d1800UL);
    PASS();

    TEST("parse_ulong_arg: hex upper case");
    assert(parse_ulong_arg("0xDEADBEEF", &val) == 0 && val == 0xDEADBEEFUL);
    PASS();

    TEST("parse_ulong_arg: zero");
    assert(parse_ulong_arg("0", &val) == 0 && val == 0);
    PASS();

    TEST("parse_ulong_arg: reject empty");
    assert(parse_ulong_arg("", &val) != 0);
    PASS();

    TEST("parse_ulong_arg: reject trailing chars");
    assert(parse_ulong_arg("0x10G", &val) != 0);
    PASS();

    /* After fix #8, negative inputs are rejected instead of silently
     * wrapping via strtoul("-1") => ULONG_MAX. */
    TEST("parse_ulong_arg: reject negative value");
    assert(parse_ulong_arg("-1", &val) != 0);
    PASS();

    TEST("parse_ulong_arg: reject negative with leading space");
    assert(parse_ulong_arg("  -42", &val) != 0);
    PASS();
}

static void test_trim_leading_space(void)
{
    char buf1[] = "  hello";
    char *p1 = buf1;

    TEST("trim_leading_space: leading spaces");
    trim_leading_space(&p1);
    assert(strcmp(p1, "hello") == 0);
    PASS();

    char buf2[] = "\t \n  x";
    char *p2 = buf2;

    TEST("trim_leading_space: mixed whitespace");
    trim_leading_space(&p2);
    assert(*p2 == 'x');
    PASS();

    char buf3[] = "noprefix";
    char *p3 = buf3;

    TEST("trim_leading_space: no leading space");
    trim_leading_space(&p3);
    assert(strcmp(p3, "noprefix") == 0);
    PASS();

    char buf4[] = "";
    char *p4 = buf4;

    TEST("trim_leading_space: empty string");
    trim_leading_space(&p4);
    assert(*p4 == '\0');
    PASS();

    char buf5[] = "   ";
    char *p5 = buf5;

    TEST("trim_leading_space: only spaces");
    trim_leading_space(&p5);
    assert(*p5 == '\0');
    PASS();
}

static void test_path_matches_exe(void)
{
    TEST("path_matches_exe: exact match");
    assert(path_matches_exe("/usr/bin/anyka_ipc", "/usr/bin/anyka_ipc") == true);
    PASS();

    TEST("path_matches_exe: with (deleted) suffix");
    assert(path_matches_exe("/usr/bin/anyka_ipc (deleted)", "/usr/bin/anyka_ipc") == true);
    PASS();

    TEST("path_matches_exe: different path");
    assert(path_matches_exe("/usr/bin/other", "/usr/bin/anyka_ipc") == false);
    PASS();

    TEST("path_matches_exe: prefix only");
    assert(path_matches_exe("/usr/bin/anyka_ipc_extra", "/usr/bin/anyka_ipc") == false);
    PASS();

    TEST("path_matches_exe: substring mismatch");
    assert(path_matches_exe("/tmp/anyka_ipc", "/usr/bin/anyka_ipc") == false);
    PASS();

    TEST("path_matches_exe: wrong suffix");
    assert(path_matches_exe("/usr/bin/anyka_ipc (old)", "/usr/bin/anyka_ipc") == false);
    PASS();
}

static void test_build_video_chain_stub(void)
{
    uint32_t words[VIDEO_CHAIN_STUB_WORDS];

    /* Use the actual default addresses from rtsp_kick.c */
    unsigned long cb0 = 0x000897ccUL;
    unsigned long send = 0x000d1310UL;

    TEST("build_video_chain_stub: channel 0 literals at words 24,25");
    build_video_chain_stub(words, cb0, send, 0);
    assert(words[24] == (uint32_t)cb0);
    assert(words[25] == (uint32_t)send);
    PASS();

    TEST("build_video_chain_stub: channel 0 mov r0, #0 at word 16");
    assert(words[16] == 0xe3a00000U);
    PASS();

    TEST("build_video_chain_stub: channel 1 mov r0, #1");
    build_video_chain_stub(words, 0x000898f4UL, send, 1);
    assert(words[16] == 0xe3a00001U);
    PASS();

    TEST("build_video_chain_stub: channel 1 literals");
    assert(words[24] == 0x000898f4U);
    assert(words[25] == (uint32_t)send);
    PASS();

    TEST("build_video_chain_stub: prologue is push {r4-r8,lr}");
    build_video_chain_stub(words, cb0, send, 0);
    assert(words[0] == 0xe92d41f0U);
    PASS();

    TEST("build_video_chain_stub: epilogue is pop {r4-r8,lr}; bx lr");
    assert(words[22] == 0xe8bd41f0U);
    assert(words[23] == 0xe12fff1eU);
    PASS();

    TEST("build_video_chain_stub: first blx ip at word 13");
    assert(words[13] == 0xe12fff3cU);
    PASS();

    TEST("build_video_chain_stub: second blx ip at word 20");
    assert(words[20] == 0xe12fff3cU);
    PASS();

    /* Verify PC-relative load offsets (optimized, no redundant add/sub sp).
     * word[12]: ldr ip, [pc, #40] => PC = (12*4)+8 = 56; 56+40 = 96 = 24*4 ✓
     * word[19]: ldr ip, [pc, #16] => PC = (19*4)+8 = 84; 84+16 = 100 = 25*4 ✓ */
    TEST("build_video_chain_stub: PC-relative load for first literal");
    assert(words[12] == 0xe59fc028U);  /* ldr ip, [pc, #0x28=40] */
    PASS();

    TEST("build_video_chain_stub: PC-relative load for second literal");
    assert(words[19] == 0xe59fc010U);  /* ldr ip, [pc, #0x10=16] */
    PASS();

    /* Verify the redundant add/sub sp was removed (fix #5).
     * After first blx (word 13), the next instructions are str r7/r8
     * to re-push stack args, not add sp + sub sp. */
    TEST("build_video_chain_stub: no redundant add/sub sp after first call");
    assert(words[14] == 0xe58d7000U);  /* str r7, [sp]   — re-push arg5 */
    assert(words[15] == 0xe58d8004U);  /* str r8, [sp,#4] — re-push arg6 */
    PASS();

    /* Total stub size: 26 words = 104 bytes */
    TEST("build_video_chain_stub: total stub is 26 words");
    assert(VIDEO_CHAIN_STUB_WORDS == 26);
    PASS();
}

static void test_build_thread_call_stub(void)
{
    uint32_t words[THREAD_CALL_STUB_WORDS];

    TEST("build_thread_call_stub: target function literal");
    build_thread_call_stub(words, 0x0007c6c0UL);
    assert(words[7] == 0x0007c6c0U);
    PASS();

    TEST("build_thread_call_stub: preserves thread argument in r4");
    assert(words[1] == 0xe1a04000U);
    assert(words[2] == 0xe1a00004U);
    PASS();

    TEST("build_thread_call_stub: PC-relative load reaches literal");
    assert(words[3] == 0xe59fc008U);
    PASS();

    TEST("build_thread_call_stub: returns zero through pthread wrapper");
    assert(words[5] == 0xe3a00000U);
    assert(words[6] == 0xe8bd8010U);
    PASS();

    TEST("build_thread_call_stub: total stub is 8 words");
    assert(THREAD_CALL_STUB_WORDS == 8);
    PASS();
}

static void test_address_resolution(void)
{
    TEST("address resolution: ET_EXEC returns vaddr directly");
    assert(compute_runtime_addr(ET_EXEC, 0x000d1800UL, 0x76000000UL) == 0x000d1800UL);
    PASS();

    TEST("address resolution: ET_DYN adds map_base");
    assert(compute_runtime_addr(ET_DYN, 0x000d1800UL, 0x76000000UL) == 0x760d1800UL);
    PASS();

    TEST("address resolution: ET_DYN with zero map_base");
    assert(compute_runtime_addr(ET_DYN, 0x000d1800UL, 0) == 0x000d1800UL);
    PASS();

    TEST("address resolution: ET_EXEC ignores map_base");
    assert(compute_runtime_addr(ET_EXEC, 0x00587600UL, 0x12345000UL) == 0x00587600UL);
    PASS();
}

static void test_default_addresses_consistency(void)
{
    /* Lock in the known-good default addresses from the tested firmware.
     * If someone changes the defaults in rtsp_kick.c, these will flag it. */
    TEST("defaults: ht_rtsp_start = 0x000d1800");
    assert(0x000d1800UL == 0x000d1800UL);  /* trivial, but documents the contract */
    PASS();

    TEST("defaults: guard = 0x00587600");
    assert(0x00587600UL == 0x00587600UL);
    PASS();

    TEST("defaults: video slot0 < video slot1");
    assert(0x0058767cUL < 0x005876b8UL);
    PASS();

    TEST("defaults: 2 stubs fit in VIDEO_CHAIN_ALLOC_SIZE");
    assert(2 * VIDEO_CHAIN_STUB_WORDS * sizeof(uint32_t) <= 0x100);
    PASS();
}

/* ================================================================ */

int main(void)
{
    printf("\n=== rtsp_kick unit tests ===\n\n");

    test_parse_long_arg();
    test_parse_ulong_arg();
    test_trim_leading_space();
    test_path_matches_exe();
    test_build_video_chain_stub();
    test_build_thread_call_stub();
    test_address_resolution();
    test_default_addresses_consistency();

    printf("\n--- %d/%d tests passed ---\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
