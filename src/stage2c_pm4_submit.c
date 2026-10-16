/* PROMO4 Stage 2c: first submission of a REAL PM4 payload from an independent client, in the verified record framing.
 *
 * Differs from stage 2b: stage 2b filled the payload with 0x80000000, which the kernel's record walker reads as an
 * immediate terminator (opcode 0x80, length 0) - so no PM4 was ever exercised. Here the buffer is laid out exactly like a
 * real stock GL buffer captured on this machine (reveng/stage11-simulation-layer1.md):
 *     +0x1c  : n+1            record header (opcode 0, length incl. header) - outside the submit range
 *     +0x20.. : n PM4 dwords
 *     +0x20+4n: 0             terminator
 * The PM4 words come from a file (hex, comma/space separated) that tools/promo4_sim/gate.py has ALREADY cleared
 * (packet structure + register policy + stock kernel dispatcher in the emulator). This program refuses anything that does not
 * fit the mapped buffer. Same trigger as stage 2b (re-map of memory type 1), bounded selector-8 finish loop, health checks
 * before and after, and every log line is F_FULLFSYNC'd so a crash leaves evidence.
 *
 * Usage: stage2c-pm4-submit PM4.hex LOGFILE
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>

static FILE *g_log;
static void LOG(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); fcntl(fileno(g_log), F_FULLFSYNC, 0); }
}
static int health_check(const char *label) {
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    if (!s) { LOG("[health:%s] no matching service\n", label); return 0; }
    io_connect_t c = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 1, &c);
    IOObjectRelease(s);
    if (kr != KERN_SUCCESS) { LOG("[health:%s] IOServiceOpen failed 0x%x\n", label, kr); return 0; }
    int o0 = 0, o1 = 0, o2 = 0;
    kr = IOConnectMethodScalarIScalarO(c, 3, 0, 3, &o0, &o1, &o2);
    IOServiceClose(c);
    if (kr != KERN_SUCCESS) { LOG("[health:%s] selector 3 FAILED 0x%x\n", label, kr); return 0; }
    LOG("[health:%s] out=(0x%x,0x%x,0x%x)\n", label, o0, o1, o2);
    return o0 == 0x810040 && o1 == 0xf7c8000 && o2 == 0x10000000;
}
static void dump(const char *tag, volatile uint32_t *b, int from, int n) {
    int i; LOG("[%s] words@+0x%x:", tag, from * 4);
    for (i = 0; i < n; i++) LOG(" %08x", b[from + i]);
    LOG("\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s PM4.hex LOGFILE\n", argv[0]); return 2; }
    g_log = fopen(argv[2], "a");
    uint32_t pm4[4096]; int n = 0;
    { FILE *f = fopen(argv[1], "r"); char tok[64]; int ch, t = 0;
      if (!f) { LOG("cannot open %s\n", argv[1]); return 2; }
      while ((ch = fgetc(f)) != EOF) {
          if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || ch == 'x' || ch == 'X') { if (t < 60) tok[t++] = ch; }
          else if (t) { tok[t] = 0; if (n < 4096) pm4[n++] = (uint32_t)strtoul(tok, NULL, 0); t = 0; }
      }
      if (t) { tok[t] = 0; if (n < 4096) pm4[n++] = (uint32_t)strtoul(tok, NULL, 0); }
      fclose(f); }
    LOG("[stage2c] %d PM4 dwords loaded from %s\n", n, argv[1]);
    if (n == 0) { LOG("[stage2c] empty stream, abort\n"); return 2; }
    int i;   /* zero dwords inside PM4 bodies are legal (the walker steps by record length); zero HEADERS are rejected by tools/promo4_sim/gate.py */

    if (!health_check("pre")) { LOG("[stage2c] pre health check failed - abort before any submission\n"); return 1; }
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    if (!service) return 1;
    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) { LOG("[stage2c] IOServiceOpen failed 0x%x\n", kr); return 1; }
    int o0, o1, o2;
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0, &o1, &o2);
    LOG("[stage2c] selector 3: 0x%x\n", kr);

    struct { uint32_t type, options; } maps[4] = { {0, 0x101}, {1, 1}, {2, 1}, {4, 1} };
    vm_address_t addrs[4] = {0, 0, 0, 0}; vm_size_t sizes[4] = {0, 0, 0, 0}; int mapped = 0;
    for (i = 0; i < 4; i++) {
        kr = IOConnectMapMemory(conn, maps[i].type, mach_task_self(), &addrs[i], &sizes[i], maps[i].options);
        if (kr != KERN_SUCCESS) { LOG("[stage2c] map type %u failed 0x%x\n", maps[i].type, kr); break; }
        mapped = i + 1;
        LOG("[stage2c] map type %u: addr=0x%lx size=0x%lx\n", maps[i].type, (unsigned long)addrs[i], (unsigned long)sizes[i]);
    }
    if (mapped != 4) goto cleanup;
    {
        volatile uint32_t *b = (volatile uint32_t *)addrs[1];
        uint32_t capdw = (uint32_t)((sizes[1] - 0x20) / 4);
        if ((uint32_t)n + 8 > capdw) { LOG("[stage2c] stream %d dw does not fit capacity %u - abort\n", n, capdw); goto cleanup; }
        dump("pre-write header", b, 0, 12);
        for (i = 0; i < n; i++) b[8 + i] = pm4[i];       /* +0x20.. */
        for (i = 0; i < 4; i++) b[8 + n + i] = 0;         /* terminator + slack */
        b[7] = (uint32_t)(n + 1);                         /* +0x1c record header, written LAST so the walker never sees a half-built stream */
        __asm__ __volatile__("sync" ::: "memory");
        dump("armed", b, 7, n + 6);
        LOG("[stage2c] triggering submission (re-map of type 1)\n");
        vm_address_t na = 0; vm_size_t ns = 0;
        kr = IOConnectMapMemory(conn, 1, mach_task_self(), &na, &ns, 1);
        LOG("[stage2c] re-map result 0x%x new=0x%lx size=0x%lx\n", kr, (unsigned long)na, (unsigned long)ns);
        dump("old buffer after kernel", b, 7, n + 6);
        if (kr == KERN_SUCCESS) { addrs[1] = na; sizes[1] = ns; }
        int tries = 0; kern_return_t fk;
        do { fk = IOConnectMethodScalarIStructureI(conn, 8, 0, 0, NULL); tries++; } while (fk == (kern_return_t)0xE00002D6 && tries < 200);
        LOG("[stage2c] selector 8 (finish) after %d tries: 0x%x\n", tries, fk);
    }
cleanup:
    for (i = mapped - 1; i >= 0; i--) { kr = IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]); LOG("[stage2c] unmap type %u: 0x%x\n", maps[i].type, kr); }
    kr = IOServiceClose(conn); LOG("[stage2c] close: 0x%x\n", kr);
    int ok = health_check("post");
    LOG("[stage2c] post-test health: %s\n", ok ? "OK" : "MISMATCH/FAILED");
    return ok ? 0 : 1;
}
