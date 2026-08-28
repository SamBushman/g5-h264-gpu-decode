/*
 * multi-ref-timing-probe: before building a full multi-reference-per-draw
 * MC dispatch (the idea flagged after confirming dispatch_singlepass_group
 * is at a real GL_MAX_TEXTURE_SIZE dispatch ceiling, not an artificial
 * one), measure directly whether it would actually be a net win on real
 * hardware. GLSL 110 on this driver (quirk #7) has no dynamic sampler
 * indexing - the ONLY way to conditionally use one of several bound
 * reference textures is the same "compute every candidate unconditionally,
 * select after" pattern already used for quarter-pel phase selection
 * (never branch AROUND a texture2DRect call, quirk #2). The real
 * production shader (fs_mc_batch_var) does 27 texture2DRect calls per
 * output fragment (3 full-pel + 4x6-tap half-pel candidates); merging K
 * reference groups into one draw call would need all 27 candidates
 * computed once PER reference (27*K total), to let a real driver decide
 * whether that per-fragment cost increase is actually cheaper than K
 * separate dispatches' fixed per-round-trip overhead - or worse, as the
 * already-reverted MC+IDCT merge attempt found for a similar trade.
 *
 * Directly compares, for the SAME total real batch of blocks split across
 * 2 distinct reference frames (matching a realistic ~50/50 split, chosen
 * as the cheapest, most favorable-to-merging case - real content skews
 * toward MORE groups per call per the measured distribution, which would
 * only make the per-fragment multiplication worse):
 *   A) TODAY's real approach: two separate single-reference draw+readback
 *      round trips (the real fs_mc_batch_var shader, unmodified).
 *   B) The proposed merge: ONE draw+readback round trip using a 2-reference
 *      variant that samples both textures for every fragment and selects
 *      per-block - same total fragment count and total data as (A).
 */

#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Real production shader, copied verbatim (single reference). */
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

/* Merged 2-reference variant: same 27-candidate computation done TWICE
 * (once per bound ref texture), selecting both the phase-candidate AND
 * which reference to read from, per block - via blockInfoTex's alpha-free
 * 5th value packed into colInfoTex's .b channel (0=ref0, 1=ref1), the
 * cheapest available slot without adding a whole new texture for this
 * probe. Real production code would need a cleaner encoding; this probe
 * only needs to be honest about the real fetch/select cost. */
static const char *fs_mc_batch_2ref =
"uniform sampler2DRect refTex0;\n"
"uniform sampler2DRect refTex1;\n"
"uniform sampler2DRect blockInfoTex;\n"
"uniform sampler2DRect colInfoTex;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float halfH0f(sampler2DRect t, vec2 b) {\n"
"  float a2=texture2DRect(t,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(t,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(t,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(t,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(t,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(t,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfV0f(sampler2DRect t, vec2 b) {\n"
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
"  float halfH0 = halfH0f(t, b);\n"
"  float halfH1 = halfH0f(t, b+vec2(0.0,1.0));\n"
"  float halfV0 = halfV0f(t, b);\n"
"  float halfV1 = halfV0f(t, b+vec2(1.0,0.0));\n"
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
"  float refSel = colInfo.b;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 b = vec2(info.r + localX, info.g + row);\n"
"  float hPhase = info.b, vPhase = info.a;\n"
"  float result0 = mcOne(refTex0, b, hPhase, vPhase);\n"
"  float result1 = mcOne(refTex1, b, hPhase, vPhase);\n"
"  float result = (refSel < 0.5) ? result0 : result1;\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

static double wall_ms(struct timeval *a, struct timeval *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_usec - a->tv_usec) / 1000.0;
}

int main(void) {
    const int FRAME_W = 480, FRAME_H = 480;
    const int NBLOCKS_TOTAL = 128;   /* 128 blocks * 16px = 2048px wide, a realistic batch */
    const int NBLOCKS_HALF = 64;     /* split 50/50 across 2 refs - the cheapest, most
                                       * favorable-to-merging real case */

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4096, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    /* Two synthetic reference frames, real dimensions, GL_LUMINANCE8 -
     * matching production's reftex format exactly. */
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

    /* blockInfoTex: NBLOCKS_TOTAL entries (shared layout for both tests -
     * safe positions with real margin, varied phases exercising the full
     * candidate set, not just one path). colInfoTex varies per test. */
    float *blockinfo = (float*)malloc(sizeof(float)*NBLOCKS_TOTAL*4);
    for (int i = 0; i < NBLOCKS_TOTAL; i++) {
        blockinfo[i*4+0] = (float)(10 + (i % 20) * 16); /* pel_x, safe margin */
        blockinfo[i*4+1] = (float)(10 + (i / 20) * 16); /* pel_y */
        blockinfo[i*4+2] = (float)(i % 4);              /* hPhase 0..3 */
        blockinfo[i*4+3] = (float)((i / 4) % 4);        /* vPhase 0..3 */
    }
    GLuint blockInfoTex; glGenTextures(1,&blockInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,blockInfoTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,NBLOCKS_TOTAL,1,0,GL_RGBA,GL_FLOAT,blockinfo);
    checkgl("blockinfo upload");

    int vw = NBLOCKS_TOTAL * 16;
    float *colinfo_merged = (float*)malloc(sizeof(float)*vw*4);
    float *colinfo_half = (float*)malloc(sizeof(float)*vw*4); /* reused for both halves */
    for (int i = 0; i < NBLOCKS_TOTAL; i++)
        for (int c = 0; c < 16; c++) {
            int idx = i*16+c;
            colinfo_merged[idx*4+0] = (float)i; colinfo_merged[idx*4+1] = (float)c;
            colinfo_merged[idx*4+2] = (i < NBLOCKS_HALF) ? 0.0f : 1.0f; /* which ref */
            colinfo_merged[idx*4+3] = 1;
        }
    GLuint colInfoTexMerged; glGenTextures(1,&colInfoTexMerged); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,colInfoTexMerged);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,vw,1,0,GL_RGBA,GL_FLOAT,colinfo_merged);
    checkgl("colinfo merged upload");

    /* Half-batch colInfoTex (for the "2 separate draws" baseline): local
     * blockIdx 0..NBLOCKS_HALF-1 within each half's own draw. */
    int vw_half = NBLOCKS_HALF * 16;
    GLuint colInfoTexHalf; glGenTextures(1,&colInfoTexHalf); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,colInfoTexHalf);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);

    unsigned char *pixels = (unsigned char*)malloc((size_t)vw*16*4);

    GLhandleARB progSingle = linkp(vs_plain, fs_mc_batch_var);
    GLhandleARB progMerged = linkp(vs_plain, fs_mc_batch_2ref);

    const int REPS = 200;

    /* ---- Approach A: two separate single-reference draw+readback round
     * trips, matching TODAY's real dispatch pattern exactly. ---- */
    double a_wall = 0;
    for (int rep = 0; rep < REPS; rep++) {
        struct timeval w0, w1;
        gettimeofday(&w0, NULL);
        for (int half = 0; half < 2; half++) {
            for (int i = 0; i < NBLOCKS_HALF; i++)
                for (int c = 0; c < 16; c++) {
                    int idx = i*16+c;
                    colinfo_half[idx*4+0] = (float)(half*NBLOCKS_HALF + i);
                    colinfo_half[idx*4+1] = (float)c;
                    colinfo_half[idx*4+2] = 0; colinfo_half[idx*4+3] = 1;
                }
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTexHalf);
            glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,vw_half,1,0,GL_RGBA,GL_FLOAT,colinfo_half);
            glViewport(0, 0, vw_half, 16);
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw_half, 0, 16, -1, 1);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glUseProgramObjectARB(progSingle);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, half==0?refTex0:refTex1);
            glUniform1iARB(glGetUniformLocationARB(progSingle,"refTex"),0);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
            glUniform1iARB(glGetUniformLocationARB(progSingle,"blockInfoTex"),1);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTexHalf);
            glUniform1iARB(glGetUniformLocationARB(progSingle,"colInfoTex"),2);
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(vw_half,0);glVertex2f(vw_half,16);glVertex2f(0,16); glEnd();
            glReadPixels(0, 0, vw_half, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        gettimeofday(&w1, NULL);
        a_wall += wall_ms(&w0, &w1);
    }

    /* ---- Approach B: one merged 2-reference draw+readback round trip,
     * same total blocks/fragments/data. ---- */
    double b_wall = 0;
    for (int rep = 0; rep < REPS; rep++) {
        struct timeval w0, w1;
        gettimeofday(&w0, NULL);
        glViewport(0, 0, vw, 16);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw, 0, 16, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glUseProgramObjectARB(progMerged);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex0);
        glUniform1iARB(glGetUniformLocationARB(progMerged,"refTex0"),0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex1);
        glUniform1iARB(glGetUniformLocationARB(progMerged,"refTex1"),1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
        glUniform1iARB(glGetUniformLocationARB(progMerged,"blockInfoTex"),2);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTexMerged);
        glUniform1iARB(glGetUniformLocationARB(progMerged,"colInfoTex"),3);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(vw,0);glVertex2f(vw,16);glVertex2f(0,16); glEnd();
        glReadPixels(0, 0, vw, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        gettimeofday(&w1, NULL);
        b_wall += wall_ms(&w0, &w1);
    }

    checkgl("final");
    printf("=== multi-ref-timing-probe: %d blocks total, split 2x%d ===\n", NBLOCKS_TOTAL, NBLOCKS_HALF);
    printf("A) two separate single-ref draw+readback round trips: %.2fms/rep avg (%d reps)\n", a_wall/REPS, REPS);
    printf("B) one merged 2-ref draw+readback round trip:         %.2fms/rep avg (%d reps)\n", b_wall/REPS, REPS);
    if (b_wall < a_wall)
        printf("\n-> MERGED IS FASTER (%.1f%% less time) - worth building the full production version.\n",
               100.0*(a_wall-b_wall)/a_wall);
    else
        printf("\n-> MERGED IS SLOWER (%.1f%% more time) - the per-fragment 2x cost outweighs the saved\n"
               "   round trip. Do NOT build the full production version; this confirms the same failure\n"
               "   mode as the already-reverted MC+IDCT merge attempt.\n",
               100.0*(b_wall-a_wall)/a_wall);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
