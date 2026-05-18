#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __arm__
#error "rtsp_kick currently supports 32-bit ARM targets only"
#endif

#ifndef __WALL
#define __WALL 0x40000000
#endif

#define DEFAULT_FUNC_VADDR  0x000d1800UL
#define DEFAULT_GUARD_VADDR 0x00587600UL
#define DEFAULT_TRAP_ADDR   0x00000000UL
#define DEFAULT_MALLOC_VADDR             0x000798b4UL
#define DEFAULT_THREAD_CREATE_VADDR      0x00000000UL
#define DEFAULT_THREAD_STACK_SIZE        0x00010000UL
#define DEFAULT_VIDEO_SEND_VADDR         0x000d1310UL
#define DEFAULT_VIDEO_SLOT0_VADDR        0x0058767cUL
#define DEFAULT_VIDEO_SLOT1_VADDR        0x005876b8UL
#define DEFAULT_EXPECTED_VIDEO_CB0_VADDR 0x000897ccUL
#define DEFAULT_EXPECTED_VIDEO_CB1_VADDR 0x000898f4UL
#define DEFAULT_WAIT_TIMEOUT_SECONDS     5

#define MAX_REMOTE_STACK_WORDS 8
#define VIDEO_CHAIN_STUB_WORDS 26
/* Two stubs = 2 * 26 * 4 = 208 bytes.  The remaining 48 bytes of padding
 * are reserved for the cacheflush trampoline (16 bytes) and cache-line
 * alignment. */
#define VIDEO_CHAIN_ALLOC_SIZE 0x100UL

/* EABI cacheflush trampoline: 4 ARM instructions written into the padding
 * area after the two stubs.  Called via remote_call with r0 = start,
 * r1 = end, r2 = 0.  This ensures the I-cache sees the newly written
 * stub code on ARM cores with separate I/D caches. */
#define CACHEFLUSH_TRAMPOLINE_WORDS 4
static const uint32_t cacheflush_trampoline[CACHEFLUSH_TRAMPOLINE_WORDS] = {
    0xe59f7004, /* ldr r7, [pc, #4]   ; load __ARM_NR_cacheflush */
    0xef000000, /* svc #0             ; invoke kernel cacheflush   */
    0xe12fff1e, /* bx lr              ; return to trap address     */
    0x000f0002, /* .word 0x000f0002   ; __ARM_NR_cacheflush        */
};

#define THREAD_CALL_STUB_WORDS       8
#define THREAD_CALL_ALLOC_SIZE       0x80UL
#define THREAD_CALL_TID_OFFSET       0x40UL
#define THREAD_CALL_TRAMPOLINE_OFFSET 0x50UL

enum {
    ARM_R0 = 0,
    ARM_R1 = 1,
    ARM_R2 = 2,
    ARM_R3 = 3,
    ARM_SP = 13,
    ARM_LR = 14,
    ARM_PC = 15,
    ARM_CPSR = 16,
};

typedef struct {
    unsigned long uregs[18];
} arm_regs_t;

typedef struct {
    pid_t tid;
    unsigned long arg0;
    unsigned long arg1;
    unsigned long arg2;
    unsigned long arg3;
    unsigned long func_vaddr;
    unsigned long guard_vaddr;
    unsigned long trap_addr;
    unsigned long malloc_vaddr;
    unsigned long thread_create_vaddr;
    unsigned long thread_stack_size;
    unsigned long video_send_vaddr;
    unsigned long video_slot0_vaddr;
    unsigned long video_slot1_vaddr;
    unsigned long expected_video_cb0_vaddr;
    unsigned long expected_video_cb1_vaddr;
    unsigned long peek_vaddr;
    const char *arg0_string;
    bool dry_run;
    bool install_video_chain;
    bool call_in_new_thread;
    bool no_guard_check;
    bool no_start_call;
    bool peek_mode;
    bool verbose;
    bool arg0_was_set;
    bool arg0_string_set;
    int peek_words;
    int wait_timeout_seconds;
} config_t;

static void usage(FILE *stream, const char *argv0)
{
    fprintf(stream,
            "usage: %s [options] <tid>\n"
            "\n"
            "One-shot ptrace caller for Anyka/Tuya stock firmware functions.\n"
            "Defaults target ht_rtsp_start, but --func-vaddr and --arg0..--arg3\n"
            "can be used for other in-process stock helpers.\n"
            "\n"
            "options:\n"
            "  --dry-run               attach, resolve addresses, read guard, detach\n"
            "  --no-guard-check        continue even if the RTSP guard is non-zero\n"
            "  --verbose               print extra diagnostics\n"
            "  --arg0 N                argument passed in r0 (default: 0)\n"
            "  --arg0-string TEXT      malloc TEXT in the target and pass that pointer in r0\n"
            "  --arg1 N                argument passed in r1 (default: 0)\n"
            "  --arg2 N                argument passed in r2 (default: 0)\n"
            "  --arg3 N                argument passed in r3 (default: 0)\n"
            "  --wait-timeout N        seconds to wait for the remote call to return (default: %d)\n"
            "  --call-in-new-thread    spawn --func-vaddr in a new ak_thread_create thread using --arg0 as thread arg\n"
            "  --func-vaddr HEX        virtual address of target function (default ht_rtsp_start: 0x%08lx)\n"
            "  --guard-vaddr HEX       virtual address of RTSP guard (default: 0x%08lx)\n"
            "  --trap-addr HEX         return trap address in LR (default: 0x%08lx)\n"
            "  --install-video-chain   allocate a heap stub and chain Tuya video callbacks to ht_rtsp_send_video_frame\n"
            "  --no-start-call         with --install-video-chain, do not call ht_rtsp_start first\n"
            "  --malloc-vaddr HEX      virtual address of malloc@plt (default: 0x%08lx)\n"
            "  --thread-create-vaddr HEX virtual address of ak_thread_create for --call-in-new-thread\n"
            "  --thread-stack-size N   stack bytes for --call-in-new-thread (default: 0x%08lx)\n"
            "  --video-send-vaddr HEX  virtual address of ht_rtsp_send_video_frame (default: 0x%08lx)\n"
            "  --video-slot0-vaddr HEX virtual address of video callback slot 0 (default: 0x%08lx)\n"
            "  --video-slot1-vaddr HEX virtual address of video callback slot 1 (default: 0x%08lx)\n"
            "  --expected-video-cb0 HEX expected current callback for slot 0 (default: 0x%08lx)\n"
            "  --expected-video-cb1 HEX expected current callback for slot 1 (default: 0x%08lx)\n"
            "  --peek-vaddr HEX        resolve and dump words from a target virtual address\n"
            "  --peek-words N          number of 32-bit words to dump with --peek-vaddr (default: 1)\n"
            "  -h, --help              show this help\n",
            argv0,
            DEFAULT_WAIT_TIMEOUT_SECONDS,
            DEFAULT_FUNC_VADDR,
            DEFAULT_GUARD_VADDR,
            DEFAULT_TRAP_ADDR,
            DEFAULT_MALLOC_VADDR,
            DEFAULT_THREAD_STACK_SIZE,
            DEFAULT_VIDEO_SEND_VADDR,
            DEFAULT_VIDEO_SLOT0_VADDR,
            DEFAULT_VIDEO_SLOT1_VADDR,
            DEFAULT_EXPECTED_VIDEO_CB0_VADDR,
            DEFAULT_EXPECTED_VIDEO_CB1_VADDR);
}

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

    /* Skip leading whitespace to find the sign character.
     * strtoul silently accepts "-1" and returns ULONG_MAX, which
     * would be a confusing silent misparse for address arguments. */
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

static int read_exe_path(pid_t pid, char *buffer, size_t buffer_size)
{
    char proc_path[64];
    ssize_t len = 0;

    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
    len = readlink(proc_path, buffer, buffer_size - 1);
    if (len < 0) {
        return -1;
    }

    buffer[len] = '\0';
    return 0;
}

static int read_elf_type(const char *path, uint16_t *elf_type)
{
    Elf32_Ehdr ehdr;
    FILE *fp = fopen(path, "rb");

    if (fp == NULL) {
        return -1;
    }

    if (fread(&ehdr, 1, sizeof(ehdr), fp) != sizeof(ehdr)) {
        fclose(fp);
        errno = EIO;
        return -1;
    }
    fclose(fp);

    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS32) {
        errno = ENOEXEC;
        return -1;
    }

    *elf_type = ehdr.e_type;
    return 0;
}

static int find_lowest_exe_map(pid_t pid, const char *exe_path, unsigned long *map_start)
{
    char maps_path[64];
    char line[1024];
    FILE *fp = NULL;
    bool found = false;
    unsigned long lowest = 0;

    snprintf(maps_path, sizeof(maps_path), "/proc/%ld/maps", (long)pid);
    fp = fopen(maps_path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long start = 0;
        unsigned long end = 0;
        unsigned long offset = 0;
        unsigned int major = 0;
        unsigned int minor = 0;
        unsigned long inode = 0;
        char perms[5] = {0};
        char path[512] = {0};
        char *path_ptr = path;
        int parsed = sscanf(line,
                            "%lx-%lx %4s %lx %x:%x %lu %511[^\n]",
                            &start,
                            &end,
                            perms,
                            &offset,
                            &major,
                            &minor,
                            &inode,
                            path);
        (void)end;
        (void)offset;
        (void)major;
        (void)minor;
        (void)inode;
        (void)perms;

        if (parsed < 7) {
            continue;
        }
        if (parsed < 8) {
            path[0] = '\0';
        }

        trim_leading_space(&path_ptr);
        if (*path_ptr == '\0') {
            continue;
        }
        if (!path_matches_exe(path_ptr, exe_path)) {
            continue;
        }

        if (!found || start < lowest) {
            lowest = start;
            found = true;
        }
    }

    fclose(fp);

    if (!found) {
        errno = ESRCH;
        return -1;
    }

    *map_start = lowest;
    return 0;
}

/* Resolve the ELF base once (reads /proc/pid/exe and /proc/pid/maps)
 * and return the map_base and elf_type.  All subsequent vaddr-to-runtime
 * translations use apply_base() which is pure arithmetic. */
static int resolve_base(pid_t pid,
                        uint16_t *elf_type_out,
                        char *exe_path,
                        size_t exe_path_size,
                        unsigned long *map_base_out)
{
    uint16_t elf_type = 0;
    unsigned long map_base = 0;

    if (read_exe_path(pid, exe_path, exe_path_size) != 0) {
        return -1;
    }
    if (read_elf_type(exe_path, &elf_type) != 0) {
        return -1;
    }

    if (elf_type == ET_EXEC) {
        *map_base_out = 0;
    } else if (elf_type == ET_DYN) {
        if (find_lowest_exe_map(pid, exe_path, &map_base) != 0) {
            return -1;
        }
        *map_base_out = map_base;
    } else {
        errno = ENOTSUP;
        return -1;
    }

    *elf_type_out = elf_type;
    return 0;
}

static unsigned long apply_base(uint16_t elf_type,
                                unsigned long vaddr,
                                unsigned long map_base)
{
    if (elf_type == ET_EXEC) {
        return vaddr;
    }
    return map_base + vaddr;
}

static volatile sig_atomic_t wait_timed_out = 0;

static void alarm_handler(int signo)
{
    (void)signo;
    wait_timed_out = 1;
}

static int wait_for_stop(pid_t tid, int *status, int timeout_seconds)
{
    struct sigaction sa;
    struct sigaction old_sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sa.sa_flags = 0;  /* do NOT set SA_RESTART — we need EINTR */
    sigaction(SIGALRM, &sa, &old_sa);

    wait_timed_out = 0;
    alarm((unsigned int)timeout_seconds);

    for (;;) {
        pid_t waited = waitpid(tid, status, __WALL);
        if (waited < 0) {
            if (errno == EINTR && !wait_timed_out) {
                continue;
            }
            alarm(0);
            sigaction(SIGALRM, &old_sa, NULL);
            if (wait_timed_out) {
                errno = ETIMEDOUT;
            }
            return -1;
        }
        if (waited == tid) {
            alarm(0);
            sigaction(SIGALRM, &old_sa, NULL);
            return 0;
        }
    }
}

static int peek_word(pid_t tid, unsigned long addr, unsigned long *value)
{
    long data = 0;

    errno = 0;
    data = ptrace(PTRACE_PEEKDATA, tid, (void *)addr, NULL);
    if (data == -1 && errno != 0) {
        return -1;
    }

    *value = (unsigned long)data;
    return 0;
}

static int poke_word(pid_t tid, unsigned long addr, unsigned long value)
{
    if (ptrace(PTRACE_POKEDATA, tid, (void *)addr, (void *)value) != 0) {
        return -1;
    }
    return 0;
}

static int poke_bytes(pid_t tid, unsigned long addr, const unsigned char *data, size_t data_len)
{
    size_t offset = 0;

    while (offset < data_len) {
        unsigned long word = 0;
        size_t i = 0;
        size_t remaining = data_len - offset;
        size_t chunk = remaining < sizeof(unsigned long) ? remaining : sizeof(unsigned long);

        for (i = 0; i < chunk; ++i) {
            word |= ((unsigned long)data[offset + i]) << (8U * i);
        }

        if (poke_word(tid, addr + (unsigned long)offset, word) != 0) {
            return -1;
        }

        offset += sizeof(unsigned long);
    }

    return 0;
}

static int remote_call(pid_t tid,
                       unsigned long func_addr,
                       unsigned long trap_addr,
                       unsigned long r0,
                       unsigned long r1,
                       unsigned long r2,
                       unsigned long r3,
                       const unsigned long *stack_words,
                       size_t stack_word_count,
                       int wait_timeout_seconds,
                       bool verbose,
                       unsigned long *return_r0)
{
    arm_regs_t regs;
    arm_regs_t original_regs;
    arm_regs_t stop_regs;
    unsigned long saved_stack_words[MAX_REMOTE_STACK_WORDS];
    int wait_status = 0;
    size_t i = 0;
    int call_status = 0;

    if (stack_word_count > MAX_REMOTE_STACK_WORDS) {
        errno = E2BIG;
        return -1;
    }

    memset(&regs, 0, sizeof(regs));
    memset(&original_regs, 0, sizeof(original_regs));
    memset(&stop_regs, 0, sizeof(stop_regs));
    memset(saved_stack_words, 0, sizeof(saved_stack_words));

    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) != 0) {
        return -1;
    }
    original_regs = regs;

    for (i = 0; i < stack_word_count; ++i) {
        unsigned long addr = regs.uregs[ARM_SP] + (unsigned long)(i * sizeof(unsigned long));

        if (peek_word(tid, addr, &saved_stack_words[i]) != 0) {
            call_status = -1;
            goto restore;
        }
        if (poke_word(tid, addr, stack_words[i]) != 0) {
            call_status = -1;
            goto restore;
        }
    }

    regs.uregs[ARM_R0] = r0;
    regs.uregs[ARM_R1] = r1;
    regs.uregs[ARM_R2] = r2;
    regs.uregs[ARM_R3] = r3;
    regs.uregs[ARM_PC] = func_addr;
    regs.uregs[ARM_LR] = trap_addr;
    regs.uregs[ARM_CPSR] &= ~0x20UL;

    if (verbose) {
        fprintf(stderr,
                "remote_call pc=0x%08lx r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx sp=0x%08lx\n",
                regs.uregs[ARM_PC],
                regs.uregs[ARM_R0],
                regs.uregs[ARM_R1],
                regs.uregs[ARM_R2],
                regs.uregs[ARM_R3],
                regs.uregs[ARM_SP]);
    }

    if (ptrace(PTRACE_SETREGS, tid, NULL, &regs) != 0) {
        call_status = -1;
        goto restore;
    }

    if (ptrace(PTRACE_CONT, tid, NULL, 0) != 0) {
        call_status = -1;
        goto restore;
    }
    if (wait_for_stop(tid, &wait_status, wait_timeout_seconds) != 0) {
        call_status = -1;
        goto restore;
    }
    if (!WIFSTOPPED(wait_status) || WSTOPSIG(wait_status) != SIGSEGV) {
        errno = EPROTO;
        call_status = -1;
        goto restore;
    }
    if (ptrace(PTRACE_GETREGS, tid, NULL, &stop_regs) != 0) {
        call_status = -1;
        goto restore;
    }
    if (stop_regs.uregs[ARM_PC] != trap_addr) {
        errno = EFAULT;
        call_status = -1;
        goto restore;
    }

    if (return_r0 != NULL) {
        *return_r0 = stop_regs.uregs[ARM_R0];
    }

restore:
    for (i = 0; i < stack_word_count; ++i) {
        unsigned long addr = original_regs.uregs[ARM_SP] + (unsigned long)(i * sizeof(unsigned long));

        if (poke_word(tid, addr, saved_stack_words[i]) != 0 && call_status == 0) {
            call_status = -1;
        }
    }
    if (ptrace(PTRACE_SETREGS, tid, NULL, &original_regs) != 0) {
        call_status = -1;
    }

    if (call_status != 0) {
        return -1;
    }
    return 0;
}

static int allocate_remote_string(const config_t *cfg,
                                  pid_t tid,
                                  unsigned long malloc_runtime,
                                  const char *text,
                                  unsigned long *remote_addr)
{
    unsigned char *buffer = NULL;
    unsigned long allocation = 0;
    size_t text_len = strlen(text) + 1U;
    size_t padded_len = ((text_len + sizeof(unsigned long) - 1U) / sizeof(unsigned long)) *
                        sizeof(unsigned long);

    if (malloc_runtime == 0) {
        fprintf(stderr, "--arg0-string requires a valid --malloc-vaddr\n");
        errno = EINVAL;
        return -1;
    }

    if (remote_call(tid,
                    malloc_runtime,
                    cfg->trap_addr,
                    (unsigned long)padded_len,
                    0,
                    0,
                    0,
                    NULL,
                    0,
                    cfg->wait_timeout_seconds,
                    cfg->verbose,
                    &allocation) != 0) {
        perror("remote malloc for arg0 string");
        return -1;
    }
    if (allocation == 0) {
        fprintf(stderr, "remote malloc for arg0 string returned NULL\n");
        errno = ENOMEM;
        return -1;
    }

    buffer = calloc(1, padded_len);
    if (buffer == NULL) {
        return -1;
    }
    memcpy(buffer, text, text_len);

    if (poke_bytes(tid, allocation, buffer, padded_len) != 0) {
        free(buffer);
        perror("write remote arg0 string");
        return -1;
    }

    free(buffer);
    *remote_addr = allocation;

    if (cfg->verbose) {
        fprintf(stderr,
                "wrote arg0 string at 0x%08lx (%lu bytes): %s\n",
                allocation,
                (unsigned long)text_len,
                text);
    }

    return 0;
}

static int detach_target(pid_t tid)
{
    return ptrace(PTRACE_DETACH, tid, NULL, 0);
}

static const char *elf_type_name(uint16_t elf_type)
{
    switch (elf_type) {
    case ET_EXEC:
        return "ET_EXEC";
    case ET_DYN:
        return "ET_DYN";
    default:
        return "unknown";
    }
}

static void print_status_line(const config_t *cfg,
                              const char *exe_path,
                              uint16_t elf_type,
                              unsigned long map_base,
                              unsigned long func_runtime,
                              unsigned long guard_runtime,
                              unsigned long guard_value)
{
    fprintf(stderr,
            "target=%ld exe=%s type=%s map_base=0x%08lx\n"
            "func=0x%08lx guard=0x%08lx guard_value=0x%08lx\n",
            (long)cfg->tid,
            exe_path,
            elf_type_name(elf_type),
            map_base,
            func_runtime,
            guard_runtime,
            guard_value);
}

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
        /* Stack args may have been clobbered by the callee, but r4-r8
         * are callee-saved so our saved values survive.  Re-store the
         * stack arguments for the second call (sp frame stays as-is). */
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

static int spawn_thread_call(const config_t *cfg,
                             pid_t tid,
                             unsigned long malloc_runtime,
                             unsigned long thread_create_runtime,
                             unsigned long func_runtime)
{
    unsigned long allocation = 0;
    unsigned long stub_addr = 0;
    unsigned long tid_slot_addr = 0;
    unsigned long trampoline_addr = 0;
    unsigned long stack_words[1] = {0xffffffffUL};
    unsigned long thread_create_result = 0;
    unsigned long created_tid = 0;
    uint32_t stub[THREAD_CALL_STUB_WORDS];
    size_t i = 0;

    if (thread_create_runtime == 0) {
        fprintf(stderr, "--call-in-new-thread requires --thread-create-vaddr\n");
        errno = EINVAL;
        return -1;
    }

    if (cfg->dry_run) {
        fprintf(stderr,
                "dry-run: would malloc(0x%08lx), write a %lu-byte thread stub, "
                "and call ak_thread_create(stub, arg=0x%08lx)\n",
                (unsigned long)THREAD_CALL_ALLOC_SIZE,
                (unsigned long)(THREAD_CALL_STUB_WORDS * sizeof(uint32_t)),
                cfg->arg0);
        return 0;
    }

    if (remote_call(tid,
                    malloc_runtime,
                    cfg->trap_addr,
                    THREAD_CALL_ALLOC_SIZE,
                    0,
                    0,
                    0,
                    NULL,
                    0,
                    cfg->wait_timeout_seconds,
                    cfg->verbose,
                    &allocation) != 0) {
        perror("remote malloc");
        return -1;
    }
    if (allocation == 0) {
        fprintf(stderr, "remote malloc returned NULL\n");
        errno = ENOMEM;
        return -1;
    }

    stub_addr = allocation;
    tid_slot_addr = allocation + THREAD_CALL_TID_OFFSET;
    trampoline_addr = allocation + THREAD_CALL_TRAMPOLINE_OFFSET;

    build_thread_call_stub(stub, func_runtime);

    for (i = 0; i < THREAD_CALL_STUB_WORDS; ++i) {
        if (poke_word(tid, stub_addr + (unsigned long)(i * sizeof(uint32_t)), stub[i]) != 0) {
            perror("write thread call stub");
            return -1;
        }
    }
    if (poke_word(tid, tid_slot_addr, 0) != 0) {
        perror("write remote tid slot");
        return -1;
    }

    for (i = 0; i < CACHEFLUSH_TRAMPOLINE_WORDS; ++i) {
        if (poke_word(tid, trampoline_addr + (unsigned long)(i * sizeof(uint32_t)),
                      cacheflush_trampoline[i]) != 0) {
            perror("write cacheflush trampoline");
            return -1;
        }
    }

    if (remote_call(tid,
                    trampoline_addr,
                    cfg->trap_addr,
                    stub_addr,
                    stub_addr + (unsigned long)(THREAD_CALL_STUB_WORDS * sizeof(uint32_t)),
                    0,
                    0,
                    NULL,
                    0,
                    cfg->wait_timeout_seconds,
                    cfg->verbose,
                    NULL) != 0) {
        fprintf(stderr,
                "warning: cacheflush trampoline failed (thread stub may still work on this kernel)\n");
    }

    if (remote_call(tid,
                    thread_create_runtime,
                    cfg->trap_addr,
                    tid_slot_addr,
                    stub_addr,
                    (unsigned long)cfg->arg0,
                    cfg->thread_stack_size,
                    stack_words,
                    1,
                    cfg->wait_timeout_seconds,
                    cfg->verbose,
                    &thread_create_result) != 0) {
        perror("remote ak_thread_create");
        return -1;
    }
    if (thread_create_result != 0) {
        fprintf(stderr, "ak_thread_create returned 0x%08lx\n", thread_create_result);
        errno = ECHILD;
        return -1;
    }
    if (peek_word(tid, tid_slot_addr, &created_tid) != 0) {
        perror("peek remote thread id");
        return -1;
    }

    fprintf(stderr,
            "spawned remote thread for function call: stub=0x%08lx tid_slot=0x%08lx thread_id=0x%08lx\n",
            stub_addr,
            tid_slot_addr,
            created_tid);
    return 0;
}

static int install_video_chain(const config_t *cfg,
                               pid_t tid,
                               unsigned long malloc_runtime,
                               unsigned long video_send_runtime,
                               unsigned long slot0_runtime,
                               unsigned long slot1_runtime,
                               unsigned long expected_cb0_runtime,
                               unsigned long expected_cb1_runtime)
{
    unsigned long current_cb0 = 0;
    unsigned long current_cb1 = 0;
    unsigned long allocation = 0;
    uint32_t stub0[VIDEO_CHAIN_STUB_WORDS];
    uint32_t stub1[VIDEO_CHAIN_STUB_WORDS];
    size_t i = 0;
    unsigned long stub0_addr = 0;
    unsigned long stub1_addr = 0;

    if (peek_word(tid, slot0_runtime, &current_cb0) != 0) {
        perror("peek video slot 0");
        return -1;
    }
    if (peek_word(tid, slot1_runtime, &current_cb1) != 0) {
        perror("peek video slot 1");
        return -1;
    }

    fprintf(stderr,
            "video slots before chain: slot0=0x%08lx slot1=0x%08lx\n",
            current_cb0,
            current_cb1);

    if (current_cb0 != expected_cb0_runtime || current_cb1 != expected_cb1_runtime) {
        fprintf(stderr,
                "refusing to install video chain because current callbacks do not match the expected Tuya wrappers "
                "(slot0 expected 0x%08lx, slot1 expected 0x%08lx)\n",
                expected_cb0_runtime,
                expected_cb1_runtime);
        errno = EEXIST;
        return -1;
    }

    if (cfg->dry_run) {
        fprintf(stderr,
                "dry-run: would call malloc(0x%08lx), write two %lu-byte stubs, and patch slot0/slot1\n",
                (unsigned long)VIDEO_CHAIN_ALLOC_SIZE,
                (unsigned long)(VIDEO_CHAIN_STUB_WORDS * sizeof(uint32_t)));
        return 0;
    }

    if (remote_call(tid,
                    malloc_runtime,
                    cfg->trap_addr,
                    VIDEO_CHAIN_ALLOC_SIZE,
                    0,
                    0,
                    0,
                    NULL,
                    0,
                    cfg->wait_timeout_seconds,
                    cfg->verbose,
                    &allocation) != 0) {
        perror("remote malloc");
        return -1;
    }
    if (allocation == 0) {
        fprintf(stderr, "remote malloc returned NULL\n");
        errno = ENOMEM;
        return -1;
    }

    stub0_addr = allocation;
    stub1_addr = allocation + (unsigned long)(VIDEO_CHAIN_STUB_WORDS * sizeof(uint32_t));

    build_video_chain_stub(stub0, current_cb0, video_send_runtime, 0);
    build_video_chain_stub(stub1, current_cb1, video_send_runtime, 1);

    for (i = 0; i < VIDEO_CHAIN_STUB_WORDS; ++i) {
        if (poke_word(tid, stub0_addr + (unsigned long)(i * sizeof(uint32_t)), stub0[i]) != 0) {
            perror("write stub0");
            return -1;
        }
    }
    for (i = 0; i < VIDEO_CHAIN_STUB_WORDS; ++i) {
        if (poke_word(tid, stub1_addr + (unsigned long)(i * sizeof(uint32_t)), stub1[i]) != 0) {
            perror("write stub1");
            return -1;
        }
    }

    /* Write a cacheflush trampoline into the padding area after the two
     * stubs and call it to ensure the I-cache sees the new code.  The
     * kernel may already flush on PTRACE_POKEDATA for executable VMAs,
     * but this makes the flush explicit and safe on all ARM kernels. */
    {
        unsigned long trampoline_addr = allocation +
            2 * (unsigned long)(VIDEO_CHAIN_STUB_WORDS * sizeof(uint32_t));

        for (i = 0; i < CACHEFLUSH_TRAMPOLINE_WORDS; ++i) {
            if (poke_word(tid, trampoline_addr + (unsigned long)(i * sizeof(uint32_t)),
                          cacheflush_trampoline[i]) != 0) {
                perror("write cacheflush trampoline");
                return -1;
            }
        }

        if (remote_call(tid,
                        trampoline_addr,
                        cfg->trap_addr,
                        allocation,
                        allocation + 2 * (unsigned long)(VIDEO_CHAIN_STUB_WORDS * sizeof(uint32_t)),
                        0,
                        0,
                        NULL,
                        0,
                        cfg->wait_timeout_seconds,
                        cfg->verbose,
                        NULL) != 0) {
            fprintf(stderr,
                    "warning: cacheflush trampoline failed (stubs may still work on this kernel)\n");
        }
    }

    /* Patch the callback slot pointers to redirect to the new stubs.
     * Note: other threads in anyka_ipc are NOT stopped by our ptrace
     * attach (only the attached tid is stopped).  There is a brief
     * window between the two poke_word calls where slot0 points to the
     * new stub while slot1 still points to the old callback.  In the
     * worst case, one video frame on one channel goes through the old
     * path during that window — no crash, just one frame without RTSP
     * delivery. */
    if (poke_word(tid, slot0_runtime, stub0_addr) != 0) {
        perror("patch slot0");
        return -1;
    }
    if (poke_word(tid, slot1_runtime, stub1_addr) != 0) {
        perror("patch slot1");
        return -1;
    }

    fprintf(stderr,
            "installed video chain stubs: stub0=0x%08lx stub1=0x%08lx slot0=0x%08lx slot1=0x%08lx\n",
            stub0_addr,
            stub1_addr,
            slot0_runtime,
            slot1_runtime);
    return 0;
}

int main(int argc, char **argv)
{
    config_t cfg;
    int i = 0;
    bool attached = false;
    int call_status = 0;
    int wait_status = 0;
    uint16_t elf_type = 0;
    unsigned long func_runtime = 0;
    unsigned long guard_runtime = 0;
    unsigned long guard_value = 0;
    unsigned long malloc_runtime = 0;
    unsigned long thread_create_runtime = 0;
    unsigned long video_send_runtime = 0;
    unsigned long video_slot0_runtime = 0;
    unsigned long video_slot1_runtime = 0;
    unsigned long expected_video_cb0_runtime = 0;
    unsigned long expected_video_cb1_runtime = 0;
    unsigned long peek_runtime = 0;
    unsigned long map_base = 0;
    char exe_path[512] = {0};

    memset(&cfg, 0, sizeof(cfg));
    cfg.func_vaddr = DEFAULT_FUNC_VADDR;
    cfg.guard_vaddr = DEFAULT_GUARD_VADDR;
    cfg.trap_addr = DEFAULT_TRAP_ADDR;
    cfg.malloc_vaddr = DEFAULT_MALLOC_VADDR;
    cfg.thread_create_vaddr = DEFAULT_THREAD_CREATE_VADDR;
    cfg.thread_stack_size = DEFAULT_THREAD_STACK_SIZE;
    cfg.video_send_vaddr = DEFAULT_VIDEO_SEND_VADDR;
    cfg.video_slot0_vaddr = DEFAULT_VIDEO_SLOT0_VADDR;
    cfg.video_slot1_vaddr = DEFAULT_VIDEO_SLOT1_VADDR;
    cfg.expected_video_cb0_vaddr = DEFAULT_EXPECTED_VIDEO_CB0_VADDR;
    cfg.expected_video_cb1_vaddr = DEFAULT_EXPECTED_VIDEO_CB1_VADDR;
    cfg.peek_words = 1;
    cfg.wait_timeout_seconds = DEFAULT_WAIT_TIMEOUT_SECONDS;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--dry-run") == 0) {
            cfg.dry_run = true;
        } else if (strcmp(arg, "--install-video-chain") == 0) {
            cfg.install_video_chain = true;
        } else if (strcmp(arg, "--call-in-new-thread") == 0) {
            cfg.call_in_new_thread = true;
        } else if (strcmp(arg, "--no-guard-check") == 0) {
            cfg.no_guard_check = true;
        } else if (strcmp(arg, "--no-start-call") == 0) {
            cfg.no_start_call = true;
        } else if (strcmp(arg, "--verbose") == 0) {
            cfg.verbose = true;
        } else if (strcmp(arg, "--arg0") == 0) {
            if (cfg.arg0_string_set) {
                fprintf(stderr, "--arg0 and --arg0-string cannot be used together\n");
                return 2;
            }
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.arg0) != 0) {
                fprintf(stderr, "invalid value for --arg0\n");
                return 2;
            }
            cfg.arg0_was_set = true;
        } else if (strcmp(arg, "--arg0-string") == 0) {
            if (cfg.arg0_was_set) {
                fprintf(stderr, "--arg0 and --arg0-string cannot be used together\n");
                return 2;
            }
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "invalid value for --arg0-string\n");
                return 2;
            }
            cfg.arg0_string = argv[++i];
            cfg.arg0_string_set = true;
        } else if (strcmp(arg, "--arg1") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.arg1) != 0) {
                fprintf(stderr, "invalid value for --arg1\n");
                return 2;
            }
        } else if (strcmp(arg, "--arg2") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.arg2) != 0) {
                fprintf(stderr, "invalid value for --arg2\n");
                return 2;
            }
        } else if (strcmp(arg, "--arg3") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.arg3) != 0) {
                fprintf(stderr, "invalid value for --arg3\n");
                return 2;
            }
        } else if (strcmp(arg, "--wait-timeout") == 0) {
            long parsed = 0;
            if (i + 1 >= argc || parse_long_arg(argv[++i], &parsed) != 0 || parsed <= 0) {
                fprintf(stderr, "invalid value for --wait-timeout\n");
                return 2;
            }
            cfg.wait_timeout_seconds = (int)parsed;
        } else if (strcmp(arg, "--func-vaddr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.func_vaddr) != 0) {
                fprintf(stderr, "invalid value for --func-vaddr\n");
                return 2;
            }
        } else if (strcmp(arg, "--guard-vaddr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.guard_vaddr) != 0) {
                fprintf(stderr, "invalid value for --guard-vaddr\n");
                return 2;
            }
        } else if (strcmp(arg, "--trap-addr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.trap_addr) != 0) {
                fprintf(stderr, "invalid value for --trap-addr\n");
                return 2;
            }
        } else if (strcmp(arg, "--malloc-vaddr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.malloc_vaddr) != 0) {
                fprintf(stderr, "invalid value for --malloc-vaddr\n");
                return 2;
            }
        } else if (strcmp(arg, "--thread-create-vaddr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.thread_create_vaddr) != 0) {
                fprintf(stderr, "invalid value for --thread-create-vaddr\n");
                return 2;
            }
        } else if (strcmp(arg, "--thread-stack-size") == 0) {
            unsigned long parsed = 0;
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &parsed) != 0 || parsed == 0) {
                fprintf(stderr, "invalid value for --thread-stack-size\n");
                return 2;
            }
            cfg.thread_stack_size = parsed;
        } else if (strcmp(arg, "--video-send-vaddr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.video_send_vaddr) != 0) {
                fprintf(stderr, "invalid value for --video-send-vaddr\n");
                return 2;
            }
        } else if (strcmp(arg, "--video-slot0-vaddr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.video_slot0_vaddr) != 0) {
                fprintf(stderr, "invalid value for --video-slot0-vaddr\n");
                return 2;
            }
        } else if (strcmp(arg, "--video-slot1-vaddr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.video_slot1_vaddr) != 0) {
                fprintf(stderr, "invalid value for --video-slot1-vaddr\n");
                return 2;
            }
        } else if (strcmp(arg, "--expected-video-cb0") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.expected_video_cb0_vaddr) != 0) {
                fprintf(stderr, "invalid value for --expected-video-cb0\n");
                return 2;
            }
        } else if (strcmp(arg, "--expected-video-cb1") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.expected_video_cb1_vaddr) != 0) {
                fprintf(stderr, "invalid value for --expected-video-cb1\n");
                return 2;
            }
        } else if (strcmp(arg, "--peek-vaddr") == 0) {
            if (i + 1 >= argc || parse_ulong_arg(argv[++i], &cfg.peek_vaddr) != 0) {
                fprintf(stderr, "invalid value for --peek-vaddr\n");
                return 2;
            }
            cfg.peek_mode = true;
        } else if (strcmp(arg, "--peek-words") == 0) {
            long parsed = 0;
            if (i + 1 >= argc || parse_long_arg(argv[++i], &parsed) != 0 || parsed <= 0) {
                fprintf(stderr, "invalid value for --peek-words\n");
                return 2;
            }
            cfg.peek_words = (int)parsed;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else if (arg[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", arg);
            usage(stderr, argv[0]);
            return 2;
        } else if (cfg.tid == 0) {
            long parsed = 0;
            if (parse_long_arg(arg, &parsed) != 0 || parsed <= 0) {
                fprintf(stderr, "invalid tid: %s\n", arg);
                return 2;
            }
            cfg.tid = (pid_t)parsed;
        } else {
            fprintf(stderr, "unexpected extra argument: %s\n", arg);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (cfg.tid <= 0) {
        usage(stderr, argv[0]);
        return 2;
    }

    if (cfg.install_video_chain && cfg.call_in_new_thread) {
        fprintf(stderr, "--install-video-chain and --call-in-new-thread cannot be used together\n");
        return 2;
    }
    if (cfg.call_in_new_thread && cfg.no_start_call) {
        fprintf(stderr, "--no-start-call is not meaningful with --call-in-new-thread\n");
        return 2;
    }
    if (cfg.call_in_new_thread && cfg.thread_create_vaddr == 0) {
        fprintf(stderr, "--call-in-new-thread requires --thread-create-vaddr\n");
        return 2;
    }

    /* Resolve the ELF base address once — this reads /proc/pid/exe and
     * /proc/pid/maps a single time instead of once per address. */
    if (resolve_base(cfg.tid, &elf_type, exe_path, sizeof(exe_path), &map_base) != 0) {
        perror("resolve ELF base");
        return 1;
    }

    if (cfg.peek_mode) {
        peek_runtime = apply_base(elf_type, cfg.peek_vaddr, map_base);
    } else {
        func_runtime  = apply_base(elf_type, cfg.func_vaddr, map_base);
        guard_runtime = apply_base(elf_type, cfg.guard_vaddr, map_base);
        if (cfg.install_video_chain || cfg.call_in_new_thread || cfg.arg0_string_set) {
            malloc_runtime             = apply_base(elf_type, cfg.malloc_vaddr, map_base);
        }
        if (cfg.call_in_new_thread) {
            thread_create_runtime      = apply_base(elf_type, cfg.thread_create_vaddr, map_base);
        }
        if (cfg.install_video_chain) {
            video_send_runtime         = apply_base(elf_type, cfg.video_send_vaddr, map_base);
            video_slot0_runtime        = apply_base(elf_type, cfg.video_slot0_vaddr, map_base);
            video_slot1_runtime        = apply_base(elf_type, cfg.video_slot1_vaddr, map_base);
            expected_video_cb0_runtime = apply_base(elf_type, cfg.expected_video_cb0_vaddr, map_base);
            expected_video_cb1_runtime = apply_base(elf_type, cfg.expected_video_cb1_vaddr, map_base);
        }
    }

    if (ptrace(PTRACE_ATTACH, cfg.tid, NULL, NULL) != 0) {
        perror("PTRACE_ATTACH");
        return 1;
    }
    attached = true;

    if (wait_for_stop(cfg.tid, &wait_status, DEFAULT_WAIT_TIMEOUT_SECONDS) != 0) {
        perror("waitpid after attach");
        call_status = 1;
        goto cleanup;
    }
    if (!WIFSTOPPED(wait_status)) {
        fprintf(stderr, "target did not enter a ptrace stop after attach\n");
        call_status = 1;
        goto cleanup;
    }

    if (cfg.peek_mode) {
        int word_index = 0;

        fprintf(stderr,
                "target=%ld exe=%s type=%s map_base=0x%08lx\n"
                "peek_vaddr=0x%08lx resolved=0x%08lx words=%d\n",
                (long)cfg.tid,
                exe_path,
                elf_type_name(elf_type),
                map_base,
                cfg.peek_vaddr,
                peek_runtime,
                cfg.peek_words);

        for (word_index = 0; word_index < cfg.peek_words; ++word_index) {
            unsigned long addr = peek_runtime + ((unsigned long)word_index * sizeof(unsigned long));
            unsigned long value = 0;

            if (peek_word(cfg.tid, addr, &value) != 0) {
                perror("PTRACE_PEEKDATA");
                call_status = 1;
                goto cleanup;
            }

            fprintf(stderr, "0x%08lx: 0x%08lx\n", addr, value);
        }

        call_status = 0;
        goto cleanup;
    }

    if (peek_word(cfg.tid, guard_runtime, &guard_value) != 0) {
        perror("PTRACE_PEEKDATA guard");
        call_status = 1;
        goto cleanup;
    }

    if (cfg.verbose || cfg.dry_run) {
        print_status_line(&cfg,
                          exe_path,
                          elf_type,
                          map_base,
                          func_runtime,
                          guard_runtime,
                          guard_value);
    }

    if (!cfg.install_video_chain && !cfg.no_guard_check && guard_value != 0) {
        fprintf(stderr,
                "refusing to call target function because guard is non-zero "
                "(0x%08lx). Use --no-guard-check to override.\n",
                guard_value);
        call_status = 3;
        goto cleanup;
    }

    if (cfg.arg0_string_set) {
        if (cfg.dry_run) {
            fprintf(stderr,
                    "dry-run: would malloc and write arg0 string (%lu bytes): %s\n",
                    (unsigned long)(strlen(cfg.arg0_string) + 1U),
                    cfg.arg0_string);
        } else if (allocate_remote_string(&cfg,
                                          cfg.tid,
                                          malloc_runtime,
                                          cfg.arg0_string,
                                          &cfg.arg0) != 0) {
            call_status = 1;
            goto cleanup;
        }
    }

    if (cfg.dry_run) {
        if (cfg.install_video_chain) {
            if (install_video_chain(&cfg,
                                    cfg.tid,
                                    malloc_runtime,
                                    video_send_runtime,
                                    video_slot0_runtime,
                                    video_slot1_runtime,
                                    expected_video_cb0_runtime,
                                    expected_video_cb1_runtime) != 0) {
                call_status = 1;
                goto cleanup;
            }
        } else if (cfg.call_in_new_thread) {
            if (spawn_thread_call(&cfg,
                                  cfg.tid,
                                  malloc_runtime,
                                  thread_create_runtime,
                                  func_runtime) != 0) {
                call_status = 1;
                goto cleanup;
            }
        }
        call_status = 0;
        goto cleanup;
    }

    if (!cfg.no_start_call) {
        bool skip_start_call = false;

        if (cfg.install_video_chain && guard_value != 0 && !cfg.no_guard_check) {
            fprintf(stderr,
                    "guard is already non-zero (0x%08lx); skipping ht_rtsp_start and continuing with video chain install\n",
                    guard_value);
            skip_start_call = true;
        }

        if (!skip_start_call) {
            if (cfg.call_in_new_thread) {
                if (spawn_thread_call(&cfg,
                                      cfg.tid,
                                      malloc_runtime,
                                      thread_create_runtime,
                                      func_runtime) != 0) {
                    call_status = 1;
                    goto cleanup;
                }
            } else {
                if (remote_call(cfg.tid,
                                func_runtime,
                                cfg.trap_addr,
                                (unsigned long)cfg.arg0,
                                (unsigned long)cfg.arg1,
                                (unsigned long)cfg.arg2,
                                (unsigned long)cfg.arg3,
                                NULL,
                                0,
                                cfg.wait_timeout_seconds,
                                cfg.verbose,
                                NULL) != 0) {
                    perror("remote function call");
                    call_status = 1;
                    goto cleanup;
                }
            }
        }
    }

    if (cfg.install_video_chain) {
        if (install_video_chain(&cfg,
                                cfg.tid,
                                malloc_runtime,
                                video_send_runtime,
                                video_slot0_runtime,
                                video_slot1_runtime,
                                expected_video_cb0_runtime,
                                expected_video_cb1_runtime) != 0) {
            call_status = 1;
            goto cleanup;
        }
    }

cleanup:
    if (attached) {
        if (detach_target(cfg.tid) != 0) {
            perror("PTRACE_DETACH");
            if (call_status == 0) {
                call_status = 1;
            }
        }
    }

    if (call_status == 0 && !cfg.dry_run && !cfg.peek_mode && cfg.call_in_new_thread) {
        fprintf(stderr, "remote thread call was started and target thread was detached cleanly\n");
    } else if (call_status == 0 && !cfg.dry_run && !cfg.peek_mode && !cfg.install_video_chain) {
        fprintf(stderr, "remote function call completed and registers were restored\n");
    } else if (call_status == 0 && !cfg.dry_run && cfg.install_video_chain) {
        fprintf(stderr, "video chain install completed and target thread was detached cleanly\n");
    }

    return call_status;
}
