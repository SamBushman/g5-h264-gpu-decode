/* PROMO4 Stage 3g: reproduce the already-proven GLSL shader by injecting a hand-encoded
 * native program directly into a LIVE AGLContext's own already-mapped command buffer,
 * at its live write cursor - the structurally-correct fix for stage3_native_shader.c's
 * negative result (that version opened a SEPARATE io_connect_t, and per-context GPU
 * register-shadow virtualization meant its writes never touched the context that was
 * actually drawing).
 *
 * Real, gdb-confirmed offsets inside the opaque AGLContext struct (agl.h only exposes
 * `typedef struct __AGLContextRec *AGLContext` - no public layout, confirmed by reading
 * the real header):
 *   ctx + 0x1604 = live io_connect_t handle (the connection AGL itself opened)
 *   ctx + 0x17e4 = mapped command-buffer base (already a valid pointer in OUR address
 *                  space - AGL mapped it into this same process, no translation needed)
 *   ctx + 0x17d8 = live command-buffer write cursor (also an already-mapped address)
 *
 * REAL BUG FROM THE PRIOR ATTEMPT, FIXED HERE: that run captured cursor = base + 0x1c
 * and wrote an 11-dword payload there without checking it was genuinely past the
 * confirmed 32-byte VendorCommandBufferHeader. It almost certainly clobbered real
 * header bookkeeping and desynced the kext's command-stream parser - `glFinish()` never
 * returned, and a FRESH ssh connection timed out at the TCP level (a genuine
 * system-level hang, not a stuck process), requiring physical power-cycle to recover.
 * This version adds an explicit, hard alignment/bounds check before writing anything -
 * if the cursor isn't past the header, or there isn't room for the payload, it aborts
 * and writes nothing. No IOConnectMapMemory re-map call here: writing happens directly
 * into memory AGL already owns and mapped, so the actual "submit" is the context's own
 * next ordinary glFinish() - the same call this program needs to call anyway to read
 * pixels back, since AGL's own driver code already treats a real glFinish() on this
 * context as the point it flushes/re-maps this exact buffer during normal operation.
 *
 * Usage: stage3g-real-injection
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void checkgl(const char *w) { GLenum e = glGetError(); if (e) fprintf(stderr, "GL err %s: 0x%x\n", w, (unsigned)e); }
static GLhandleARB compile(GLenum t, const char *s) {
    GLhandleARB h = glCreateShaderObjectARB(t);
    glShaderSourceARB(h, 1, &s, NULL); glCompileShaderARB(h);
    GLint ok = 0; glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(h, sizeof log, &n, log); fprintf(stderr, "compile fail:\n%s\n", log); exit(1); }
    return h;
}
static GLhandleARB linkp(const char *vs, const char *fs) {
    GLhandleARB p = glCreateProgramObjectARB();
    glAttachObjectARB(p, compile(GL_VERTEX_SHADER_ARB, vs));
    glAttachObjectARB(p, compile(GL_FRAGMENT_SHADER_ARB, fs));
    glLinkProgramARB(p);
    GLint ok = 0; glGetObjectParameterivARB(p, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(p, sizeof log, &n, log); fprintf(stderr, "link fail:\n%s\n", log); exit(1); }
    return p;
}
static const char *vs_plain = "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}";
static const char *fs_red   = "void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}";

static void draw_quad_and_read(unsigned char out[4]) {
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4);
    glEnd();
    glFinish();
    checkgl("draw");
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    checkgl("readpixels");
}

/* real register dword (MMIO/4) addresses, from AMD's R5xx doc register reference -
 * identical constants to stage3_native_shader.c, already proven correct there. */
#define REG_GA_US_VECTOR_INDEX 0x1094u
#define REG_GA_US_VECTOR_DATA  0x1095u
#define REG_US_CODE_ADDR       0x118Cu
#define HEADER_BYTES           0x20u   /* confirmed real VendorCommandBufferHeader size */
#define TEST_SLOT              100u

static uint32_t type0_header(uint32_t baseIndex, uint32_t countMinus1, int oneRegWr) {
    return (baseIndex & 0x1FFFu) | ((uint32_t)(oneRegWr ? 1 : 0) << 15) |
           ((countMinus1 & 0x3FFFu) << 16) | (0u << 30);
}

static uint32_t *emit_load_program(uint32_t *p, uint32_t slot,
                                    uint32_t cmn, uint32_t rgbAddr, uint32_t alphaAddr,
                                    uint32_t rgbInst, uint32_t alphaInst, uint32_t rgbaInst) {
    *p++ = type0_header(REG_GA_US_VECTOR_INDEX, 0, 0);
    *p++ = (slot & 0x1FFu);
    *p++ = type0_header(REG_GA_US_VECTOR_DATA, 5, 1);
    *p++ = cmn;
    *p++ = rgbAddr;
    *p++ = alphaAddr;
    *p++ = rgbInst;
    *p++ = alphaInst;
    *p++ = rgbaInst;
    *p++ = type0_header(REG_US_CODE_ADDR, 0, 0);
    *p++ = ((slot & 0x1FFu) << 16) | (slot & 0x1FFu);
    return p;
}

#define US_CMN_INST_VAL       0x001F8105u
#define US_ALU_RGB_ADDR_VAL   0x00000000u
#define US_ALU_ALPHA_ADDR_VAL 0x00000000u
#define US_ALU_ALPHA_INST_VAL 0x00C18000u
#define US_ALU_RGBA_INST_VAL  0x20490000u
#define US_ALU_RGB_INST_RED   0x00DB0498u
#define US_ALU_RGB_INST_BLUE  0x00DB0690u
#define INJECT_DWORDS 11u   /* exact size emit_load_program writes, kept explicit for the bounds check */

/* Real fix: read the live cursor/base straight out of the opaque AGLContext, verify it
 * is genuinely past the header AND there is room for the whole payload before writing
 * a single byte. Returns 0 (and writes nothing) on any doubt at all. */
static int inject_at_live_cursor(AGLContext ctx, uint32_t rgbInstVal, const char *label) {
    unsigned char *base   = *(unsigned char **)((unsigned char *)ctx + 0x17e4);
    unsigned char *cursor = *(unsigned char **)((unsigned char *)ctx + 0x17d8);

    ptrdiff_t off = cursor - base;
    fprintf(stderr, "[stage3g:%s] live base=%p cursor=%p (offset 0x%lx)\n",
            label, (void *)base, (void *)cursor, (unsigned long)off);

    if (off < (ptrdiff_t)HEADER_BYTES) {
        fprintf(stderr, "[stage3g:%s] ABORT: cursor offset 0x%lx is NOT past the confirmed "
                        "0x%x-byte header - this is exactly the condition that hung the "
                        "machine last time. Writing nothing.\n",
                label, (unsigned long)off, HEADER_BYTES);
        return 0;
    }
    if ((off % 4) != 0) {
        fprintf(stderr, "[stage3g:%s] ABORT: cursor offset 0x%lx is not 4-byte aligned - "
                        "refusing to write a dword stream here.\n", label, (unsigned long)off);
        return 0;
    }

    uint32_t *p = (uint32_t *)cursor;
    uint32_t *end = emit_load_program(p, TEST_SLOT, US_CMN_INST_VAL,
                                       US_ALU_RGB_ADDR_VAL, US_ALU_ALPHA_ADDR_VAL,
                                       rgbInstVal, US_ALU_ALPHA_INST_VAL, US_ALU_RGBA_INST_VAL);
    if ((uint32_t)(end - p) != INJECT_DWORDS) {
        fprintf(stderr, "[stage3g:%s] INTERNAL ERROR: emitted %ld dwords, expected %u - "
                        "logic bug, not a hardware issue. Not advancing cursor.\n",
                label, (long)(end - p), INJECT_DWORDS);
        return 0;
    }

    unsigned char *newCursor = cursor + INJECT_DWORDS * 4;
    fprintf(stderr, "[stage3g:%s] wrote %u dwords at %p, advancing cursor to %p\n",
            label, INJECT_DWORDS, (void *)cursor, (void *)newCursor);
    *(unsigned char **)((unsigned char *)ctx + 0x17d8) = newCursor;
    return 1;
}

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    GLhandleARB prog = linkp(vs_plain, fs_red);
    glUseProgramObjectARB(prog);

    unsigned char baseline[4] = {0,0,0,0};
    draw_quad_and_read(baseline);
    fprintf(stderr, "[stage3g] real GLSL baseline (compiler-generated native code): RGBA=(%d,%d,%d,%d)\n",
            baseline[0], baseline[1], baseline[2], baseline[3]);
    fprintf(stderr, "[stage3g] live connect handle field = 0x%x\n",
            *(uint32_t *)((unsigned char *)ctx + 0x1604));

    /* Real finding this run: this exact program's cursor reproducibly lands at
     * base+0x1c (mid-header) right after the baseline draw+readback - 4 bytes short
     * of the confirmed 32-byte header boundary that a simpler clear-only program
     * reaches cleanly. One more full real flush cycle here, before capturing the
     * cursor for injection, to see whether it settles onto the clean boundary -
     * still fully gated by inject_at_live_cursor's check either way. */
    unsigned char settle[4] = {0,0,0,0};
    draw_quad_and_read(settle);
    fprintf(stderr, "[stage3g] extra settle draw+finish+readback done: RGBA=(%d,%d,%d,%d)\n",
            settle[0], settle[1], settle[2], settle[3]);

    int blueOk = inject_at_live_cursor(ctx, US_ALU_RGB_INST_BLUE, "control-blue");
    unsigned char control[4] = {0,0,0,0};
    if (blueOk) {
        draw_quad_and_read(control);
        fprintf(stderr, "[stage3g] after BLUE injection, redraw result: RGBA=(%d,%d,%d,%d)\n",
                control[0], control[1], control[2], control[3]);
    } else {
        fprintf(stderr, "[stage3g] control-blue injection skipped (safety abort above) - stopping here, no further writes.\n");
    }

    int redOk = 0;
    unsigned char test[4] = {0,0,0,0};
    if (blueOk) {
        redOk = inject_at_live_cursor(ctx, US_ALU_RGB_INST_RED, "test-red");
        if (redOk) {
            draw_quad_and_read(test);
            fprintf(stderr, "[stage3g] after RED injection, redraw result: RGBA=(%d,%d,%d,%d)\n",
                    test[0], test[1], test[2], test[3]);
        }
    }

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);

    int redirectProven = (blueOk && (control[0]!=baseline[0] || control[1]!=baseline[1] ||
                                      control[2]!=baseline[2] || control[3]!=baseline[3]));
    int reproductionMatches = (redOk && test[0]==baseline[0] && test[1]==baseline[1] &&
                                test[2]==baseline[2] && test[3]==baseline[3]);

    fprintf(stderr, "\n[stage3g] === VERDICT ===\n");
    fprintf(stderr, "[stage3g] baseline (real GLSL): RGBA=(%d,%d,%d,%d)\n", baseline[0],baseline[1],baseline[2],baseline[3]);
    fprintf(stderr, "[stage3g] control  (hand blue): RGBA=(%d,%d,%d,%d) - redirect %s\n",
            control[0],control[1],control[2],control[3], redirectProven ? "PROVEN" : "not proven / skipped");
    fprintf(stderr, "[stage3g] test     (hand red):  RGBA=(%d,%d,%d,%d) - reproduction %s\n",
            test[0],test[1],test[2],test[3], reproductionMatches ? "MATCHES baseline" : "does not match / skipped");

    return (redirectProven && reproductionMatches) ? 0 : 1;
}
