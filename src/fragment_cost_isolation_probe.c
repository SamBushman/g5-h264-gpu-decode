/*
 * fragment-cost-isolation-probe: directly verifies the conclusion drawn
 * from multi-ref-timing-probe/multi-ref-overlay-probe ("per-fragment
 * texture-fetch cost dominates over fixed per-dispatch overhead on this
 * driver") by isolating each variable independently, instead of only
 * comparing two whole-scenario totals against each other.
 *
 * Experiment A: hold dispatch count (1 draw+readback), width (1024px),
 * and readback size all FIXED. Vary ONLY how many independent 27-fetch
 * MC-formula evaluations each fragment performs (K=1..8, each at a
 * genuinely different texel offset so the compiler can't collapse
 * repeated fetches into one - same anti-dead-code-elimination principle
 * precision-boundary-probe used for its uniform pairs). If per-fragment
 * cost dominates, wall time should scale roughly linearly with K.
 *
 * Experiment B: hold per-fragment complexity FIXED (K=1, the real
 * production formula) and vary ONLY the fragment count (width, at fixed
 * height=16) across a real range (128..4096px). Fit wall time as
 * fixed_overhead + per_fragment_cost * fragment_count via the two
 * extreme points (128 and 4096) - if fixed_overhead is small relative to
 * the per-fragment term at real production widths (~1024-2048px), that
 * directly confirms fragment-count/complexity, not dispatch count, is
 * what's actually being paid for.
 *
 * Together these give real numbers for BOTH terms in the cost model
 * (fixed per-dispatch overhead vs. per-fragment cost) instead of only
 * comparing two specific whole scenarios that happened to differ in both
 * variables at once.
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

/* Real production MC formula (27 texture2DRect fetches), reused verbatim
 * as one unit of "real per-fragment work" - same halfH/halfV/full-pel
 * structure as fs_mc_batch_var, parameterized as a function so it can be
 * called K times at K different offsets. */
static const char *mc_unit_glsl =
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float halfHt(sampler2DRect t, vec2 b) {\n"
"  float a2=texture2DRect(t,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(t,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(t,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(t,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(t,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(t,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfVt(sampler2DRect t, vec2 b) {\n"
"  float a2=texture2DRect(t,b+vec2(0.0,-2.0)).r*255.0;\n"
"  float a1=texture2DRect(t,b+vec2(0.0,-1.0)).r*255.0;\n"
"  float a0=texture2DRect(t,b+vec2(0.0, 0.0)).r*255.0;\n"
"  float a3=texture2DRect(t,b+vec2(0.0, 1.0)).r*255.0;\n"
"  float a4=texture2DRect(t,b+vec2(0.0, 2.0)).r*255.0;\n"
"  float a5=texture2DRect(t,b+vec2(0.0, 3.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float mcOne(sampler2DRect t, vec2 b, float hPhase, float vPhase) {\n"
"  float full00 = texture2DRect(t, b).r*255.0;\n"
"  float full10 = texture2DRect(t, b+vec2(1.0,0.0)).r*255.0;\n"
"  float full01 = texture2DRect(t, b+vec2(0.0,1.0)).r*255.0;\n"
"  float halfH0 = halfHt(t, b);\n"
"  float halfH1 = halfHt(t, b+vec2(0.0,1.0));\n"
"  float halfV0 = halfVt(t, b);\n"
"  float halfV1 = halfVt(t, b+vec2(1.0,0.0));\n"
"  float result = full00;\n"
"  if (hPhase == 0.0 && vPhase == 0.0) { result = full00; }\n"
"  else if (hPhase == 2.0 && vPhase == 0.0) { result = halfH0; }\n"
"  else if (hPhase == 0.0 && vPhase == 2.0) { result = halfV0; }\n"
"  else if (vPhase == 0.0) { float fullOp = full00; if (hPhase == 3.0) fullOp = full10; result = floor((fullOp + halfH0 + 1.0) / 2.0); }\n"
"  else if (hPhase == 0.0) { float fullOp = full00; if (vPhase == 3.0) fullOp = full01; result = floor((fullOp + halfV0 + 1.0) / 2.0); }\n"
"  else { float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1; float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1; result = floor((hOp + vOp + 1.0) / 2.0); }\n"
"  return result;\n"
"}\n";

/* Builds a shader computing K independent mcOne() evaluations, each at a
 * genuinely different integer texel offset (0..K-1 columns apart) so the
 * compiler cannot collapse them into fewer real fetches, and averages the
 * results into the final output - every evaluation has real data
 * dependence on the output, so none can be dead-code-eliminated. */
static char *build_k_shader(int K) {
    static char buf[16384];
    int off = 0;
    off += snprintf(buf+off, sizeof(buf)-off,
        "uniform sampler2DRect refTex;\n"
        "uniform sampler2DRect blockInfoTex;\n"
        "uniform sampler2DRect colInfoTex;\n"
        "%s"
        "void main(){\n"
        "  float col = floor(gl_FragCoord.x);\n"
        "  float row = floor(gl_FragCoord.y);\n"
        "  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));\n"
        "  float blockIdx = colInfo.r;\n"
        "  float localX = colInfo.g;\n"
        "  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx+0.5, 0.5));\n"
        "  vec2 b = vec2(info.r + localX, info.g + row);\n"
        "  float hPhase = info.b, vPhase = info.a;\n"
        "  float acc = 0.0;\n",
        mc_unit_glsl);
    for (int k = 0; k < K; k++) {
        off += snprintf(buf+off, sizeof(buf)-off,
            "  acc += mcOne(refTex, b + vec2(%d.0, 0.0), hPhase, vPhase);\n", k);
    }
    off += snprintf(buf+off, sizeof(buf)-off,
        "  gl_FragColor = vec4((acc/%d.0)/255.0, 0.0, 0.0, 1.0);\n"
        "}\n", K);
    return buf;
}

static double wall_ms(struct timeval *a, struct timeval *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_usec - a->tv_usec) / 1000.0;
}

static double time_dispatch(GLhandleARB prog, GLuint refTex, GLuint blockInfoTex, GLuint colInfoTex,
                             int vw, int reps, unsigned char *pixels) {
    struct timeval w0, w1;
    gettimeofday(&w0, NULL);
    for (int rep = 0; rep < reps; rep++) {
        glViewport(0, 0, vw, 16);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw, 0, 16, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glUseProgramObjectARB(prog);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
        glUniform1iARB(glGetUniformLocationARB(prog,"refTex"),0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
        glUniform1iARB(glGetUniformLocationARB(prog,"blockInfoTex"),1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
        glUniform1iARB(glGetUniformLocationARB(prog,"colInfoTex"),2);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(vw,0);glVertex2f(vw,16);glVertex2f(0,16); glEnd();
        glReadPixels(0, 0, vw, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    gettimeofday(&w1, NULL);
    return wall_ms(&w0, &w1) / reps;
}

int main(void) {
    const int FRAME_W = 1024, FRAME_H = 64; /* generous margin for offset fetches up to K=8 */
    const int NBLOCKS = 256; /* enough blocks to cover width up to 4096px (256*16) */

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4096, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    unsigned char *buf = (unsigned char*)malloc(FRAME_W*FRAME_H);
    for (int i = 0; i < FRAME_W*FRAME_H; i++) buf[i] = (i*37)&0xFF;
    GLuint refTex; glGenTextures(1,&refTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_LUMINANCE8,FRAME_W,FRAME_H,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,buf);
    checkgl("reftex upload");

    float *blockinfo = (float*)malloc(sizeof(float)*NBLOCKS*4);
    for (int i = 0; i < NBLOCKS; i++) {
        blockinfo[i*4+0] = 32.0f; blockinfo[i*4+1] = (float)(10 + (i % 40));
        blockinfo[i*4+2] = (float)(i % 4); blockinfo[i*4+3] = (float)((i / 4) % 4);
    }
    GLuint blockInfoTex; glGenTextures(1,&blockInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,blockInfoTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,NBLOCKS,1,0,GL_RGBA,GL_FLOAT,blockinfo);
    checkgl("blockinfo upload");

    int vw_max = NBLOCKS * 16;
    float *colinfo = (float*)malloc(sizeof(float)*vw_max*4);
    for (int i = 0; i < NBLOCKS; i++)
        for (int c = 0; c < 16; c++) {
            int idx = i*16+c;
            colinfo[idx*4+0] = (float)i; colinfo[idx*4+1] = (float)c; colinfo[idx*4+2] = 0; colinfo[idx*4+3] = 1;
        }
    GLuint colInfoTex; glGenTextures(1,&colInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,colInfoTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,vw_max,1,0,GL_RGBA,GL_FLOAT,colinfo);
    checkgl("colinfo upload");

    unsigned char *pixels = (unsigned char*)malloc((size_t)vw_max*16*4);
    const int REPS = 300;

    /* ---- Experiment A: fixed width=1024px (64 blocks), vary K=1..8 ---- */
    printf("=== Experiment A: fixed width=1024px, vary per-fragment work (K evaluations/fragment) ===\n");
    const int W_FIXED = 1024;
    double timeK[9];
    int ks[] = {1,2,3,4,6,8};
    for (int ki = 0; ki < 6; ki++) {
        int K = ks[ki];
        GLhandleARB prog = linkp(vs_plain, build_k_shader(K));
        double t = time_dispatch(prog, refTex, blockInfoTex, colInfoTex, W_FIXED, REPS, pixels);
        timeK[K] = t;
        printf("  K=%d (%2d fetches/fragment): %.3fms/dispatch\n", K, K*27, t);
        glDeleteObjectARB(prog);
    }
    checkgl("experiment A");
    double slopeK = (timeK[8] - timeK[1]) / (8 - 1);
    double interceptK = timeK[1] - slopeK * 1;
    printf("  Linear fit (K=1..8): ~%.4fms fixed + %.4fms per extra K-unit (R-squared not computed, eyeball the table above for linearity)\n",
           interceptK, slopeK);

    /* ---- Experiment B: fixed K=1 (real production formula), vary width ---- */
    printf("\n=== Experiment B: fixed K=1 (real formula), vary fragment count (width) ===\n");
    GLhandleARB progK1 = linkp(vs_plain, build_k_shader(1));
    int widths[] = {128, 256, 512, 1024, 2048, 4096};
    double timeW[6];
    for (int wi = 0; wi < 6; wi++) {
        int w = widths[wi];
        double t = time_dispatch(progK1, refTex, blockInfoTex, colInfoTex, w, REPS, pixels);
        timeW[wi] = t;
        printf("  width=%4dpx (%3d blocks): %.3fms/dispatch\n", w, w/16, t);
    }
    checkgl("experiment B");
    double slopeW = (timeW[5] - timeW[0]) / (4096 - 128);
    double interceptW = timeW[0] - slopeW * 128;
    printf("  Linear fit (width=128..4096): fixed_overhead ~= %.4fms, per-pixel-width cost ~= %.6fms/px\n",
           interceptW, slopeW);
    printf("  At a realistic production width (2048px): fixed=%.1f%%, scales-with-width=%.1f%% of total (%.3fms)\n",
           100.0*interceptW/(interceptW+slopeW*2048), 100.0*(slopeW*2048)/(interceptW+slopeW*2048), interceptW+slopeW*2048);

    /* ---- Experiment C: does binding a SECOND, DISTINCT texture (matching
     * the real overlay/multi-ref scenario) cost more than sampling the
     * SAME texture twice at different offsets (Experiment A's K=2 case)?
     * Reconciles a real discrepancy: Experiment A's K=2 (same texture,
     * two offsets) vs. multi-ref-overlay-probe's actual two-texture case
     * measured very differently for what should be similar raw fetch
     * counts - this isolates whether texture-UNIT count itself is a real,
     * separate cost factor beyond raw fetch instruction count. ---- */
    printf("\n=== Experiment C: one texture x2 offsets vs. two DISTINCT bound textures, same width/fetch count ===\n");
    unsigned char *buf2 = (unsigned char*)malloc(FRAME_W*FRAME_H);
    for (int i = 0; i < FRAME_W*FRAME_H; i++) buf2[i] = (i*53+11)&0xFF;
    GLuint refTex2; glGenTextures(1,&refTex2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex2);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_LUMINANCE8,FRAME_W,FRAME_H,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,buf2);
    checkgl("refTex2 upload");

    static char twotex_buf[16384];
    {
        int off = 0;
        off += snprintf(twotex_buf+off, sizeof(twotex_buf)-off,
            "uniform sampler2DRect refTexA;\n"
            "uniform sampler2DRect refTexB;\n"
            "uniform sampler2DRect blockInfoTex;\n"
            "uniform sampler2DRect colInfoTex;\n"
            "%s"
            "void main(){\n"
            "  float col = floor(gl_FragCoord.x);\n"
            "  float row = floor(gl_FragCoord.y);\n"
            "  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));\n"
            "  float blockIdx = colInfo.r;\n"
            "  float localX = colInfo.g;\n"
            "  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx+0.5, 0.5));\n"
            "  vec2 b = vec2(info.r + localX, info.g + row);\n"
            "  float hPhase = info.b, vPhase = info.a;\n"
            "  float acc = mcOne(refTexA, b, hPhase, vPhase);\n"
            "  acc += mcOne(refTexB, b, hPhase, vPhase);\n"
            "  gl_FragColor = vec4((acc/2.0)/255.0, 0.0, 0.0, 1.0);\n"
            "}\n",
            mc_unit_glsl);
    }
    GLhandleARB progTwoTex = linkp(vs_plain, twotex_buf);
    {
        struct timeval w0, w1;
        gettimeofday(&w0, NULL);
        for (int rep = 0; rep < REPS; rep++) {
            glViewport(0, 0, W_FIXED, 16);
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W_FIXED, 0, 16, -1, 1);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glUseProgramObjectARB(progTwoTex);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
            glUniform1iARB(glGetUniformLocationARB(progTwoTex,"refTexA"),0);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex2);
            glUniform1iARB(glGetUniformLocationARB(progTwoTex,"refTexB"),1);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
            glUniform1iARB(glGetUniformLocationARB(progTwoTex,"blockInfoTex"),2);
            glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
            glUniform1iARB(glGetUniformLocationARB(progTwoTex,"colInfoTex"),3);
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W_FIXED,0);glVertex2f(W_FIXED,16);glVertex2f(0,16); glEnd();
            glReadPixels(0, 0, W_FIXED, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        gettimeofday(&w1, NULL);
        double t = wall_ms(&w0, &w1) / REPS;
        checkgl("experiment C");
        printf("  K=2, ONE texture (2 offsets), width=1024px:      %.3fms/dispatch  (Experiment A's own K=2 result)\n", timeK[2]);
        printf("  K=2-equivalent, TWO DISTINCT textures, width=1024px: %.3fms/dispatch\n", t);
        if (t > timeK[2] * 1.15)
            printf("  -> Binding a SECOND DISTINCT texture costs MORE than raw fetch count alone predicts\n"
                   "     (%.1fx vs same-texture-two-offsets) - texture-unit/cache-locality cost is a real,\n"
                   "     SEPARATE factor beyond per-fragment ALU/fetch instruction count.\n", t/timeK[2]);
        else
            printf("  -> Comparable to the same-texture case - texture unit count itself is not a major\n"
                   "     additional cost factor; raw fetch/ALU instruction count explains most of it.\n");
    }

    printf("\n=== Conclusion ===\n");
    if (slopeK > 0 && (timeK[8]/timeK[1]) > 3.0)
        printf("Experiment A: cost scales substantially with per-fragment work (%.1fx from K=1 to K=8,\n"
               "  vs 8x theoretical if purely linear) - CONFIRMS per-fragment compute is a real, scaling cost,\n"
               "  not a fixed tax paid once per dispatch regardless of shader complexity.\n", timeK[8]/timeK[1]);
    else
        printf("Experiment A: cost did NOT scale much with K (%.2fx from K=1 to K=8) - would CONTRADICT\n"
               "  the earlier conclusion; per-fragment work may not be the dominant cost after all.\n", timeK[8]/timeK[1]);
    if (interceptW < (interceptW + slopeW*2048) * 0.3)
        printf("Experiment B: at realistic widths, fixed per-dispatch overhead is a MINORITY of total cost\n"
               "  (~%.0f%% at 2048px) - CONFIRMS the width/fragment-count-scaling term dominates, matching\n"
               "  Experiment A's finding that per-fragment cost (not dispatch count) is the real driver.\n",
               100.0*interceptW/(interceptW+slopeW*2048));
    else
        printf("Experiment B: fixed per-dispatch overhead is a LARGE fraction of total cost even at realistic\n"
               "  widths - would suggest dispatch count matters more than previously concluded.\n");

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
