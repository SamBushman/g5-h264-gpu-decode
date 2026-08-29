/*
 * channel-pack-pipeline-probe: the real timing question the user asked
 * for - does "K cheap GPU-side copy passes (assembling a channel-packed
 * combined reference texture via glColorMask, confirmed correct in
 * channel-mask-probe) + ONE combined MC dispatch" beat today's "K
 * separate full MC dispatches", once the copy-pass overhead is counted?
 *
 * Real complication checked FIRST, before building the full pipeline:
 * copying a reference frame's full width (480px, matching this project's
 * real content) into the combined texture means rendering INTO an
 * FBO-attached texture at 480px - and this project has a documented
 * quirk (#16, found integrating the now-removed diagonal-MC two-pass
 * shader): FBO-attached rendering has its own ~256px width ceiling,
 * independent of GL_MAX_TEXTURE_SIZE, plus a separate repeated-resize
 * corruption bug. If that ceiling applies here too, the whole approach
 * is blocked at realistic scale regardless of timing - worth confirming
 * or ruling out before spending more effort on the full comparison.
 *
 * Same 128-block-total, 2x64-split scenario used throughout this
 * thread's earlier probes, for direct comparability.
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

static double wall_ms(struct timeval *a, struct timeval *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_usec - a->tv_usec) / 1000.0;
}

/* ---- Real production MC formula, reused verbatim throughout this thread ---- */
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

static const char *fs_mc_single =
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"
"uniform sampler2DRect colInfoTex;\n";
static const char *fs_mc_single_tail =
"void main(){\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float row = floor(gl_FragCoord.y);\n"
"  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));\n"
"  float blockIdx = colInfo.r;\n"
"  float localX = colInfo.g;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 b = vec2(info.r + localX, info.g + row);\n"
"  float result = mcOne(refTex, b, info.b, info.a);\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

/* Channel-packed dual dispatch: samples the ALREADY-ASSEMBLED combined
 * texture ONCE per tap position (getting both refs' values via .r/.g),
 * runs the MC formula twice (once per channel), packs both results into
 * R/G of the output - same output shape as multi-ref-overlay-probe's
 * fs_mc_batch_overlay, but reading ONE shared texture instead of two
 * separately bound ones. */
static const char *fs_mc_packed_tail =
"void main(){\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float row = floor(gl_FragCoord.y);\n"
"  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));\n"
"  float blockIdx = colInfo.r;\n"
"  float localX = colInfo.g;\n"
"  vec4 infoA = texture2DRect(blockInfoTexA, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 bA = vec2(infoA.r + localX, infoA.g + row);\n"
"  vec4 infoB = texture2DRect(blockInfoTexB, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 bB = vec2(infoB.r + localX, infoB.g + row);\n"
"  float resultA = mcOne(packedTex, bA, infoA.b, infoA.a);\n"
"  float resultB = mcOne(packedTex, bB, infoB.b, infoB.a);\n"
"  gl_FragColor = vec4(resultA/255.0, resultB/255.0, 0.0, 1.0);\n"
"}\n";

static const char *fs_copy_r =
"uniform sampler2DRect srcTex;\n"
"void main(){ gl_FragColor = vec4(texture2DRect(srcTex, floor(gl_FragCoord.xy)+vec2(0.5,0.5)).r, 1.0, 1.0, 1.0); }\n";
static const char *fs_copy_g =
"uniform sampler2DRect srcTex;\n"
"void main(){ gl_FragColor = vec4(1.0, texture2DRect(srcTex, floor(gl_FragCoord.xy)+vec2(0.5,0.5)).r, 1.0, 1.0); }\n";

static char *build(const char *head, const char *body) {
    static char buf[16384];
    snprintf(buf, sizeof(buf), "%s%s%s", head, mc_unit_glsl, body);
    return buf;
}

int main(void) {
    const int FRAME_W = 480, FRAME_H = 480; /* real production reference-frame size */
    const int NBLOCKS_HALF = 64;

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4096, 480, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    /* ---- Step 0: does an FBO-attached copy pass survive full 480px
     * width, or does quirk #16's ~256px ceiling block this outright? ---- */
    printf("=== Step 0: FBO copy-pass correctness check at real frame width (%dpx) ===\n", FRAME_W);
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

    GLuint packedTex; glGenTextures(1,&packedTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,packedTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA8,FRAME_W,FRAME_H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    GLuint fbo; glGenFramebuffersEXT(1,&fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,packedTex,0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT) { fprintf(stderr, "FBO incomplete: 0x%x\n", status); return 1; }

    GLhandleARB progCopyR = linkp(vs_plain, fs_copy_r);
    GLhandleARB progCopyG = linkp(vs_plain, fs_copy_g);

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glViewport(0, 0, FRAME_W, FRAME_H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, FRAME_W, 0, FRAME_H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
    glUseProgramObjectARB(progCopyR);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex0);
    glUniform1iARB(glGetUniformLocationARB(progCopyR,"srcTex"),0);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(FRAME_W,0);glVertex2f(FRAME_W,FRAME_H);glVertex2f(0,FRAME_H); glEnd();
    checkgl("copy R pass (480px)");

    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
    glUseProgramObjectARB(progCopyG);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex1);
    glUniform1iARB(glGetUniformLocationARB(progCopyG,"srcTex"),0);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(FRAME_W,0);glVertex2f(FRAME_W,FRAME_H);glVertex2f(0,FRAME_H); glEnd();
    checkgl("copy G pass (480px)");
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    /* Safe readback: sample into default framebuffer, then glReadPixels. */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    GLhandleARB progPass = linkp(vs_plain,
        "uniform sampler2DRect srcTex;\n"
        "void main(){ gl_FragColor = texture2DRect(srcTex, floor(gl_FragCoord.xy)+vec2(0.5,0.5)); }\n");
    glUseProgramObjectARB(progPass);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, packedTex);
    glUniform1iARB(glGetUniformLocationARB(progPass,"srcTex"),0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(FRAME_W,0);glVertex2f(FRAME_W,FRAME_H);glVertex2f(0,FRAME_H); glEnd();
    glFinish(); checkgl("passthrough readback");

    unsigned char *checkpix = (unsigned char*)malloc((size_t)FRAME_W*FRAME_H*4);
    glReadPixels(0, 0, FRAME_W, FRAME_H, GL_RGBA, GL_UNSIGNED_BYTE, checkpix);
    int rmis = 0, gmis = 0;
    for (int y = 0; y < FRAME_H; y++) for (int x = 0; x < FRAME_W; x++) {
        int i = y*FRAME_W+x;
        if (checkpix[i*4+0] != buf0[i]) rmis++;
        if (checkpix[i*4+1] != buf1[i]) gmis++;
    }
    printf("  R channel (from refTex0): %d/%d wrong\n", rmis, FRAME_W*FRAME_H);
    printf("  G channel (from refTex1): %d/%d wrong\n", gmis, FRAME_W*FRAME_H);
    int copy_ok = (rmis == 0 && gmis == 0);
    if (!copy_ok) {
        printf("\n-> FBO copy-pass FAILS at full 480px width - quirk #16's width ceiling (or similar) blocks\n"
               "   this approach at realistic scale. Stopping here - no point measuring timing for a\n"
               "   mechanism that doesn't work correctly at production size.\n");
        return 1;
    }
    printf("  -> Full-width (480px) FBO copy pass is CORRECT - no width-ceiling problem here.\n");

    /* ---- Real timing comparison: today's baseline vs. the full
     * copy+combined-dispatch pipeline, same 128-block/2x64 scenario used
     * throughout this thread. ---- */
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

    char single_src[16384];
    snprintf(single_src, sizeof(single_src), "%s%s%s", fs_mc_single, mc_unit_glsl, fs_mc_single_tail);
    GLhandleARB progSingle = linkp(vs_plain, single_src);

    char packed_head[2048];
    snprintf(packed_head, sizeof(packed_head),
        "uniform sampler2DRect packedTex;\n"
        "uniform sampler2DRect blockInfoTexA;\n"
        "uniform sampler2DRect blockInfoTexB;\n"
        "uniform sampler2DRect colInfoTex;\n");
    char packed_src[16384];
    snprintf(packed_src, sizeof(packed_src), "%s%s%s", packed_head, mc_unit_glsl, fs_mc_packed_tail);
    GLhandleARB progPacked = linkp(vs_plain, packed_src);

    const int REPS = 200;

    /* Approach A: today's baseline, 2 separate single-ref dispatches. */
    double a_wall = 0;
    for (int rep = 0; rep < REPS; rep++) {
        struct timeval w0, w1;
        gettimeofday(&w0, NULL);
        for (int half = 0; half < 2; half++) {
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
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

    /* Approach D: 2 copy passes (full 480px frame, color-masked) + 1
     * combined MC dispatch reading the packed texture. */
    double d_wall = 0;
    for (int rep = 0; rep < REPS; rep++) {
        struct timeval w0, w1;
        gettimeofday(&w0, NULL);

        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glViewport(0, 0, FRAME_W, FRAME_H);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, FRAME_W, 0, FRAME_H, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
        glUseProgramObjectARB(progCopyR);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex0);
        glUniform1iARB(glGetUniformLocationARB(progCopyR,"srcTex"),0);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(FRAME_W,0);glVertex2f(FRAME_W,FRAME_H);glVertex2f(0,FRAME_H); glEnd();
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
        glUseProgramObjectARB(progCopyG);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex1);
        glUniform1iARB(glGetUniformLocationARB(progCopyG,"srcTex"),0);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(FRAME_W,0);glVertex2f(FRAME_W,FRAME_H);glVertex2f(0,FRAME_H); glEnd();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glViewport(0, 0, vw_half, 16);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw_half, 0, 16, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glUseProgramObjectARB(progPacked);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, packedTex);
        glUniform1iARB(glGetUniformLocationARB(progPacked,"packedTex"),0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexA);
        glUniform1iARB(glGetUniformLocationARB(progPacked,"blockInfoTexA"),1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexB);
        glUniform1iARB(glGetUniformLocationARB(progPacked,"blockInfoTexB"),2);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
        glUniform1iARB(glGetUniformLocationARB(progPacked,"colInfoTex"),3);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(vw_half,0);glVertex2f(vw_half,16);glVertex2f(0,16); glEnd();
        glReadPixels(0, 0, vw_half, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        gettimeofday(&w1, NULL);
        d_wall += wall_ms(&w0, &w1);
    }
    checkgl("final");

    printf("\n=== Timing: today's baseline vs. channel-packed pipeline (copy passes + combined dispatch) ===\n");
    printf("A) two separate single-ref draw+readback round trips: %.2fms/rep avg (%d reps)\n", a_wall/REPS, REPS);
    printf("D) 2 copy passes (480px, color-masked) + 1 combined MC dispatch: %.2fms/rep avg (%d reps)\n", d_wall/REPS, REPS);
    if (d_wall < a_wall)
        printf("\n-> CHANNEL-PACKED PIPELINE IS FASTER (%.1f%% less time) - worth pursuing a real production\n"
               "   implementation, including reference-cache bookkeeping for which pairs are already packed.\n",
               100.0*(a_wall-d_wall)/a_wall);
    else
        printf("\n-> CHANNEL-PACKED PIPELINE IS SLOWER (%.1f%% more time) - the copy-pass overhead (2 full-\n"
               "   frame-width draw+state-change round trips) outweighs what the combined dispatch saves.\n"
               "   Do NOT pursue a full production implementation of this approach.\n",
               100.0*(d_wall-a_wall)/a_wall);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
