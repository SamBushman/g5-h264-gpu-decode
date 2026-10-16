/* PROMO4 Stage 3i: READ-ONLY dump of a live AGLContext's command-buffer bookkeeping, using the CORRECTED field map from
 * reveng/stage3g-cursor-field-misidentified.md:  ctx+0x17d8 chain-link, +0x17dc write cursor, +0x17e0 end/limit, +0x17e4 base.
 * Writes NOTHING to the command buffer or to any context field. Takes snapshots at: (0) context current, (1) after glClear
 * (unflushed), (2) after glFinish, (3) after a second clear (unflushed). Each snapshot prints the four fields and the words at
 * base+0x00..0x3c and at cursor-0x20..cursor+0x10, with deltas, so the driver's header/cursor convention can be read off directly.
 * Usage: stage3i-state-dump LOGFILE [CAPTUREFILE]   (capture = 5 big-endian dwords: link_off cursor_off limit_off magic 0, then buffer bytes)
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static FILE *g_log;
static void LOG(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); fcntl(fileno(g_log), F_FULLFSYNC, 0); }
}
static const char *g_cap;   /* optional: binary capture of base..cursor+16 at snapshot "1-" (read-only copy) */
static void snap(AGLContext ctx, const char *tag) {
    unsigned char *c = (unsigned char *)ctx;
    uint32_t link = *(uint32_t *)(c + 0x17d8), cur = *(uint32_t *)(c + 0x17dc), lim = *(uint32_t *)(c + 0x17e0), base = *(uint32_t *)(c + 0x17e4);
    LOG("[snap %s] link=%08x cursor=%08x limit=%08x base=%08x | link-base=0x%x cursor-base=0x%x limit-base=0x%x\n",
        tag, link, cur, lim, base, link - base, cur - base, lim - base);
    if (!base || cur < base || cur - base > 0x40000) { LOG("[snap %s] implausible, no buffer dump\n", tag); return; }
    if (g_cap && tag[0] == '1') {
        FILE *f = fopen(g_cap, "wb");
        if (f) { uint32_t hdr[5] = {link - base, cur - base, lim - base, 0x50414330u, 0}; fwrite(hdr, 4, 5, f); fwrite((void *)base, 1, (cur - base) + 16, f); fflush(f); fcntl(fileno(f), F_FULLFSYNC, 0); fclose(f); LOG("  capture written: %s (%u bytes of buffer)\n", g_cap, (unsigned)((cur - base) + 16)); }
    }
    volatile uint32_t *b = (volatile uint32_t *)base; int i;
    LOG("  base+0x00:"); for (i = 0; i < 16; i++) LOG(" %08x", b[i]); LOG("\n");
    uint32_t co = (cur - base) / 4; int from = (int)co - 8; if (from < 0) from = 0;
    LOG("  cursor-0x20..+0x10 (from +0x%x):", from * 4); for (i = from; i < (int)co + 4 && i < from + 16; i++) LOG(" %08x", b[i]); LOG("\n");
    if (link >= base && link < base + 0x40000) { uint32_t lo = (link - base) / 4; LOG("  at link target (+0x%x): %08x %08x %08x\n", lo * 4, b[lo], b[lo + 1], b[lo + 2]); }
}
int main(int argc, char **argv) {
    g_log = argc > 1 ? fopen(argv[1], "a") : NULL; g_cap = argc > 2 ? argv[2] : NULL;
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    LOG("[3i] connect field = 0x%x\n", *(uint32_t *)((unsigned char *)ctx + 0x1604));
    snap(ctx, "0-current");
    glViewport(0, 0, 4, 4); glClearColor(0.25f, 0.5f, 0.75f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
    snap(ctx, "1-after-clear-unflushed");
    glFinish();
    snap(ctx, "2-after-finish");
    unsigned char px[4]; glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    LOG("[3i] pixel after clear: %d,%d,%d,%d\n", px[0], px[1], px[2], px[3]);
    glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    snap(ctx, "3-after-2nd-clear-unflushed");
    glFinish();
    snap(ctx, "4-after-2nd-finish");
    aglSetCurrentContext(NULL); aglDestroyContext(ctx);
    return 0;
}
