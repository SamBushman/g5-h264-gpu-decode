/*
 * channel-pack-scoped-probe: follow-up to channel-pack-pipeline-probe,
 * which found the full-frame-copy version of the channel-packed pipeline
 * 17.2% SLOWER than today's baseline, with a clear, understood cause -
 * the copy passes touched the entire 480x480 frame (230,400 fragments)
 * to prepare data a batch only needs across a small fraction of that
 * area. This tests the natural fix: restrict each copy pass's viewport
 * to the real bounding box of that group's own block positions (plus the
 * 6-tap filter's margin), computed from the actual block data, instead
 * of copying the whole frame.
 *
 * Mechanism: glViewport(x,y,w,h) sets both the size AND the window-space
 * OFFSET of where rendering lands - gl_FragCoord.xy in the fragment
 * shader always reports true framebuffer pixel coordinates (not
 * viewport-local ones), so a copy shader reading
 * texture2DRect(srcTex, gl_FragCoord.xy) and writing into the SAME
 * packedTex, run with a viewport restricted to a group's bounding box,
 * correctly writes only that region - no coordinate remapping needed in
 * the shader itself.
 *
 * Two things verified, not just timed: (1) real correctness - the scoped
 * pipeline's MC output must exactly match the baseline's, not just "no
 * GL errors" (a scoped copy that misses part of the real read footprint
 * would silently sample stale/uninitialized packedTex data outside the
 * copied region); (2) real timing against the same baseline used
 * throughout this thread.
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

static const char *fs_mc_single_head =
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
/* Real bug found via the diagnostic dump below: mcOne()/halfHt()/halfVt()
 * always sample `.r` - fine when each reference has its OWN texture, but
 * WRONG for group B once both references share packedTex, since group
 * B's data lives in the G channel, not R. Needs its own G-reading
 * variant, not just a different position. */
static const char *mc_unit_g_glsl =
"float halfHtG(sampler2DRect t, vec2 b) {\n"
"  float a2=texture2DRect(t,b+vec2(-2.0,0.0)).g*255.0;\n"
"  float a1=texture2DRect(t,b+vec2(-1.0,0.0)).g*255.0;\n"
"  float a0=texture2DRect(t,b+vec2( 0.0,0.0)).g*255.0;\n"
"  float a3=texture2DRect(t,b+vec2( 1.0,0.0)).g*255.0;\n"
"  float a4=texture2DRect(t,b+vec2( 2.0,0.0)).g*255.0;\n"
"  float a5=texture2DRect(t,b+vec2( 3.0,0.0)).g*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfVtG(sampler2DRect t, vec2 b) {\n"
"  float a2=texture2DRect(t,b+vec2(0.0,-2.0)).g*255.0;\n"
"  float a1=texture2DRect(t,b+vec2(0.0,-1.0)).g*255.0;\n"
"  float a0=texture2DRect(t,b+vec2(0.0, 0.0)).g*255.0;\n"
"  float a3=texture2DRect(t,b+vec2(0.0, 1.0)).g*255.0;\n"
"  float a4=texture2DRect(t,b+vec2(0.0, 2.0)).g*255.0;\n"
"  float a5=texture2DRect(t,b+vec2(0.0, 3.0)).g*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float mcOneG(sampler2DRect t, vec2 b, float hPhase, float vPhase) {\n"
"  float full00 = texture2DRect(t, b).g*255.0;\n"
"  float full10 = texture2DRect(t, b+vec2(1.0,0.0)).g*255.0;\n"
"  float full01 = texture2DRect(t, b+vec2(0.0,1.0)).g*255.0;\n"
"  float halfH0 = halfHtG(t, b);\n"
"  float halfH1 = halfHtG(t, b+vec2(0.0,1.0));\n"
"  float halfV0 = halfVtG(t, b);\n"
"  float halfV1 = halfVtG(t, b+vec2(1.0,0.0));\n"
"  float result = full00;\n"
"  if (hPhase == 0.0 && vPhase == 0.0) { result = full00; }\n"
"  else if (hPhase == 2.0 && vPhase == 0.0) { result = halfH0; }\n"
"  else if (hPhase == 0.0 && vPhase == 2.0) { result = halfV0; }\n"
"  else if (vPhase == 0.0) { float fullOp = full00; if (hPhase == 3.0) fullOp = full10; result = floor((fullOp + halfH0 + 1.0) / 2.0); }\n"
"  else if (hPhase == 0.0) { float fullOp = full00; if (vPhase == 3.0) fullOp = full01; result = floor((fullOp + halfV0 + 1.0) / 2.0); }\n"
"  else { float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1; float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1; result = floor((hOp + vOp + 1.0) / 2.0); }\n"
"  return result;\n"
"}\n";
static const char *fs_mc_packed_head =
"uniform sampler2DRect packedTex;\n"
"uniform sampler2DRect blockInfoTexA;\n"
"uniform sampler2DRect blockInfoTexB;\n"
"uniform sampler2DRect colInfoTex;\n";
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
"  float resultB = mcOneG(packedTex, bB, infoB.b, infoB.a);\n"
"  gl_FragColor = vec4(resultA/255.0, resultB/255.0, 0.0, 1.0);\n"
"}\n";
static const char *fs_copy_r =
"uniform sampler2DRect srcTex;\n"
"void main(){ gl_FragColor = vec4(texture2DRect(srcTex, gl_FragCoord.xy).r, 1.0, 1.0, 1.0); }\n";
static const char *fs_copy_g =
"uniform sampler2DRect srcTex;\n"
"void main(){ gl_FragColor = vec4(1.0, texture2DRect(srcTex, gl_FragCoord.xy).r, 1.0, 1.0); }\n";

int main(void) {
    const int FRAME_W = 480, FRAME_H = 480;
    const int NBLOCKS_HALF = 64;
    const int PAD = 3; /* matches this project's established margin for the 6-tap+full-pel footprint */

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4096, 480, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
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

    float *blockinfoA = (float*)malloc(sizeof(float)*NBLOCKS_HALF*4);
    float *blockinfoB = (float*)malloc(sizeof(float)*NBLOCKS_HALF*4);
    int axmin=1<<30, axmax=-(1<<30), aymin=1<<30, aymax=-(1<<30);
    int bxmin=1<<30, bxmax=-(1<<30), bymin=1<<30, bymax=-(1<<30);
    for (int i = 0; i < NBLOCKS_HALF; i++) {
        int axp = 10 + (i % 20) * 16, ayp = 10 + (i / 20) * 16;
        int bxp = 20 + (i % 15) * 16, byp = 20 + (i / 15) * 16;
        blockinfoA[i*4+0] = (float)axp; blockinfoA[i*4+1] = (float)ayp;
        blockinfoA[i*4+2] = (float)(i % 4); blockinfoA[i*4+3] = (float)((i / 4) % 4);
        blockinfoB[i*4+0] = (float)bxp; blockinfoB[i*4+1] = (float)byp;
        blockinfoB[i*4+2] = (float)((i+1) % 4); blockinfoB[i*4+3] = (float)((i / 3) % 4);
        if (axp < axmin) axmin = axp; if (axp+16 > axmax) axmax = axp+16;
        if (ayp < aymin) aymin = ayp; if (ayp+16 > aymax) aymax = ayp+16;
        if (bxp < bxmin) bxmin = bxp; if (bxp+16 > bxmax) bxmax = bxp+16;
        if (byp < bymin) bymin = byp; if (byp+16 > bymax) bymax = byp+16;
    }
    /* Real read footprint per block extends PAD beyond the raw block
     * extent on every side (6-tap filter + full-pel "+1" reads). */
    int aX = axmin - PAD, aY = aymin - PAD, aW = (axmax + PAD) - aX, aH = (aymax + PAD) - aY;
    int bX = bxmin - PAD, bY = bymin - PAD, bW = (bxmax + PAD) - bX, bH = (bymax + PAD) - bY;
    printf("=== Group A bbox: (%d,%d) %dx%d (%d fragments) ===\n", aX, aY, aW, aH, aW*aH);
    printf("=== Group B bbox: (%d,%d) %dx%d (%d fragments) ===\n", bX, bY, bW, bH, bW*bH);
    printf("=== Combined scoped copy fragments: %d, vs full-frame 2x%d=%d (%.1fx reduction) ===\n",
           aW*aH+bW*bH, FRAME_W*FRAME_H, 2*FRAME_W*FRAME_H,
           (double)(2*FRAME_W*FRAME_H)/(aW*aH+bW*bH));

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
    char single_src[16384]; snprintf(single_src, sizeof(single_src), "%s%s%s", fs_mc_single_head, mc_unit_glsl, fs_mc_single_tail);
    GLhandleARB progSingle = linkp(vs_plain, single_src);
    char packed_src[16384]; snprintf(packed_src, sizeof(packed_src), "%s%s%s%s", fs_mc_packed_head, mc_unit_glsl, mc_unit_g_glsl, fs_mc_packed_tail);
    GLhandleARB progPacked = linkp(vs_plain, packed_src);

    unsigned char *pixels = (unsigned char*)malloc((size_t)vw_half*16*4);
    unsigned char *baseline_out = (unsigned char*)malloc((size_t)vw_half*16*4*2); /* both halves, R channel only kept */

    /* ---- Run baseline (today's approach) once to capture ground truth ---- */
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
        glFinish();
        glReadPixels(0, 0, vw_half, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        for (int i = 0; i < vw_half*16; i++) baseline_out[half*vw_half*16 + i] = pixels[i*4];
    }
    checkgl("baseline capture");

    /* ---- Correctness check: scoped-copy pipeline vs. baseline ---- */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
    glUseProgramObjectARB(progCopyR);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex0);
    glUniform1iARB(glGetUniformLocationARB(progCopyR,"srcTex"),0);
    glViewport(aX, aY, aW, aH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(aX, aX+aW, aY, aY+aH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBegin(GL_QUADS); glVertex2f(aX,aY);glVertex2f(aX+aW,aY);glVertex2f(aX+aW,aY+aH);glVertex2f(aX,aY+aH); glEnd();
    checkgl("scoped copy R");

    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
    glUseProgramObjectARB(progCopyG);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex1);
    glUniform1iARB(glGetUniformLocationARB(progCopyG,"srcTex"),0);
    glViewport(bX, bY, bW, bH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(bX, bX+bW, bY, bY+bH, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBegin(GL_QUADS); glVertex2f(bX,bY);glVertex2f(bX+bW,bY);glVertex2f(bX+bW,bY+bH);glVertex2f(bX,bY+bH); glEnd();
    checkgl("scoped copy G");
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    /* DIAGNOSTIC: dump the packed texture's full RGBA content (safe
     * sample-then-readback pattern) to check whether the G channel copy
     * actually wrote correct data, before blaming the MC dispatch step. */
    {
        GLhandleARB progDump = linkp(vs_plain,
            "uniform sampler2DRect srcTex;\n"
            "void main(){ gl_FragColor = texture2DRect(srcTex, gl_FragCoord.xy); }\n");
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glViewport(0, 0, FRAME_W, FRAME_H);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, FRAME_W, 0, FRAME_H, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glUseProgramObjectARB(progDump);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, packedTex);
        glUniform1iARB(glGetUniformLocationARB(progDump,"srcTex"),0);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(FRAME_W,0);glVertex2f(FRAME_W,FRAME_H);glVertex2f(0,FRAME_H); glEnd();
        glFinish(); checkgl("diagnostic dump draw");
        unsigned char *dump = (unsigned char*)malloc((size_t)FRAME_W*FRAME_H*4);
        glReadPixels(0, 0, FRAME_W, FRAME_H, GL_RGBA, GL_UNSIGNED_BYTE, dump);
        int gwrong = 0, gchecked = 0;
        int firstx=-1, firsty=-1, firstgot=0, firstwant=0;
        for (int y = bY; y < bY+bH && y < FRAME_H; y++) {
            for (int x = bX; x < bX+bW && x < FRAME_W; x++) {
                int i = y*FRAME_W+x;
                int want = buf1[i];
                int got = dump[i*4+1];
                gchecked++;
                if (got != want) { gwrong++; if (firstx<0){firstx=x;firsty=y;firstgot=got;firstwant=want;} }
            }
        }
        printf("DIAGNOSTIC: packedTex G channel within group B's bbox: %d/%d wrong", gwrong, gchecked);
        if (gwrong > 0) printf(" (first bad @ (%d,%d): got=%d want=%d)", firstx, firsty, firstgot, firstwant);
        printf("\n");
        int rwrong=0, rchecked=0;
        for (int y = aY; y < aY+aH && y < FRAME_H; y++) {
            for (int x = aX; x < aX+aW && x < FRAME_W; x++) {
                int i = y*FRAME_W+x;
                rchecked++;
                if (dump[i*4+0] != buf0[i]) rwrong++;
            }
        }
        printf("DIAGNOSTIC: packedTex R channel within group A's bbox: %d/%d wrong\n", rwrong, rchecked);
        free(dump);
    }

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
    glFinish(); checkgl("scoped packed dispatch");
    glReadPixels(0, 0, vw_half, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    int mismA = 0, mismB = 0;
    for (int i = 0; i < vw_half*16; i++) {
        if (pixels[i*4+0] != baseline_out[i]) mismA++;
        if (pixels[i*4+1] != baseline_out[vw_half*16 + i]) mismB++;
    }
    printf("\n=== Correctness: scoped-copy pipeline vs. baseline (two separate dispatches) ===\n");
    printf("  group A (R channel): %d/%d wrong\n", mismA, vw_half*16);
    printf("  group B (G channel): %d/%d wrong\n", mismB, vw_half*16);
    int correct = (mismA == 0 && mismB == 0);
    if (!correct) {
        printf("\n-> INCORRECT - scoped copy region doesn't cover the real read footprint, or another bug.\n"
               "   Stopping here - no point timing a pipeline that produces wrong output.\n");
        return 1;
    }
    printf("  -> CORRECT - scoped bounding-box copy produces byte-identical output to the baseline.\n");

    /* ---- Timing: baseline vs. scoped-copy pipeline ---- */
    const int REPS = 200;
    double a_wall = 0, e_wall = 0;
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
    for (int rep = 0; rep < REPS; rep++) {
        struct timeval w0, w1;
        gettimeofday(&w0, NULL);

        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
        glUseProgramObjectARB(progCopyR);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex0);
        glUniform1iARB(glGetUniformLocationARB(progCopyR,"srcTex"),0);
        glViewport(aX, aY, aW, aH);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(aX, aX+aW, aY, aY+aH, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glBegin(GL_QUADS); glVertex2f(aX,aY);glVertex2f(aX+aW,aY);glVertex2f(aX+aW,aY+aH);glVertex2f(aX,aY+aH); glEnd();

        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
        glUseProgramObjectARB(progCopyG);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex1);
        glUniform1iARB(glGetUniformLocationARB(progCopyG,"srcTex"),0);
        glViewport(bX, bY, bW, bH);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(bX, bX+bW, bY, bY+bH, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glBegin(GL_QUADS); glVertex2f(bX,bY);glVertex2f(bX+bW,bY);glVertex2f(bX+bW,bY+bH);glVertex2f(bX,bY+bH); glEnd();
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
        e_wall += wall_ms(&w0, &w1);
    }
    checkgl("final");

    printf("\n=== Timing: baseline vs. SCOPED-copy channel-packed pipeline ===\n");
    printf("A) two separate single-ref draw+readback round trips: %.2fms/rep avg (%d reps)\n", a_wall/REPS, REPS);
    printf("E) scoped-bbox copy passes + 1 combined MC dispatch:   %.2fms/rep avg (%d reps)\n", e_wall/REPS, REPS);
    if (e_wall < a_wall)
        printf("\n-> SCOPED CHANNEL-PACKED PIPELINE IS FASTER (%.1f%% less time) - the bounding-box restriction\n"
               "   closed the gap. Worth considering a real production implementation, with real bookkeeping\n"
               "   for per-call bounding-box computation and reference-cache coordination.\n",
               100.0*(a_wall-e_wall)/a_wall);
    else
        printf("\n-> SCOPED CHANNEL-PACKED PIPELINE IS STILL SLOWER (%.1f%% more time) - even with the\n"
               "   bounding-box restriction, the extra draw calls' fixed overhead outweighs what's saved.\n"
               "   Do NOT pursue a production implementation of this approach.\n",
               100.0*(e_wall-a_wall)/a_wall);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
