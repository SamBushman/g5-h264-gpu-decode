/* PROMO4 Stage 3h: pure read-only diagnostic. No writes to the command buffer, no
 * IOKit calls of our own - just dump raw bytes around the live "base"/"cursor"
 * pointers found at AGLContext+0x17e4/+0x17d8, to check whether the real 32-byte
 * VendorCommandBufferHeader (kernel-side confirmed: init_command_buffer_header sets
 * +0x10=capacity, +0x14=tag, +0x18=per-context tag from ctx+0x7c, +0x1c=1,
 * +0x20=0x1000000 marker) actually starts 4 bytes BEFORE what was captured as "base" -
 * i.e. whether the gdb-found base pointer is off by one dword, which would fully
 * explain why the live cursor reproducibly sits at base+0x1c instead of the assumed
 * base+0x20.
 *
 * Usage: stage3h-header-dump
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

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

static void hexdump(const unsigned char *p, long startOffset, int nWords) {
    for (int i = 0; i < nWords; i++) {
        long off = startOffset + i * 4;
        uint32_t v = *(uint32_t *)(p + i * 4);
        fprintf(stderr, "  base%+ld (0x%02lx): 0x%08x\n", off, off, v);
    }
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

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4);
    glEnd();
    glFinish();
    checkgl("draw");
    unsigned char px[4];
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    checkgl("readpixels");
    fprintf(stderr, "[stage3h] shader-path draw+readback: RGBA=(%d,%d,%d,%d)\n", px[0], px[1], px[2], px[3]);

    unsigned char *base   = *(unsigned char **)((unsigned char *)ctx + 0x17e4);
    unsigned char *cursor = *(unsigned char **)((unsigned char *)ctx + 0x17d8);
    uint32_t perCtxTag    = *(uint32_t *)((unsigned char *)ctx + 0x7c);

    fprintf(stderr, "[stage3h] ctx=%p base=%p cursor=%p (cursor-base=0x%lx)\n",
            (void *)ctx, (void *)base, (void *)cursor, (unsigned long)(cursor - base));
    fprintf(stderr, "[stage3h] ctx+0x7c (candidate per-context tag) = 0x%08x\n", perCtxTag);

    fprintf(stderr, "[stage3h] raw dwords from base-8 through base+0x28:\n");
    hexdump(base - 8, -8, (0x28 + 8) / 4 + 1);

    fprintf(stderr, "\n[stage3h] checking real header field predictions per init_command_buffer_header:\n");
    fprintf(stderr, "  if header truly starts at base+0   : +0x1c should be 1        -> got 0x%08x\n", *(uint32_t *)(base + 0x1c));
    fprintf(stderr, "  if header truly starts at base+0   : +0x18 should be tag 0x%08x -> got 0x%08x\n", perCtxTag, *(uint32_t *)(base + 0x18));
    fprintf(stderr, "  if header truly starts at base-4   : +0x18 (=base+0x14) should be 1        -> got 0x%08x\n", *(uint32_t *)(base - 4 + 0x18));
    fprintf(stderr, "  if header truly starts at base-4   : +0x14 (=base+0x10) should be tag 0x%08x -> got 0x%08x\n", perCtxTag, *(uint32_t *)(base - 4 + 0x14));

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
