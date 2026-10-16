/* PROMO4 Stage 3m: READ-ONLY search for the GPU retire/completion counter in the accelerator-wide shared mapping (memory type 2).
 * Kernel side (ATIRadeonX1000::waitForRetiredTimeStamp): the counter is a byte-reversed (little-endian) word at (this+0x864)+(this+0x868) in
 * DMA memory; stamps are signed-compared. This probe opens a SECOND connection (safe alongside a live GL context, stage 1), maps type 2 and
 * diffs it around N tiny GL draw+glFinish cycles to find words that advance once per submission. Writes nothing to any GPU-visible memory.
 * Usage: stage3m-stamp-probe LOGFILE [N]
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static FILE *g_log;
static void LOG(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); fcntl(fileno(g_log), F_FULLFSYNC, 0); }
}
static uint32_t bswap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }

int main(int argc, char **argv) {
    g_log = argc > 1 ? fopen(argv[1], "a") : NULL;
    int N = argc > 2 ? atoi(argv[2]) : 8;
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glViewport(0, 0, 4, 4); glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();

    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    io_connect_t conn = MACH_PORT_NULL; kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 1, &conn); IOObjectRelease(svc);
    if (kr) { LOG("IOServiceOpen failed 0x%x\n", kr); return 1; }
    vm_address_t a0 = 0, a1 = 0, a2 = 0; vm_size_t s0 = 0, s1 = 0, s2 = 0;
    IOConnectMapMemory(conn, 0, mach_task_self(), &a0, &s0, 0x101);
    IOConnectMapMemory(conn, 1, mach_task_self(), &a1, &s1, 1);
    kr = IOConnectMapMemory(conn, 2, mach_task_self(), &a2, &s2, 1);
    LOG("[3m] type2 map: kr=0x%x addr=0x%lx size=0x%lx\n", kr, (unsigned long)a2, (unsigned long)s2);
    if (kr || !a2 || s2 < 0x100 || s2 > 0x100000) return 1;
    LOG("[3m] type0 map: addr=0x%lx size=0x%lx\n", (unsigned long)a0, (unsigned long)s0);
    uint32_t *cur0 = (uint32_t *)a0, *prev0 = malloc(s0 ? s0 : 4); int n0 = s0 / 4;
    if (s0) memcpy(prev0, cur0, s0);
    { int j; LOG("[3m] type0 initial nonzero words:"); for (j = 0; j < n0 && j < 1024; j++) if (cur0[j]) LOG(" +0x%x=%08x", j * 4, cur0[j]); LOG("\n"); }
    int nw = s2 / 4, i, k; uint32_t *cur = (uint32_t *)a2, *prev = malloc(s2);
    memcpy(prev, cur, s2);
    for (k = 1; k <= N; k++) {
        glClear(GL_COLOR_BUFFER_BIT); glFinish();
        int shown = 0;
        LOG("[3m] after draw+finish #%d: changed words (offset: old -> new, byte-swapped new):\n", k);
        for (i = 0; i < nw; i++) if (cur[i] != prev[i]) { if (shown++ < 24) LOG("   +0x%04x: %08x -> %08x  (bswap %08x -> %08x)\n", i * 4, prev[i], cur[i], bswap(prev[i]), bswap(cur[i])); }
        LOG("   (%d words changed)\n", shown);
        memcpy(prev, cur, s2);
        { int sh0 = 0, j; LOG("   type0 changed:"); for (j = 0; j < n0; j++) if (cur0[j] != prev0[j]) { if (sh0++ < 12) LOG(" +0x%x: %08x->%08x (bswap %08x->%08x)", j * 4, prev0[j], cur0[j], bswap(prev0[j]), bswap(cur0[j])); }
          LOG(" [%d words]\n", sh0); memcpy(prev0, cur0, s0); }
    }
    aglSetCurrentContext(NULL); aglDestroyContext(ctx);
    return 0;
}
