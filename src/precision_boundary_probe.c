/*
 * precision-boundary-probe: settle the FP24-vs-FP32 question empirically,
 * directly on the real hardware, instead of continuing to reverse-engineer
 * the driver's internal codegen decision (see plan.md, "Precision question
 * (§4): dynamic gdb investigation done" - both static (Ghidra) and dynamic
 * (gdb) analysis hit a real ceiling: the R520-specific codegen class is a
 * C++ RTTI-only symbol, invisible to both tools' symbol resolution).
 *
 * AMD's own R5xx_Acceleration_v1.5 doc (§8.7) says the US fragment ALU is
 * true IEEE 32-bit float (23-bit mantissa). This project's own quirk #14
 * (precision-probe's x*20.0 test, ~0.55% relative error) contradicts that.
 * Rather than argue from either source, ask the hardware directly with a
 * test whose expected answer is exactly computable by hand:
 *
 *   True FP32 (23-bit mantissa) represents every integer exactly up to
 *   2^24. A reduced format with M stored mantissa bits (FP24-style ATI
 *   "full precision" = 16 stored bits, s1e7m16) only represents integers
 *   exactly up to 2^(M+1). So: compute (2^k + delta) - 2^k in-shader for
 *   increasing k, using uniforms (not compile-time constants, and NOT the
 *   textually same variable on both sides of the subtraction - see below)
 *   so the compiler can't algebraically simplify it away. The largest k
 *   at which a given delta still survives directly reveals the ALU's real
 *   effective mantissa width - independent of what the driver's compiler
 *   decided to do internally, and independent of the RE ceiling above.
 *
 * Defends against a real pitfall: `(u0+u1)-u0` is an algebraic identity a
 * naive constant-folder might "simplify" to `u1` even for a non-constant
 * uniform, which would be valid for real numbers but WRONG for floats and
 * would falsely show "always exact" regardless of real hardware behavior.
 * Fixed by passing the base value twice, through two DIFFERENT uniform
 * slots (u0 for the add, u2 for the subtract) - textually distinct, so a
 * compiler cannot statically know they're equal without doing real
 * constant propagation across a uniform (which it doesn't have to do,
 * since uniforms are opaque at compile time).
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

static const char *vs = "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}";

/* result = (u0 + u1) - u2, encoded as result/scale + 0.5 into an 8-bit
 * readback channel (same harness pattern as precision_probe.c's run_expr -
 * proven working on this driver). scale chosen per-call so the expected
 * small delta (0..a few units) uses the readback's full 8-bit range. */
static float run_delta(float base, float delta, float scale) {
    char fs[1024];
    snprintf(fs, sizeof fs,
             "uniform float u0,u1,u2;\n"
             "void main() {\n"
             "  float result = (u0 + u1) - u2;\n"
             "  gl_FragColor = vec4(result/%f + 0.5, 0.0, 0.0, 1.0);\n"
             "}\n", scale);
    GLhandleARB prog = linkp(vs, fs);
    glUseProgramObjectARB(prog);
    glUniform1fARB(glGetUniformLocationARB(prog, "u0"), base);
    glUniform1fARB(glGetUniformLocationARB(prog, "u1"), delta);
    glUniform1fARB(glGetUniformLocationARB(prog, "u2"), base);
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

    /* Test 1: does base+1.0 survive, for base = 2^k, k = 8..26?
     * True FP32 must hold exact through k=23 (2^23+1 exact, ULP=1 below
     * 2^24) and MUST fail at k=24 (ULP becomes 2 there - not a driver
     * bug, real IEEE754 behavior). A reduced-mantissa ALU (FP24-style,
     * 16 stored bits = 17 significant bits) would fail starting k=17. */
    printf("=== Test 1: does 2^k + 1.0 survive exactly? (k=8..26) ===\n");
    int first_fail_k = -1;
    for (int k = 8; k <= 26; k++) {
        float base = (float)(1u << (k > 30 ? 30 : k));
        if (k >= 31) base = powf(2.0f, (float)k); /* not reached, guard only */
        else base = powf(2.0f, (float)k);
        float got = run_delta(base, 1.0f, 4.0f);
        int exact = fabsf(got - 1.0f) < 0.01f;
        printf("  k=%2d  base=2^%-2d=%14.0f  got_delta=%6.3f  %s\n",
               k, k, base, got, exact ? "EXACT" : "LOST");
        if (!exact && first_fail_k < 0) first_fail_k = k;
    }
    if (first_fail_k < 0) {
        printf("  -> never lost +1 through k=26 (shouldn't happen - even real FP32\n");
        printf("     must fail by k=24; if this prints, something is wrong with the probe itself).\n");
    } else {
        printf("  -> first loses +1.0 at k=%d (real FP32 would first fail at k=24)\n", first_fail_k);
        if (first_fail_k < 24)
            printf("     CONFIRMS reduced-precision ALU - fails %d bits earlier than documented FP32.\n", 24 - first_fail_k);
        else if (first_fail_k == 24)
            printf("     MATCHES true FP32 behavior exactly - AMD doc's claim holds up on this driver.\n");
    }

    /* Test 2: ULP characterization around the transition - for a few k
     * at/after first_fail_k, binary-search the smallest power-of-two
     * delta that still survives, to get the real effective mantissa
     * width at that magnitude directly (not just a yes/no at delta=1). */
    if (first_fail_k > 0) {
        printf("\n=== Test 2: smallest surviving power-of-two delta, near/after the transition ===\n");
        int test_ks[4] = {first_fail_k, first_fail_k + 1, first_fail_k + 3, first_fail_k + 6};
        for (int t = 0; t < 4; t++) {
            int k = test_ks[t];
            if (k > 30) continue;
            float base = powf(2.0f, (float)k);
            int min_exact_log2 = -1;
            for (int d = 0; d <= 12; d++) {
                float delta = powf(2.0f, (float)d);
                float scale = delta * 4.0f;
                float got = run_delta(base, delta, scale);
                int exact = fabsf(got - delta) < delta * 0.05f + 0.01f;
                if (exact) { min_exact_log2 = d; break; }
            }
            if (min_exact_log2 >= 0) {
                int effective_mantissa = k - min_exact_log2;
                printf("  k=%2d (base=2^%-2d): smallest exact delta = 2^%-2d  ->  effective mantissa ~%d bits\n",
                       k, k, min_exact_log2, effective_mantissa);
            } else {
                printf("  k=%2d (base=2^%-2d): no power-of-two delta up to 2^12 survived (unexpected)\n", k, k);
            }
        }
    }

    /* Test 3: decode-realistic magnitudes - do values in the actual range
     * this project's shaders operate on (pixel sums, IDCT/MC intermediate
     * accumulators, roughly 0..4096) show ANY loss at all? Directly
     * answers the practical question independent of the big-picture
     * threshold above. */
    printf("\n=== Test 3: decode-realistic magnitudes (pixel/coefficient range) ===\n");
    float real_bases[] = {64, 255, 512, 1024, 2048, 4095};
    for (int i = 0; i < 6; i++) {
        float got = run_delta(real_bases[i], 1.0f, 4.0f);
        int exact = fabsf(got - 1.0f) < 0.01f;
        printf("  base=%6.0f  got_delta=%6.3f  %s\n", real_bases[i], got, exact ? "EXACT" : "LOST");
    }

    /* Test 4: re-run precision_probe.c's original Test 1 (x*20.0), which
     * reported ~0.55% relative error and drove quirk #14's diagonal-MC
     * two-pass workaround - but with a properly magnitude-scaled delta
     * encoding instead of that test's fixed scale=65536.0. With an 8-bit
     * readback channel, scale=65536 gives ~257-unit resolution per code -
     * larger than the x=10..300 range's own values (want=200..6000),
     * meaning that test's own quantization noise could easily have
     * produced a spurious few-percent "error" unrelated to real GPU
     * precision. Compute the true product on the CPU (double), pass it as
     * a uniform (u1, NOT textually u0*20.0 - defeats folding same as
     * above), and encode only the (GPU_result - true_value) delta at a
     * scale sized for real FP32 rounding noise (~1e-2 range), not the
     * value's own magnitude. */
    printf("\n=== Test 4: re-run original quirk #14 test (x*20.0), fairly scaled ===\n");
    for (float x = 10; x <= 8000; x *= 3) {
        double true_val = (double)x * 20.0;
        char fs[512];
        snprintf(fs, sizeof fs,
                 "uniform float u0,u1;\n"
                 "void main() {\n"
                 "  float result = (u0 * 20.0) - u1;\n"
                 "  gl_FragColor = vec4(result/2.0 + 0.5, 0.0, 0.0, 1.0);\n"
                 "}\n");
        GLhandleARB prog = linkp(vs, fs);
        glUseProgramObjectARB(prog);
        glUniform1fARB(glGetUniformLocationARB(prog, "u0"), x);
        glUniform1fARB(glGetUniformLocationARB(prog, "u1"), (float)true_val);
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
        float delta = ((px[0] / 255.0f) - 0.5f) * 2.0f;
        double relerr = 100.0 * fabs(delta) / true_val;
        printf("  x=%7.1f  true=%10.1f  gpu_delta=%8.4f  rel_err=%.5f%%\n",
               x, true_val, delta, relerr);
    }

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
