/*
 * readback-luminance-probe: tests whether glReadPixels(..., GL_LUMINANCE,
 * GL_UNSIGNED_BYTE, ...) against a real rendered framebuffer returns the
 * true R value on this driver, and whether it's actually faster than the
 * GL_RGBA readback every hot dispatch site currently uses.
 *
 * Real motivation: dispatch_singlepass_group and dispatch_diag_group's
 * fragment shaders both write `vec4(result/255.0, 0.0, 0.0, 1.0)` - only R
 * carries real data, G/B are always 0, A is always 1. Reading back
 * GL_RGBA/GL_UNSIGNED_BYTE transfers 4x more data than needed and forces
 * the CPU-side unpack loop into a strided per-pixel gather
 * (`pixels[idx*4]`) instead of a near-linear copy. If GL_LUMINANCE
 * readback correctly returns L=R here (not some other GL_LUMINANCE
 * conversion formula this project hasn't verified), this could cut both
 * readback bandwidth and unpack cost - readback+unpack is currently the
 * single largest cost category in every profiled dispatch site.
 *
 * Real risk this project has been burned by before (GL_LUMINANCE_FLOAT32_
 * ATI's negative-value bug): don't assume a format works just because it's
 * spec-legal - this driver has real, specific quirks. Verify directly.
 */

#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

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

/* Exact encoding pattern fs_mc_batch_var/fs_diag_singlepass_batch use:
 * one value 0..255 per output pixel, R only, G=B=0, A=1. Uses a small
 * lookup texture (matching production's blockInfoTex-driven pattern) so
 * the rendered value varies per output column, covering the full 0..255
 * range across the test width - not just one constant value. */
static const char *fs_encode =
"uniform sampler2DRect valTex;\n"
"void main(){\n"
"  float v = texture2DRect(valTex, floor(gl_FragCoord.xy)+vec2(0.5,0.5)).r * 255.0;\n"
"  gl_FragColor = vec4(v/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

static double wall_ms(struct timeval *a, struct timeval *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_usec - a->tv_usec) / 1000.0;
}

int main(void) {
    const int W = 2048, H = 16; /* realistic production batch scale */

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(W, H, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    /* Value lookup texture: covers 0..255 repeatedly across the width, so
     * every possible byte value gets tested many times. */
    float *valdata = (float*)malloc(sizeof(float) * W);
    for (int x = 0; x < W; x++) valdata[x] = (float)(x % 256) / 255.0f;
    GLuint valTex; glGenTextures(1,&valTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,valTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_LUMINANCE8,W,1,0,GL_LUMINANCE,GL_UNSIGNED_BYTE, NULL);
    /* upload real byte data via UNSIGNED_BYTE path matching reftex's own convention */
    unsigned char *valbytes = (unsigned char*)malloc(W);
    for (int x = 0; x < W; x++) valbytes[x] = (unsigned char)(x % 256);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_LUMINANCE8,W,1,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,valbytes);
    checkgl("valTex upload");

    GLhandleARB prog = linkp(vs_plain, fs_encode);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, valTex);
    glUniform1iARB(glGetUniformLocationARB(prog,"valTex"),0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W,0);glVertex2f(W,H);glVertex2f(0,H); glEnd();
    glFinish(); checkgl("draw");

    /* ---- Correctness: read back the SAME rendered content via both
     * formats, compare against the true intended value. ---- */
    unsigned char *rgba = (unsigned char*)malloc((size_t)W*H*4);
    unsigned char *lum = (unsigned char*)malloc((size_t)W*H);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    checkgl("rgba readback");
    glReadPixels(0, 0, W, H, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
    checkgl("luminance readback");

    int mismatches = 0, rgba_wrong = 0;
    int first_bad_x = -1, first_bad_true = 0, first_bad_rgba = 0, first_bad_lum = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = y*W+x;
            int true_v = x % 256;
            int rgba_v = rgba[idx*4];
            int lum_v = lum[idx];
            if (rgba_v != true_v) rgba_wrong++; /* sanity check on the control itself */
            if (lum_v != true_v) {
                mismatches++;
                if (first_bad_x < 0) { first_bad_x = x; first_bad_true = true_v; first_bad_rgba = rgba_v; first_bad_lum = lum_v; }
            }
        }
    }
    printf("=== readback-luminance-probe: %dx%d, values 0..255 repeating ===\n", W, H);
    printf("RGBA control: %d/%d wrong (sanity check on the render itself)\n", rgba_wrong, W*H);
    printf("LUMINANCE readback: %d/%d wrong\n", mismatches, W*H);
    if (mismatches > 0)
        printf("  first mismatch: x=%d true=%d rgba_got=%d luminance_got=%d\n",
               first_bad_x, first_bad_true, first_bad_rgba, first_bad_lum);

    /* ---- Timing: real repeated readback comparison at production scale,
     * draw held constant (same rendered frame), isolating readback cost
     * specifically - not re-measuring draw. ---- */
    const int REPS = 500;
    struct timeval w0, w1;
    double rgba_ms = 0, lum_ms = 0;

    gettimeofday(&w0, NULL);
    for (int i = 0; i < REPS; i++) glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gettimeofday(&w1, NULL);
    rgba_ms = wall_ms(&w0, &w1);

    gettimeofday(&w0, NULL);
    for (int i = 0; i < REPS; i++) glReadPixels(0, 0, W, H, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
    gettimeofday(&w1, NULL);
    lum_ms = wall_ms(&w0, &w1);

    printf("\n=== Timing (%d reps, %dx%d readback) ===\n", REPS, W, H);
    printf("GL_RGBA readback:      %.3fms/call\n", rgba_ms/REPS);
    printf("GL_LUMINANCE readback: %.3fms/call\n", lum_ms/REPS);
    if (mismatches == 0) {
        if (lum_ms < rgba_ms)
            printf("\n-> CORRECT and FASTER (%.1f%% less time) - worth landing in the live path.\n",
                   100.0*(rgba_ms-lum_ms)/rgba_ms);
        else
            printf("\n-> CORRECT but NOT faster (%.1f%% more time) - readback format wasn't the bottleneck here.\n",
                   100.0*(lum_ms-rgba_ms)/rgba_ms);
    } else {
        printf("\n-> INCORRECT - GL_LUMINANCE readback does not return the true R value on this driver.\n"
               "   Do not use it. This is a real, newly-found driver quirk if confirmed.\n");
    }

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
