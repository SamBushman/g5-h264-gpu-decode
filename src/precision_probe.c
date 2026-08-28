/*
 * precision-probe: characterize the exact source of the diagonal-MC
 * discrepancy found in M7, using synthetic (not real-frame) inputs so the
 * expected value is hand-computable exactly, across a range of
 * magnitudes and computation structures. Tests:
 *   1. Does a single scalar multiply (x*20.0) already lose precision?
 *   2. Does the raw 6-tap sum lose precision, and does the error scale
 *      with magnitude (consistent with reduced-mantissa float, e.g. FP24)
 *      or is it constant/threshold-like (consistent with a compiler bug)?
 *   3. Does restructuring the SAME expression into more/fewer intermediate
 *      named variables change the result (would point at a compiler
 *      scheduling bug rather than true numeric precision)?
 */

#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void checkgl(const char *w) {
    GLenum e = glGetError();
    if (e) fprintf(stderr, "GL err %s: 0x%lx\n", w, (unsigned long)e);
}
static GLhandleARB compile(GLenum t, const char *s) {
    GLhandleARB h = glCreateShaderObjectARB(t);
    glShaderSourceARB(h, 1, &s, NULL);
    glCompileShaderARB(h);
    GLint ok = 0;
    glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) {
        char log[4096]; GLsizei n;
        glGetInfoLogARB(h, sizeof log, &n, log);
        fprintf(stderr, "compile fail:\n%s\n", log);
        exit(1);
    }
    return h;
}
static GLhandleARB linkp(const char *vs, const char *fs) {
    GLhandleARB p = glCreateProgramObjectARB();
    glAttachObjectARB(p, compile(GL_VERTEX_SHADER_ARB, vs));
    glAttachObjectARB(p, compile(GL_FRAGMENT_SHADER_ARB, fs));
    glLinkProgramARB(p);
    GLint ok = 0;
    glGetObjectParameterivARB(p, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) {
        char log[4096]; GLsizei n;
        glGetInfoLogARB(p, sizeof log, &n, log);
        fprintf(stderr, "link fail:\n%s\n", log);
        exit(1);
    }
    return p;
}

static const char *vs = "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex; "
                         "gl_TexCoord[0]=gl_MultiTexCoord0;}";

/* Renders one value, encoded as result/SCALE + 0.5, reading back a single
 * pixel and decoding. `expr_fs` computes `float result = ...;` from six
 * uniforms u0..u5 (so we can feed exact known inputs without depending on
 * texture sampling at all - eliminates the texture-fetch path as a
 * variable entirely for this diagnostic). */
static float run_expr(const char *body, float u[6], float scale) {
    char fs[4096];
    snprintf(fs, sizeof fs,
             "uniform float u0,u1,u2,u3,u4,u5;\n"
             "void main() {\n%s\n"
             "  gl_FragColor = vec4(result/%f + 0.5, 0.0, 0.0, 1.0);\n"
             "}\n",
             body, scale);
    GLhandleARB prog = linkp(vs, fs);
    glUseProgramObjectARB(prog);
    const char *names[6] = {"u0", "u1", "u2", "u3", "u4", "u5"};
    for (int i = 0; i < 6; i++)
        glUniform1fARB(glGetUniformLocationARB(prog, names[i]), u[i]);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4);
    glEnd();
    glFinish();
    checkgl("draw");
    unsigned char px[4];
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glDeleteObjectARB(prog);
    return ((px[0] / 255.0f) - 0.5f) * scale;
}

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL);
    aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf;
    aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0);
    aglSetCurrentContext(ctx);
    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    printf("=== Test 1: single multiply x*20.0, various magnitudes ===\n");
    for (float x = 10; x <= 300; x *= 3) {
        float u[6] = {x, 0, 0, 0, 0, 0};
        float got = run_expr("  float result = u0 * 20.0;", u, 65536.0f);
        float want = x * 20.0f;
        printf("  x=%8.1f  want=%9.1f  got=%9.1f  diff=%8.2f\n", x, want, got, got - want);
    }

    printf("\n=== Test 2: 6-tap raw sum (a0+a3)*20-(a1+a4)*5+(a2+a5), scaled pixel-like inputs ===\n");
    /* Sweep a synthetic "pixel-like" 6-tap input at increasing overall
     * magnitude (scale factor k applied to a fixed pattern), matching the
     * real h_raw shape but with hand-verifiable exact expected output. */
    const char *tap_body =
        "  float result = (u0+u3)*20.0 - (u1+u4)*5.0 + (u2+u5);";
    float pattern[6] = {200, 180, 150, 190, 160, 140}; /* pixel-like values 0..255 */
    for (float k = 0.1f; k <= 30.0f; k *= 3.0f) {
        float u[6];
        double want = 0;
        float coeff[6] = {20, -5, 1, 20, -5, 1}; /* matches (u0+u3)*20 -(u1+u4)*5 +(u2+u5) per-term */
        /* recompute want directly from formula for clarity */
        for (int i = 0; i < 6; i++) u[i] = pattern[i] * k;
        want = (double)(u[0] + u[3]) * 20.0 - (double)(u[1] + u[4]) * 5.0 + (double)(u[2] + u[5]);
        float got = run_expr(tap_body, u, 65536.0f);
        printf("  k=%5.1f  inputs~%.0f..%.0f  want=%10.1f  got=%10.1f  diff=%9.2f  relerr=%.4f%%\n",
               k, pattern[5]*k, pattern[0]*k, want, got, got - (float)want,
               100.0 * fabs(got - (float)want) / (fabs(want) + 1e-6));
        (void)coeff;
    }

    printf("\n=== Test 3: same magnitude as the real failing case, but fully unrolled (no named intermediates) ===\n");
    {
        /* Reproduce the exact real failing values: a0..a5 approx from the
         * real MB(14,1) case (h_raw inputs were real pixel values around
         * 150-220 based on the frame content there). Use representative
         * values in that range, same magnitude as what triggered the bug. */
        float u[6] = {223, 195, 210, 187, 165, 201}; /* representative pixel-range values */
        double want = (double)(u[0] + u[3]) * 20.0 - (double)(u[1] + u[4]) * 5.0 + (double)(u[2] + u[5]);
        float got1 = run_expr(
            "  float result = (u0+u3)*20.0 - (u1+u4)*5.0 + (u2+u5);", u, 65536.0f);
        float got2 = run_expr(
            "  float t0=u0+u3; float t1=u1+u4; float t2=u2+u5;\n"
            "  float m0=t0*20.0; float m1=t1*5.0;\n"
            "  float result = m0 - m1 + t2;", u, 65536.0f);
        printf("  want=%.1f  one-line=%.1f (diff %.2f)  step-by-step=%.1f (diff %.2f)\n",
               want, got1, got1-(float)want, got2, got2-(float)want);
    }

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
