/*
 * lum-float-signed-probe: tests a previously-flagged, still-open lead from
 * gpu_live_decode_test.c's "Item 9 fix ATTEMPTED then REVERTED" comment
 * (gpu_idct_batch): switching the coefficient upload texture from
 * GL_RGBA_FLOAT32_ATI (4 floats/texel, only .r used) to
 * GL_LUMINANCE_FLOAT32_ATI (1 float/texel) was tried once to quarter the
 * upload payload - same idea that gave reftex_lookup_or_upload a real 28x
 * speedup (item 9 investigation) - but caused a severe correctness
 * regression (3.9-5.3% mismatch). Diagnosis at the time: DCT coefficients
 * are routinely NEGATIVE (unlike reftex's non-negative [0,255] pixel
 * data), and GL_LUMINANCE_FLOAT32_ATI apparently doesn't preserve negative
 * values correctly on this driver - flagged as "worth a dedicated
 * follow-up (e.g. a signed-bias encoding...) if revisited later."
 *
 * This is that follow-up. Hypothesis: if the bug is specifically about
 * negative VALUES (not some other property), biasing every value to be
 * non-negative before upload (store coeff+BIAS, subtract BIAS back out in
 * the shader) should sidestep it entirely. Tests a wide sweep of
 * representative signed integer values (covering real dequantized H.264
 * coefficient range with margin) through GL_LUMINANCE_FLOAT32_ATI+bias,
 * compared against the same values round-tripped through the currently-
 * working GL_RGBA_FLOAT32_ATI (no bias needed) as a control.
 */

#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

/* Encodes the sampled value (already un-biased in-shader) via the SAME
 * hi*256+lo-32768 two-byte trick gpu_idct_batch's real output already
 * uses - exact 16-bit integer round-trip fidelity through an 8-bit
 * UNSIGNED_BYTE readback, matching production exactly. */
static const char *fs_luminance_biased =
"uniform sampler2DRect coeffTex;\n"
"uniform float bias;\n"
"void main() {\n"
"  float raw = texture2DRect(coeffTex, floor(gl_FragCoord.xy)+vec2(0.5,0.5)).r;\n"
"  float v = raw - bias;\n"
"  float vc = v + 32768.0;\n"
"  float hi = floor(vc / 256.0);\n"
"  float lo = vc - hi * 256.0;\n"
"  gl_FragColor = vec4(hi/255.0, lo/255.0, 0.0, 1.0);\n"
"}\n";

static const char *fs_rgba_control =
"uniform sampler2DRect coeffTex;\n"
"void main() {\n"
"  float v = texture2DRect(coeffTex, floor(gl_FragCoord.xy)+vec2(0.5,0.5)).r;\n"
"  float vc = v + 32768.0;\n"
"  float hi = floor(vc / 256.0);\n"
"  float lo = vc - hi * 256.0;\n"
"  gl_FragColor = vec4(hi/255.0, lo/255.0, 0.0, 1.0);\n"
"}\n";

/* Representative test values: dense sweep of small magnitudes (where most
 * real dequantized coefficients live) plus sparse coverage out to a wide
 * margin beyond any real H.264 4x4 coefficient this project has ever
 * logged (qscale 20-23 in the M5 hook verification), covering the type's
 * full practical range with real safety margin. */
static void build_test_values(int *vals, int *n) {
    int c = 0;
    for (int v = -64; v <= 64; v++) vals[c++] = v;             /* dense near zero */
    int wide[] = {-8192,-4096,-2048,-1024,-512,-256,-128,
                  128,256,512,1024,2048,4096,8192,
                  -1,0,1,-2,2,32767,-32767};
    for (int i = 0; i < (int)(sizeof(wide)/sizeof(wide[0])); i++) vals[c++] = wide[i];
    *n = c;
}

int main(void) {
    int vals[256], nvals;
    build_test_values(vals, &nvals);

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(nvals + 8, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glViewport(0, 0, nvals, 1);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, nvals, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    const float BIAS = 16384.0f; /* safely covers the -8192..8192 test range with margin */

    /* ---- Pass 1: GL_LUMINANCE_FLOAT32_ATI + signed-bias encoding ---- */
    float *lumdata = (float*)malloc(sizeof(float) * nvals);
    for (int i = 0; i < nvals; i++) lumdata[i] = (float)vals[i] + BIAS;
    GLuint lumTex; glGenTextures(1,&lumTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,lumTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_LUMINANCE_FLOAT32_ATI,nvals,1,0,GL_LUMINANCE,GL_FLOAT,lumdata);
    checkgl("luminance upload");
    free(lumdata);

    GLhandleARB progL = linkp(vs_plain, fs_luminance_biased);
    glUseProgramObjectARB(progL);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,lumTex);
    glUniform1iARB(glGetUniformLocationARB(progL,"coeffTex"),0);
    glUniform1fARB(glGetUniformLocationARB(progL,"bias"),BIAS);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(nvals,0);glVertex2f(nvals,1);glVertex2f(0,1); glEnd();
    glFinish(); checkgl("luminance draw");
    unsigned char *lumpix = (unsigned char*)malloc((size_t)nvals*4);
    glReadPixels(0,0,nvals,1,GL_RGBA,GL_UNSIGNED_BYTE,lumpix);

    /* ---- Pass 2: GL_RGBA_FLOAT32_ATI control (currently-working format) ---- */
    float *rgbadata = (float*)malloc(sizeof(float) * nvals * 4);
    for (int i = 0; i < nvals; i++) {
        rgbadata[i*4] = (float)vals[i]; rgbadata[i*4+1]=rgbadata[i*4+2]=0; rgbadata[i*4+3]=1;
    }
    GLuint rgbaTex; glGenTextures(1,&rgbaTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,rgbaTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,nvals,1,0,GL_RGBA,GL_FLOAT,rgbadata);
    checkgl("rgba upload");
    free(rgbadata);

    GLhandleARB progR = linkp(vs_plain, fs_rgba_control);
    glUseProgramObjectARB(progR);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,rgbaTex);
    glUniform1iARB(glGetUniformLocationARB(progR,"coeffTex"),0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(nvals,0);glVertex2f(nvals,1);glVertex2f(0,1); glEnd();
    glFinish(); checkgl("rgba draw");
    unsigned char *rgbapix = (unsigned char*)malloc((size_t)nvals*4);
    glReadPixels(0,0,nvals,1,GL_RGBA,GL_UNSIGNED_BYTE,rgbapix);

    printf("=== lum-float-signed-probe: %d test values ===\n", nvals);
    int lum_ok = 0, rgba_ok = 0, lum_fail_shown = 0;
    for (int i = 0; i < nvals; i++) {
        int lum_hi = lumpix[i*4], lum_lo = lumpix[i*4+1];
        int lum_v = lum_hi*256 + lum_lo - 32768;
        int rgba_hi = rgbapix[i*4], rgba_lo = rgbapix[i*4+1];
        int rgba_v = rgba_hi*256 + rgba_lo - 32768;
        int lum_match = (lum_v == vals[i]);
        int rgba_match = (rgba_v == vals[i]);
        if (lum_match) lum_ok++;
        if (rgba_match) rgba_ok++;
        if (!lum_match && lum_fail_shown < 15) {
            printf("  MISMATCH val=%6d  luminance+bias got=%6d  rgba-control got=%6d\n", vals[i], lum_v, rgba_v);
            lum_fail_shown++;
        }
    }
    printf("\nluminance+bias: %d/%d exact round-trips\n", lum_ok, nvals);
    printf("rgba control:   %d/%d exact round-trips\n", rgba_ok, nvals);
    if (lum_ok == nvals) {
        printf("\n-> HYPOTHESIS CONFIRMED: signed-bias encoding fixes GL_LUMINANCE_FLOAT32_ATI's negative-value bug.\n");
        printf("   Safe to reclaim the reverted item-9 optimization (quarter the IDCT coefficient upload payload).\n");
    } else {
        printf("\n-> Hypothesis NOT confirmed - GL_LUMINANCE_FLOAT32_ATI still loses data even with bias.\n");
        printf("   Real bug is not simply about negative values. Do not pursue this optimization further.\n");
    }

    free(lumpix); free(rgbapix);
    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
