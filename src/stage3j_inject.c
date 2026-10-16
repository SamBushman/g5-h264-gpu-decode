/* PROMO4 Stage 3j: inject gate-cleared PM4 into a LIVE AGLContext at the CORRECT cursor, then verify with real pixels.
 *
 * Field map (reveng/stage3g-cursor-field-misidentified.md, confirmed by the read-only stage3i probe on this machine):
 *   ctx+0x17d8 chain-link = address of the PENDING record header (length still 0),  ctx+0x17dc write cursor,
 *   ctx+0x17e0 limit,  ctx+0x17e4 buffer base.   We only ever WRITE ctx+0x17dc (never the chain link - that was the stage 3g bug).
 *
 * Modes:  capture  - read-only: run the workload up to the injection point and save the live buffer (for tools/promo4_sim/inject_sim.py)
 *         inject   - same, then append PM4.hex words at the cursor and advance ctx+0x17dc by exactly their count.
 * Preconditions checked before ANY write (abort on doubt): link,cursor inside [base+0x20, limit); cursor-link in 8..0x400 bytes; pending
 * header word == 0x2f000000 (the record kind whose body the stock kernel passes through - the kind simulated in inject_sim.py); room for
 * the payload + 16 dwords slack; payload non-empty. After the write: draw, glFinish, read a pixel; print baseline/after colours.
 *
 * Usage: stage3j-inject capture|inject PM4.hex LOG CAPFILE
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static FILE *g_log;
static void LOG(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); fcntl(fileno(g_log), F_FULLFSYNC, 0); }
}
static GLhandleARB compile(GLenum t, const char *s) {
    GLhandleARB h = glCreateShaderObjectARB(t); glShaderSourceARB(h, 1, &s, NULL); glCompileShaderARB(h);
    GLint ok = 0; glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) { LOG("shader compile failed\n"); exit(1); }
    return h;
}
static GLhandleARB linkp(const char *vs, const char *fs) {
    GLhandleARB p = glCreateProgramObjectARB();
    glAttachObjectARB(p, compile(GL_VERTEX_SHADER_ARB, vs)); glAttachObjectARB(p, compile(GL_FRAGMENT_SHADER_ARB, fs));
    glLinkProgramARB(p);
    GLint ok = 0; glGetObjectParameterivARB(p, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) { LOG("link failed\n"); exit(1); }
    return p;
}
static void quad(void) { glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4); glEnd(); }
static void rd(unsigned char o[4]) { glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, o); }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s capture|inject PM4.hex LOG CAPFILE\n", argv[0]); return 2; }
    int doInject = !strcmp(argv[1], "inject");
    g_log = fopen(argv[3], "a");
    uint32_t pm4[512]; int n = 0;
    { FILE *f = fopen(argv[2], "r"); char tok[64]; int ch, t = 0;
      if (!f) { LOG("cannot open %s\n", argv[2]); return 2; }
      while ((ch = fgetc(f)) != EOF) {
          if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || ch == 'x' || ch == 'X') { if (t < 60) tok[t++] = ch; }
          else if (t) { tok[t] = 0; if (n < 512) pm4[n++] = (uint32_t)strtoul(tok, NULL, 0); t = 0; }
      }
      if (t) { tok[t] = 0; if (n < 512) pm4[n++] = (uint32_t)strtoul(tok, NULL, 0); }
      fclose(f); }
    LOG("[3j] mode=%s, %d PM4 dwords from %s\n", argv[1], n, argv[2]);
    if (doInject && n == 0) { LOG("[3j] empty payload, abort\n"); return 2; }

    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glViewport(0, 0, 4, 4); glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    GLhandleARB prog = linkp("void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}", "void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}");
    glUseProgramObjectARB(prog);

    unsigned char base0[4] = {0}, after[4] = {0};
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT); quad(); glFinish(); rd(base0);
    LOG("[3j] baseline (GLSL red): %d,%d,%d,%d\n", base0[0], base0[1], base0[2], base0[3]);

    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);          /* leaves a pending record with a small body, unflushed */
    unsigned char *c = (unsigned char *)ctx;
    uint32_t link = *(uint32_t *)(c + 0x17d8), cur = *(uint32_t *)(c + 0x17dc), lim = *(uint32_t *)(c + 0x17e0), base = *(uint32_t *)(c + 0x17e4);
    LOG("[3j] link=%08x cursor=%08x limit=%08x base=%08x (link-base=0x%x cursor-base=0x%x limit-base=0x%x)\n", link, cur, lim, base, link - base, cur - base, lim - base);
    volatile uint32_t *b = (volatile uint32_t *)base;
    int ok = 1;
    if (!base || link < base + 0x20 || cur <= link || cur >= lim || lim - base > 0x100000) { LOG("[3j] ABORT: fields implausible\n"); ok = 0; }
    else if ((link & 3) || (cur & 3)) { LOG("[3j] ABORT: unaligned\n"); ok = 0; }
    else if (cur - link < 8 || cur - link > 0x400) { LOG("[3j] ABORT: pending-record body size %u bytes outside 8..0x400\n", cur - link); ok = 0; }
    else if (b[(link - base) / 4] != 0x2f000000u) { LOG("[3j] ABORT: pending header is %08x, only 0x2f000000 is simulated/allowed\n", b[(link - base) / 4]); ok = 0; }
    else if (doInject && cur + 4u * (n + 16) > lim) { LOG("[3j] ABORT: no room for %d dwords\n", n); ok = 0; }
    if (ok) {
        FILE *f = fopen(argv[4], "wb");
        if (f) { uint32_t h[5] = {link - base, cur - base, lim - base, 0x50414330u, 0}; fwrite(h, 4, 5, f); fwrite((void *)base, 1, (cur - base) + 16, f); fflush(f); fcntl(fileno(f), F_FULLFSYNC, 0); fclose(f); LOG("[3j] capture written %s\n", argv[4]); }
    }
    if (ok && doInject) {
        uint32_t *p = (uint32_t *)cur; int i;
        for (i = 0; i < n; i++) p[i] = pm4[i];
        __asm__ __volatile__("sync" ::: "memory");
        *(uint32_t *)(c + 0x17dc) = cur + 4u * n;                    /* ONLY the cursor field; the chain link is never touched */
        LOG("[3j] injected %d dwords at +0x%x, cursor advanced to +0x%x\n", n, cur - base, cur + 4u * n - base);
    }
    if (ok && !doInject) LOG("[3j] capture mode: nothing written to the command buffer\n");
    quad();
    if (!doInject) {          /* second read-only capture: the buffer AFTER the draw is queued, before the flush - for inject_sim2.py (insertion with the driver's later records) */
        uint32_t l2 = *(uint32_t *)(c + 0x17d8), c2 = *(uint32_t *)(c + 0x17dc), lm2 = *(uint32_t *)(c + 0x17e0), b2 = *(uint32_t *)(c + 0x17e4);
        char nm[512]; snprintf(nm, sizeof nm, "%s.post", argv[4]);
        FILE *f = (b2 == base && c2 > cur && c2 - b2 < 0x40000) ? fopen(nm, "wb") : NULL;
        if (f) { uint32_t h[5] = {l2 - b2, c2 - b2, lm2 - b2, 0x50414330u, 0}; fwrite(h, 4, 5, f); fwrite((void *)b2, 1, (c2 - b2) + 16, f); fflush(f); fcntl(fileno(f), F_FULLFSYNC, 0); fclose(f); LOG("[3j] post-draw capture written %s (link=+0x%x cursor=+0x%x)\n", nm, l2 - b2, c2 - b2); }
        else LOG("[3j] post-draw capture skipped (buffer changed or implausible)\n");
    }
    glFinish(); rd(after);
    LOG("[3j] after: %d,%d,%d,%d  (baseline was %d,%d,%d,%d)\n", after[0], after[1], after[2], after[3], base0[0], base0[1], base0[2], base0[3]);
    /* the context must still work normally afterwards */
    unsigned char again[4]; glClearColor(0.0f, 1.0f, 0.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT); glFinish(); rd(again);
    LOG("[3j] post-check clear green: %d,%d,%d,%d\n", again[0], again[1], again[2], again[3]);
    aglSetCurrentContext(NULL); aglDestroyContext(ctx);
    LOG("[3j] done\n");
    return ok ? 0 : 1;
}
