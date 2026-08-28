/* Item 9 follow-up (2026-08-28): the threading attempt found no benefit
 * because almost every flush needs its own result immediately - overlapping
 * ACROSS flushes (via OS threads) didn't pay off. This probe checks a
 * narrower, lower-risk idea instead: WITHIN one flush, the real pipeline
 * currently does three-plus independent synchronous round trips back to
 * back - MC singlepass (draw+glFinish+glReadPixels), MC diag (same, per
 * chunk, its own two-pass FBO internally but its FINAL output still lands
 * back on the default pbuffer buffer for readback - confirmed by reading
 * gpu_live_decode_test.c, dispatch_diag_group unbinds its FBO with
 * glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0) right before its own final
 * glReadPixels), IDCT batch (same, default buffer too) - each ending its
 * own glFinish() before the next one's draw is even issued.
 *
 * Real architectural wrinkle found BEFORE writing this probe (matters for
 * whether "collapse 3 stalls into ~1" is even achievable as stated): since
 * singlepass, diag's final stage, and IDCT ALL render their real output to
 * the SAME shared default pbuffer color buffer, their readbacks genuinely
 * cannot be deferred past each other's draws without clobbering - true
 * pipelining needs each dispatch to target its OWN separate FBO-attached
 * texture instead (reusing the FBO pattern diag's own stage1 already uses
 * internally). This probe tests the REALISTIC version of the idea (three
 * separate FBO targets) rather than a version that would silently corrupt
 * data in the real pipeline.
 *
 * Variants compared, three synthetic draw+readback round trips sized to
 * roughly match the real pipeline's three dispatch shapes (MC singlepass
 * ~200x16, MC diag chunk ~256x21, IDCT batch ~960x24 - a moderate chunk,
 * not the full 3840 width), repeated NREP times:
 *   A. baseline (today's pattern): draw1,finish,read1, draw2,finish,read2,
 *      draw3,finish,read3 - fully serial, single shared target (matches
 *      what the real pipeline does today).
 *   B. same shared target, but drop the intermediate glFinish() and let
 *      glReadPixels's own implicit sync do the work - isolates whether
 *      glFinish() itself has meaningful overhead beyond that.
 *   D. TRUE pipelining: three SEPARATE FBO-attached targets, draw1,draw2,
 *      draw3 issued back to back (no finish, no read yet - safe now,
 *      nothing gets clobbered), THEN read1,read2,read3 - by the time read1
 *      blocks (if it even needs to), draw2/draw3 are already queued.
 *   C. same three separate targets, but with explicit GL_APPLE_fence
 *      (glSetFenceAPPLE after each draw, glFinishFenceAPPLE right before
 *      each corresponding read) instead of relying on glReadPixels's own
 *      implicit sync - in case this driver handles it differently.
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>

static void checkgl(const char *w) { GLenum e = glGetError(); if (e) fprintf(stderr, "GL err %s: 0x%lx\n", w, (unsigned long)e); }
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
/* Modest per-fragment work - real production shaders here (fs_mc_batch_var,
 * fs_idct_batch) do real but not extreme math (a handful of texture fetches
 * + arithmetic), not the deliberately-heavy trig loop finish_probe.c used
 * to isolate busy-wait behavior. This probe is about call/sync overhead,
 * not GPU compute time, so keep it representative-light. */
static const char *fs_light =
"uniform sampler2DRect tex;\n"
"void main() {\n"
"  vec4 c = texture2DRect(tex, gl_FragCoord.xy);\n"
"  gl_FragColor = c * 0.5 + vec4(0.1, 0.1, 0.1, 0.0);\n"
"}\n";

static double wall_ms(struct timeval *a, struct timeval *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_usec - a->tv_usec) / 1000.0;
}
static double cpu_ms(struct rusage *a, struct rusage *b) {
    double au = a->ru_utime.tv_sec * 1000.0 + a->ru_utime.tv_usec / 1000.0;
    double as_ = a->ru_stime.tv_sec * 1000.0 + a->ru_stime.tv_usec / 1000.0;
    double bu = b->ru_utime.tv_sec * 1000.0 + b->ru_utime.tv_usec / 1000.0;
    double bs = b->ru_stime.tv_sec * 1000.0 + b->ru_stime.tv_usec / 1000.0;
    return (bu - au) + (bs - as_);
}

/* Roughly matching the real pipeline's three dispatch shapes. */
static const int W[3] = {200, 256, 960};
static const int H[3] = {16, 21, 24};

static GLuint srctex[3];
static GLhandleARB prog;
static GLint loc_tex;
static GLuint dstTex[3], dstFbo[3]; /* variant D/C's separate targets */

static void do_draw(int i) {
    glViewport(0, 0, W[i], H[i]);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W[i], 0, H[i], -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, srctex[i]);
    glUniform1iARB(loc_tex, 0);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(W[i], 0); glVertex2f(W[i], H[i]); glVertex2f(0, H[i]);
    glEnd();
}

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8,
                        AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(1024, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    prog = linkp(vs_plain, fs_light);
    glUseProgramObjectARB(prog);
    loc_tex = glGetUniformLocationARB(prog, "tex");

    glGenTextures(3, srctex);
    for (int i = 0; i < 3; i++) {
        unsigned char *data = malloc((size_t)W[i] * H[i] * 4);
        for (int p = 0; p < W[i] * H[i] * 4; p++) data[p] = (unsigned char)(p * 37 % 256);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, srctex[i]);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, W[i], H[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        free(data);
    }

    /* Separate FBO-attached destination textures for D/C - each dispatch
     * "slot" gets its own render target, matching what a real restructure
     * would need (see this file's top comment). */
    glGenTextures(3, dstTex);
    glGenFramebuffersEXT(3, dstFbo);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, dstTex[i]);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, W[i], H[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, dstFbo[i]);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_RECTANGLE_ARB, dstTex[i], 0);
        GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
        if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
            fprintf(stderr, "FBO %d incomplete: 0x%x\n", i, status);
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    checkgl("setup");

    static unsigned char pixels[960 * 24 * 4];
    const int NREP = 100;

    /* --- A: baseline, today's pattern (shared default buffer, serial) --- */
    double a_wall = 0, a_cpu = 0;
    for (int rep = 0; rep < NREP; rep++) {
        struct timeval w0, w1; struct rusage r0, r1;
        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        for (int i = 0; i < 3; i++) {
            do_draw(i);
            glFinish();
            glReadPixels(0, 0, W[i], H[i], GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        a_wall += wall_ms(&w0, &w1); a_cpu += cpu_ms(&r0, &r1);
    }
    checkgl("A baseline");

    /* --- B: shared default buffer, drop glFinish, rely on glReadPixels --- */
    double b_wall = 0, b_cpu = 0;
    for (int rep = 0; rep < NREP; rep++) {
        struct timeval w0, w1; struct rusage r0, r1;
        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        for (int i = 0; i < 3; i++) {
            do_draw(i);
            glReadPixels(0, 0, W[i], H[i], GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        b_wall += wall_ms(&w0, &w1); b_cpu += cpu_ms(&r0, &r1);
    }
    checkgl("B pipelined-no-fence, shared target");

    /* --- D: separate targets, true pipelining (all draws, then all reads) --- */
    double d_wall = 0, d_cpu = 0;
    for (int rep = 0; rep < NREP; rep++) {
        struct timeval w0, w1; struct rusage r0, r1;
        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        for (int i = 0; i < 3; i++) {
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, dstFbo[i]);
            do_draw(i);
        }
        for (int i = 0; i < 3; i++) {
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, dstFbo[i]);
            glReadPixels(0, 0, W[i], H[i], GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        d_wall += wall_ms(&w0, &w1); d_cpu += cpu_ms(&r0, &r1);
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    checkgl("D true-pipelining, separate targets");

    /* --- C: separate targets, explicit APPLE_fence --- */
    GLuint fences[3];
    glGenFencesAPPLE(3, fences);
    double c_wall = 0, c_cpu = 0;
    for (int rep = 0; rep < NREP; rep++) {
        struct timeval w0, w1; struct rusage r0, r1;
        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        for (int i = 0; i < 3; i++) {
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, dstFbo[i]);
            do_draw(i);
            glSetFenceAPPLE(fences[i]);
        }
        for (int i = 0; i < 3; i++) {
            glFinishFenceAPPLE(fences[i]);
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, dstFbo[i]);
            glReadPixels(0, 0, W[i], H[i], GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        c_wall += wall_ms(&w0, &w1); c_cpu += cpu_ms(&r0, &r1);
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    checkgl("C pipelined-fence, separate targets");
    glDeleteFencesAPPLE(3, fences);

    printf("Fence/pipelining probe, %d reps of 3 draw+readback round trips (%dx%d, %dx%d, %dx%d):\n\n",
           NREP, W[0], H[0], W[1], H[1], W[2], H[2]);
    printf("A. baseline serial, shared target:        wall=%8.1fms cpu=%8.1fms (%.1f%% cpu/wall) [%.3fms/set]\n",
           a_wall, a_cpu, a_wall > 0 ? 100.0 * a_cpu / a_wall : 0.0, a_wall / NREP);
    printf("B. no-finish, shared target:               wall=%8.1fms cpu=%8.1fms (%.1f%% cpu/wall) [%.3fms/set]\n",
           b_wall, b_cpu, b_wall > 0 ? 100.0 * b_cpu / b_wall : 0.0, b_wall / NREP);
    printf("D. true pipelining, separate targets:      wall=%8.1fms cpu=%8.1fms (%.1f%% cpu/wall) [%.3fms/set]\n",
           d_wall, d_cpu, d_wall > 0 ? 100.0 * d_cpu / d_wall : 0.0, d_wall / NREP);
    printf("C. APPLE_fence, separate targets:           wall=%8.1fms cpu=%8.1fms (%.1f%% cpu/wall) [%.3fms/set]\n",
           c_wall, c_cpu, c_wall > 0 ? 100.0 * c_cpu / c_wall : 0.0, c_wall / NREP);
    printf("\nB vs A: %.2fx wall, %.2fx cpu\n", a_wall / b_wall, a_cpu / b_cpu);
    printf("D vs A: %.2fx wall, %.2fx cpu\n", a_wall / d_wall, a_cpu / d_cpu);
    printf("C vs A: %.2fx wall, %.2fx cpu\n", a_wall / c_wall, a_cpu / c_cpu);
    printf("C vs D: %.2fx wall, %.2fx cpu (does explicit fence beat implicit glReadPixels sync?)\n",
           d_wall / c_wall, d_cpu / c_cpu);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
