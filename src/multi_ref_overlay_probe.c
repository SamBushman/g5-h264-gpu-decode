/*
 * multi-ref-overlay-probe: follow-up to multi-ref-timing-probe, testing a
 * structurally different idea the user raised directly: since GL_RGBA
 * readback is the fast, native-format path (confirmed by
 * readback-luminance-probe: GL_LUMINANCE was 9x SLOWER, not faster,
 * because it forces a driver-side conversion the native RGBA path
 * avoids), and MC's own shader only ever writes R (G/B always 0) - could
 * two INDEPENDENT reference groups be OVERLAID into the same draw's R and
 * G channels, instead of concatenated side-by-side the way
 * multi-ref-timing-probe tried (and found 61% slower)?
 *
 * Structural difference from the already-disproven "concatenate + select"
 * approach:
 *   - Concatenate+select (tested, DISPROVEN): width = 2x one group's
 *     width, every fragment computes BOTH candidates' full 27-fetch cost
 *     then picks one via if/else - half the compute is thrown away, and
 *     the readback is 2x the bytes.
 *   - Overlay (this test): width = SAME as ONE group alone, every
 *     fragment computes BOTH groups' full 27-fetch results and writes
 *     BOTH to R and G respectively - no compute is wasted (every value
 *     written is used), and the readback is the SAME byte count as one
 *     of today's two separate dispatches (still native RGBA, no format
 *     penalty), not double.
 *
 * Total per-fragment texture-fetch work is still 2x a single dispatch's
 * (54 fetches/fragment instead of 27), same total real GPU work as before
 * either way - the question this test answers is whether trading "one
 * fixed per-dispatch/per-readback overhead instead of two" for "twice the
 * per-fragment cost, same total readback bytes" is a net win on real
 * hardware, given the per-fragment cost doubling is real but the
 * per-dispatch/readback fixed cost is now paid only once.
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

/* Real production shader, copied verbatim (single reference) - today's
 * actual approach A, unchanged from multi-ref-timing-probe. */
static const char *fs_mc_batch_var =
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"
"uniform sampler2DRect colInfoTex;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float halfH(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfV(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"void main(){\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float row = floor(gl_FragCoord.y);\n"
"  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));\n"
"  float blockIdx = colInfo.r;\n"
"  float localX = colInfo.g;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 b = vec2(info.r + localX, info.g + row);\n"
"  float hPhase = info.b, vPhase = info.a;\n"
"  float full00 = texture2DRect(refTex, b).r*255.0;\n"
"  float full10 = texture2DRect(refTex, b+vec2(1.0,0.0)).r*255.0;\n"
"  float full01 = texture2DRect(refTex, b+vec2(0.0,1.0)).r*255.0;\n"
"  float halfH0 = halfH(b);\n"
"  float halfH1 = halfH(b+vec2(0.0,1.0));\n"
"  float halfV0 = halfV(b);\n"
"  float halfV1 = halfV(b+vec2(1.0,0.0));\n"
"  float result = full00;\n"
"  if (hPhase == 0.0 && vPhase == 0.0) { result = full00; }\n"
"  else if (hPhase == 2.0 && vPhase == 0.0) { result = halfH0; }\n"
"  else if (hPhase == 0.0 && vPhase == 2.0) { result = halfV0; }\n"
"  else if (vPhase == 0.0) { float fullOp = full00; if (hPhase == 3.0) fullOp = full10; result = floor((fullOp + halfH0 + 1.0) / 2.0); }\n"
"  else if (hPhase == 0.0) { float fullOp = full00; if (vPhase == 3.0) fullOp = full01; result = floor((fullOp + halfV0 + 1.0) / 2.0); }\n"
"  else { float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1; float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1; result = floor((hOp + vOp + 1.0) / 2.0); }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

/* Overlay variant: width = ONE group's width, both groups' full MC
 * results computed and written to R (group A) and G (group B) of the
 * SAME fragment - no selection, no wasted compute, readback stays the
 * same byte count as a single dispatch. */
static const char *fs_mc_batch_overlay =
"uniform sampler2DRect refTexA;\n"
"uniform sampler2DRect refTexB;\n"
"uniform sampler2DRect blockInfoTexA;\n"
"uniform sampler2DRect blockInfoTexB;\n"
"uniform sampler2DRect colInfoTex;\n"
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
"}\n"
"void main(){\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float row = floor(gl_FragCoord.y);\n"
"  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));\n"
"  float blockIdx = colInfo.r;\n"
"  float localX = colInfo.g;\n"
"  vec4 infoA = texture2DRect(blockInfoTexA, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 bA = vec2(infoA.r + localX, infoA.g + row);\n"
"  float resultA = mcOne(refTexA, bA, infoA.b, infoA.a);\n"
"  vec4 infoB = texture2DRect(blockInfoTexB, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 bB = vec2(infoB.r + localX, infoB.g + row);\n"
"  float resultB = mcOne(refTexB, bB, infoB.b, infoB.a);\n"
"  gl_FragColor = vec4(resultA/255.0, resultB/255.0, 0.0, 1.0);\n"
"}\n";

static double wall_ms(struct timeval *a, struct timeval *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_usec - a->tv_usec) / 1000.0;
}

int main(void) {
    const int FRAME_W = 480, FRAME_H = 480;
    const int NBLOCKS_HALF = 64; /* each group's own block count - matches
                                   * multi-ref-timing-probe's exact shape */

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4096, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    unsigned char *buf0 = (unsigned char*)malloc(FRAME_W*FRAME_H);
    unsigned char *buf1 = (unsigned char*)malloc(FRAME_W*FRAME_H);
    for (int i = 0; i < FRAME_W*FRAME_H; i++) { buf0[i] = (i*37)&0xFF; buf1[i] = (i*53+11)&0xFF; }
    GLuint refTex0, refTex1;
    glGenTextures(1,&refTex0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex0);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_LUMINANCE8,FRAME_W,FRAME_H,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,buf0);
    glGenTextures(1,&refTex1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex1);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_LUMINANCE8,FRAME_W,FRAME_H,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,buf1);
    checkgl("reftex upload");

    /* Two independent per-group block-info tables, matching real content:
     * varied phases exercising the full candidate set, real margin. */
    float *blockinfoA = (float*)malloc(sizeof(float)*NBLOCKS_HALF*4);
    float *blockinfoB = (float*)malloc(sizeof(float)*NBLOCKS_HALF*4);
    for (int i = 0; i < NBLOCKS_HALF; i++) {
        blockinfoA[i*4+0] = (float)(10 + (i % 20) * 16); blockinfoA[i*4+1] = (float)(10 + (i / 20) * 16);
        blockinfoA[i*4+2] = (float)(i % 4); blockinfoA[i*4+3] = (float)((i / 4) % 4);
        blockinfoB[i*4+0] = (float)(20 + (i % 15) * 16); blockinfoB[i*4+1] = (float)(20 + (i / 15) * 16);
        blockinfoB[i*4+2] = (float)((i+1) % 4); blockinfoB[i*4+3] = (float)((i / 3) % 4);
    }
    GLuint blockInfoTexA, blockInfoTexB;
    glGenTextures(1,&blockInfoTexA); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,blockInfoTexA);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,NBLOCKS_HALF,1,0,GL_RGBA,GL_FLOAT,blockinfoA);
    glGenTextures(1,&blockInfoTexB); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,blockInfoTexB);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,NBLOCKS_HALF,1,0,GL_RGBA,GL_FLOAT,blockinfoB);
    checkgl("blockinfo upload");

    int vw_half = NBLOCKS_HALF * 16;
    float *colinfo = (float*)malloc(sizeof(float)*vw_half*4);
    for (int i = 0; i < NBLOCKS_HALF; i++)
        for (int c = 0; c < 16; c++) {
            int idx = i*16+c;
            colinfo[idx*4+0] = (float)i; colinfo[idx*4+1] = (float)c; colinfo[idx*4+2] = 0; colinfo[idx*4+3] = 1;
        }
    GLuint colInfoTex; glGenTextures(1,&colInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,colInfoTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,vw_half,1,0,GL_RGBA,GL_FLOAT,colinfo);
    checkgl("colinfo upload");

    unsigned char *pixels = (unsigned char*)malloc((size_t)vw_half*16*4);

    GLhandleARB progSingle = linkp(vs_plain, fs_mc_batch_var);
    GLhandleARB progOverlay = linkp(vs_plain, fs_mc_batch_overlay);

    const int REPS = 200;

    /* ---- Approach A: two separate single-reference draw+readback round
     * trips - today's real approach, unchanged from multi-ref-timing-probe
     * (re-measured here for a same-run, same-condition comparison). ---- */
    double a_wall = 0;
    for (int rep = 0; rep < REPS; rep++) {
        struct timeval w0, w1;
        gettimeofday(&w0, NULL);
        for (int half = 0; half < 2; half++) {
            glViewport(0, 0, vw_half, 16);
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw_half, 0, 16, -1, 1);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glUseProgramObjectARB(progSingle);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, half==0?refTex0:refTex1);
            glUniform1iARB(glGetUniformLocationARB(progSingle,"refTex"),0);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, half==0?blockInfoTexA:blockInfoTexB);
            glUniform1iARB(glGetUniformLocationARB(progSingle,"blockInfoTex"),1);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
            glUniform1iARB(glGetUniformLocationARB(progSingle,"colInfoTex"),2);
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(vw_half,0);glVertex2f(vw_half,16);glVertex2f(0,16); glEnd();
            glReadPixels(0, 0, vw_half, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        gettimeofday(&w1, NULL);
        a_wall += wall_ms(&w0, &w1);
    }

    /* ---- Approach C (NEW): overlay - one draw, R=groupA, G=groupB,
     * same width/readback size as ONE of today's two dispatches. ---- */
    double c_wall = 0;
    for (int rep = 0; rep < REPS; rep++) {
        struct timeval w0, w1;
        gettimeofday(&w0, NULL);
        glViewport(0, 0, vw_half, 16);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw_half, 0, 16, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glUseProgramObjectARB(progOverlay);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex0);
        glUniform1iARB(glGetUniformLocationARB(progOverlay,"refTexA"),0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex1);
        glUniform1iARB(glGetUniformLocationARB(progOverlay,"refTexB"),1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexA);
        glUniform1iARB(glGetUniformLocationARB(progOverlay,"blockInfoTexA"),2);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexB);
        glUniform1iARB(glGetUniformLocationARB(progOverlay,"blockInfoTexB"),3);
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
        glUniform1iARB(glGetUniformLocationARB(progOverlay,"colInfoTex"),4);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(vw_half,0);glVertex2f(vw_half,16);glVertex2f(0,16); glEnd();
        glReadPixels(0, 0, vw_half, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        gettimeofday(&w1, NULL);
        c_wall += wall_ms(&w0, &w1);
    }
    checkgl("final");

    /* This is a timing-only decision probe, not a correctness verification -
     * mcOne() is the same formula already verified in fs_mc_batch_var/the
     * previous multi-ref-timing-probe's fs_mc_batch_2ref, called twice with
     * different textures/metadata, not a new derivation. If this shows a
     * real win, the full production version would need its own real
     * correctness pass (real captured data, CPU cross-check) before landing,
     * same as every other change this session - not worth building that
     * here if the timing verdict below is negative. */
    printf("=== multi-ref-overlay-probe: %d blocks/group, 2 groups ===\n", NBLOCKS_HALF);
    printf("A) two separate single-ref draw+readback round trips: %.2fms/rep avg (%d reps)\n", a_wall/REPS, REPS);
    printf("C) ONE overlay draw+readback (R=groupA, G=groupB):    %.2fms/rep avg (%d reps)\n", c_wall/REPS, REPS);
    if (c_wall < a_wall)
        printf("\n-> OVERLAY IS FASTER (%.1f%% less time) - worth building the full production version.\n",
               100.0*(a_wall-c_wall)/a_wall);
    else
        printf("\n-> OVERLAY IS SLOWER (%.1f%% more time) - the doubled per-fragment cost still outweighs\n"
               "   the saved round trip, even without the earlier approach's wasted-selection and\n"
               "   doubled-readback-size penalties. Do NOT build the full production version.\n",
               100.0*(c_wall-a_wall)/a_wall);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
