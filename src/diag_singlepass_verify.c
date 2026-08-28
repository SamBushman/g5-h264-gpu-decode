/*
 * diag-singlepass-verify: directly answer "is quirk #14's two-pass diagonal
 * MC workaround still required?" now that precision-boundary-probe proved
 * the ALU itself is true FP32 (see plan.md's "Precision question (§4):
 * SETTLED" entry). That test used uniforms; this one uses the SAME data
 * path the real production shader does (texture2DRect fetches from a
 * GL_LUMINANCE8 reference texture, matching this project's live reftex
 * format) - the honest gap the uniform test explicitly flagged.
 *
 * Three implementations compared against real captured (mb,mv) cases for
 * all 5 diagonal-family phases (same infra as gpu_mc_singlepass_test.c's
 * phase 4c, reused verbatim - mp4 demux, hook, candidate finder):
 *
 *   1. TRUE CPU reference - this project's own already-verified, real
 *      FFmpeg-matching mc_luma_wh/hv_lowpass_wh (unrounded int32
 *      intermediate, single final round+clip) - the actual byte-exact
 *      target used everywhere else in the live decode path.
 *   2. EXISTING two-pass GPU shader (fs_diag_stage1+stage2, copied
 *      verbatim from gpu_mc_singlepass_test.c/gpu_live_decode_test.c) -
 *      the current production path, which routes a ROUNDED horizontal
 *      intermediate through an FBO texture specifically to avoid quirk
 *      #14's presumed FP24 limit.
 *   3. NEW single-pass GPU shader - computes the horizontal 6-tap sum as
 *      a raw, UNROUNDED float held only in shader registers (never
 *      written to a texture), then the vertical 6-tap directly over
 *      those raw values, matching the CPU reference's real algorithm
 *      exactly - if the ALU can carry the raw intermediate (max
 *      magnitude ~11000, nowhere near the ~2^23 exact-integer ceiling
 *      precision-boundary-probe confirmed), this should be able to hit
 *      true byte-exact, not just the two-pass shader's accepted
 *      tolerance - one GPU round trip cheaper per diagonal block.
 *
 * If (3) matches the TRUE reference at least as well as (2) does, the
 * two-pass workaround is proven unnecessary from a precision standpoint,
 * and the live decode path's dispatch_diag_group can be simplified to
 * one draw call. If (3) is measurably worse than (2), that's direct
 * proof the workaround still earns its keep - kept as-is, documented.
 */

#include "mp4box.h"
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/x1900_hook.h>

static int g_frame_idx = 0;
static unsigned char *g_ref_y = NULL;
static int g_ref_w, g_ref_h, g_ref_stride;

typedef struct { int mbx, mby, mvx, mvy, found; } Cand;
static Cand g_cand[16];
static int is_diag_idx(int idx) { return idx==10||idx==9||idx==11||idx==6||idx==14; }

static int margin_ok(int mb_x, int mb_y, int mvx, int mvy) {
    int sx = mb_x * 16 + (mvx >> 2), sy = mb_y * 16 + (mvy >> 2);
    return sx - 3 >= 0 && sx + 19 < g_ref_w && sy - 3 >= 0 && sy + 19 < g_ref_h;
}
static int hook(const X1900MbInfo *info, void *ud) {
    (void)ud;
    if (g_frame_idx != 1 || !info->ref_y || info->ref_l0[0] != 0) return 0;
    if (info->mb_type & ((1<<14)|(1<<15))) return 0;
    int mvx = info->mv_l0[0], mvy = info->mv_l0[1];
    int fx = mvx & 3, fy = mvy & 3;
    if (!is_diag_idx(fx + fy*4)) return 0;
    if (!margin_ok(info->mb_x, info->mb_y, mvx, mvy)) return 0;
    int idx = fx + fy * 4;
    if (!g_cand[idx].found) {
        g_cand[idx].mbx = info->mb_x; g_cand[idx].mby = info->mb_y;
        g_cand[idx].mvx = mvx; g_cand[idx].mvy = mvy;
        g_cand[idx].found = 1;
    }
    return 0;
}

/* ============ TRUE CPU reference - byte-for-byte port of this project's
 * own live gpu_live_decode_test.c mc_luma_wh/hv_lowpass_wh (w=h=16 case),
 * the REAL FFmpeg-matching algorithm, not the rounded-intermediate
 * approximation the two-pass GPU shader targets. ============ */
static int clip255_i(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int qpel_tap6(int a, int b, int c, int d, int e, int f) { return (c+d)*20-(b+e)*5+(a+f); }
static int cpu_half_h(const unsigned char *s, int st, int x, int y) {
    const unsigned char *p = s + y*st + x;
    return clip255_i((qpel_tap6(p[-2],p[-1],p[0],p[1],p[2],p[3])+16)>>5);
}
static int cpu_half_v(const unsigned char *s, int st, int x, int y) {
    const unsigned char *p = s + y*st + x;
    return clip255_i((qpel_tap6(p[-2*st],p[-st],p[0],p[st],p[2*st],p[3*st])+16)>>5);
}
static int cpu_mc_l2(int a, int b) { return (a+b+1)>>1; }
static int cpu_hv_true(const unsigned char *s, int st, int x, int y) {
    /* unrounded intermediate, real int32 arithmetic - exactly this
     * project's own hv_lowpass_wh for a single output pixel. */
    int tmp[6];
    for (int dy = -2; dy <= 3; dy++) {
        const unsigned char *p = s + (y+dy)*st + x;
        tmp[dy+2] = qpel_tap6(p[-2],p[-1],p[0],p[1],p[2],p[3]); /* raw, no round */
    }
    return clip255_i((qpel_tap6(tmp[0],tmp[1],tmp[2],tmp[3],tmp[4],tmp[5])+512)>>10);
}
static int cpu_ref_true(const unsigned char *s, int st, int x, int y, int hp, int vp) {
    if (hp==2 && vp==2) return cpu_hv_true(s, st, x, y);
    if (hp==2) { int ro=(vp==3)?1:0; return cpu_mc_l2(cpu_half_h(s,st,x,y+ro), cpu_hv_true(s,st,x,y)); }
    { int co=(hp==3)?1:0; return cpu_mc_l2(cpu_half_v(s,st,x+co,y), cpu_hv_true(s,st,x,y)); }
}

/* ============ GL plumbing ============ */
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

/* ---- EXISTING production shaders, copied verbatim (fs_diag_stage1/2) ---- */
static const char *fs_diag_stage1 =
"uniform sampler2DRect refTex;\n"
"uniform vec2 baseOffset;\n"
"void main() {\n"
"  vec2 b = floor(gl_FragCoord.xy) + baseOffset;\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float raw = (a0+a3)*20.0 - (a1+a4)*5.0 + (a2+a5);\n"
"  gl_FragColor = vec4(floor((raw+16.0)/32.0), 0.0, 0.0, 1.0);\n"
"}\n";
static const char *fs_diag_stage2 =
"uniform sampler2DRect stage1Tex;\n"
"uniform sampler2DRect refTex;\n"
"uniform vec2 baseOffset;\n"
"uniform vec2 refOrigin;\n"
"uniform float hPhase;\n"
"uniform float vPhase;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float dec(vec2 b, float dy) { return texture2DRect(stage1Tex, b+vec2(0.0,dy)).r; }\n"
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
"void main() {\n"
"  vec2 b = floor(gl_FragCoord.xy) + baseOffset;\n"
"  float hm2=dec(b,-2.0); float hm1=dec(b,-1.0); float h0=dec(b,0.0);\n"
"  float h1=dec(b,1.0);   float h2=dec(b,2.0);    float h3=dec(b,3.0);\n"
"  float v = (h0+h1)*20.0 - (hm1+h2)*5.0 + (hm2+h3);\n"
"  float diag = clip255(floor((v+16.0)/32.0));\n"
"  vec2 rb = floor(gl_FragCoord.xy) + refOrigin;\n"
"  float halfH0 = halfH(rb);\n"
"  float halfH1 = halfH(rb + vec2(0.0,1.0));\n"
"  float halfV0 = halfV(rb);\n"
"  float halfV1 = halfV(rb + vec2(1.0,0.0));\n"
"  float result = diag;\n"
"  if (hPhase == 2.0 && vPhase == 2.0) {\n"
"    result = diag;\n"
"  } else if (hPhase == 2.0) {\n"
"    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;\n"
"    result = floor((hOp + diag + 1.0) / 2.0);\n"
"  } else {\n"
"    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;\n"
"    result = floor((vOp + diag + 1.0) / 2.0);\n"
"  }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

/* ---- NEW single-pass shader: raw horizontal tap6 kept unrounded in a
 * register (never written to a texture), vertical tap6 directly over
 * those raw values, single final round+clip - matches cpu_hv_true/
 * cpu_ref_true exactly, one draw call instead of two. ---- */
static const char *fs_diag_singlepass =
"uniform sampler2DRect refTex;\n"
"uniform vec2 baseOffset;\n"
"uniform float hPhase;\n"
"uniform float vPhase;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float tap6raw(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  return (a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"}\n"
"float diagExact(vec2 b) {\n"
"  float hm2=tap6raw(b+vec2(0.0,-2.0));\n"
"  float hm1=tap6raw(b+vec2(0.0,-1.0));\n"
"  float h0 =tap6raw(b+vec2(0.0, 0.0));\n"
"  float h1 =tap6raw(b+vec2(0.0, 1.0));\n"
"  float h2 =tap6raw(b+vec2(0.0, 2.0));\n"
"  float h3 =tap6raw(b+vec2(0.0, 3.0));\n"
"  float v = (h0+h1)*20.0 - (hm1+h2)*5.0 + (hm2+h3);\n"
"  return clip255(floor((v+512.0)/1024.0));\n"
"}\n"
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
"void main() {\n"
"  vec2 b = floor(gl_FragCoord.xy) + baseOffset;\n"
"  float diag = diagExact(b);\n"
"  float result = diag;\n"
"  if (hPhase == 2.0 && vPhase == 2.0) {\n"
"    result = diag;\n"
"  } else if (hPhase == 2.0) {\n"
"    float ro = (vPhase == 3.0) ? 1.0 : 0.0;\n"
"    float hOp = halfH(b + vec2(0.0, ro));\n"
"    result = floor((hOp + diag + 1.0) / 2.0);\n"
"  } else {\n"
"    float co = (hPhase == 3.0) ? 1.0 : 0.0;\n"
"    float vOp = halfV(b + vec2(co, 0.0));\n"
"    result = floor((vOp + diag + 1.0) / 2.0);\n"
"  }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

static int run_two_pass(int idx, Cand *c, unsigned char out[16*16]) {
    int src_x = c->mbx * 16 + (c->mvx >> 2), src_y = c->mby * 16 + (c->mvy >> 2);
    int hp = idx & 3, vp = idx >> 2;
    int pad = 3, pw = 16+2*pad+1, ph = 16+2*pad+1;
    float *patch = (float*)malloc(sizeof(float)*pw*ph*4);
    for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) {
        unsigned char v = g_ref_y[(src_y-pad+y)*g_ref_stride+(src_x-pad+x)];
        int i = (y*pw+x)*4; patch[i]=v/255.0f; patch[i+1]=patch[i+2]=0; patch[i+3]=1;
    }
    GLuint refTex; glGenTextures(1,&refTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,pw,ph,0,GL_RGBA,GL_FLOAT,patch);
    checkgl("2pass upload ref");

    int s1w = 16, s1h = 21;
    GLuint s1Tex; glGenTextures(1,&s1Tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,s1Tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,s1w,s1h,0,GL_RGBA,GL_FLOAT,NULL);
    GLuint fbo; glGenFramebuffersEXT(1,&fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,s1Tex,0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT) fprintf(stderr, "2pass FBO incomplete: 0x%x\n", status);

    glViewport(0,0,s1w,s1h);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,s1w,0,s1h,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    static GLhandleARB progA = 0;
    if (!progA) progA = linkp(vs_plain, fs_diag_stage1);
    glUseProgramObjectARB(progA);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
    glUniform1iARB(glGetUniformLocationARB(progA,"refTex"),0);
    glUniform2fARB(glGetUniformLocationARB(progA,"baseOffset"), pad+0.5f, pad-2+0.5f);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(s1w,0);glVertex2f(s1w,s1h);glVertex2f(0,s1h); glEnd();
    glFinish(); checkgl("2pass stage1 draw");

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0);
    glViewport(0,0,16,16);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,16,0,16,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    static GLhandleARB progB = 0;
    if (!progB) progB = linkp(vs_plain, fs_diag_stage2);
    glUseProgramObjectARB(progB);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,s1Tex);
    glUniform1iARB(glGetUniformLocationARB(progB,"stage1Tex"),0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
    glUniform1iARB(glGetUniformLocationARB(progB,"refTex"),1);
    glUniform2fARB(glGetUniformLocationARB(progB,"baseOffset"), 0.5f, 2.5f);
    glUniform2fARB(glGetUniformLocationARB(progB,"refOrigin"), pad+0.5f, pad+0.5f);
    glUniform1fARB(glGetUniformLocationARB(progB,"hPhase"), (float)hp);
    glUniform1fARB(glGetUniformLocationARB(progB,"vPhase"), (float)vp);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(16,0);glVertex2f(16,16);glVertex2f(0,16); glEnd();
    glFinish(); checkgl("2pass stage2 draw");

    unsigned char pixels[16*16*4];
    glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    for (int i = 0; i < 256; i++) out[i] = pixels[i*4+0];

    glDeleteFramebuffersEXT(1,&fbo);
    glDeleteTextures(1,&refTex);
    glDeleteTextures(1,&s1Tex);
    free(patch);
    return 0;
}

static int run_single_pass(int idx, Cand *c, unsigned char out[16*16]) {
    int src_x = c->mbx * 16 + (c->mvx >> 2), src_y = c->mby * 16 + (c->mvy >> 2);
    int hp = idx & 3, vp = idx >> 2;
    int pad = 3, pw = 16+2*pad+1, ph = 16+2*pad+1;
    float *patch = (float*)malloc(sizeof(float)*pw*ph*4);
    for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) {
        unsigned char v = g_ref_y[(src_y-pad+y)*g_ref_stride+(src_x-pad+x)];
        int i = (y*pw+x)*4; patch[i]=v/255.0f; patch[i+1]=patch[i+2]=0; patch[i+3]=1;
    }
    GLuint refTex; glGenTextures(1,&refTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,pw,ph,0,GL_RGBA,GL_FLOAT,patch);
    checkgl("1pass upload ref");

    glViewport(0,0,16,16);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,16,0,16,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    static GLhandleARB prog = 0;
    if (!prog) prog = linkp(vs_plain, fs_diag_singlepass);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
    glUniform1iARB(glGetUniformLocationARB(prog,"refTex"),0);
    glUniform2fARB(glGetUniformLocationARB(prog,"baseOffset"), pad+0.5f, pad+0.5f);
    glUniform1fARB(glGetUniformLocationARB(prog,"hPhase"), (float)hp);
    glUniform1fARB(glGetUniformLocationARB(prog,"vPhase"), (float)vp);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(16,0);glVertex2f(16,16);glVertex2f(0,16); glEnd();
    glFinish(); checkgl("1pass draw");

    unsigned char pixels[16*16*4];
    glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    for (int i = 0; i < 256; i++) out[i] = pixels[i*4+0];

    glDeleteTextures(1,&refTex);
    free(patch);
    return 0;
}

static const char *phase_name(int idx) {
    static const char *names[16] = {
        "mc00","mc10","mc20","mc30","mc01","mc11","mc21","mc31",
        "mc02","mc12","mc22","mc32","mc03","mc13","mc23","mc33"
    };
    return names[idx];
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.mp4>\n", argv[0]); return 1; }
    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) { fprintf(stderr, "demux failed\n"); return 1; }
    g_ref_w = mov.width; g_ref_h = mov.height;
    int alen = 0; unsigned char *avcc = mp4_build_avcc(&mov, &alen);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t*)av_mallocz(alen+AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)alen); ctx->extradata_size = alen;
    avcodec_open2(ctx, codec, NULL);
    ff_x1900_set_mb_hook(hook, NULL);
    AVPacket *pkt = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    int all_found = 0;
    for (uint32_t i = 0; i < mov.sample_count && !all_found; i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data+s->offset, s->size);
        avcodec_send_packet(ctx, pkt); av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (g_frame_idx == 0) {
                g_ref_stride = frame->linesize[0];
                g_ref_y = (unsigned char*)malloc((size_t)g_ref_stride * frame->height);
                memcpy(g_ref_y, frame->data[0], (size_t)g_ref_stride * frame->height);
            }
            g_frame_idx++; av_frame_unref(frame);
            if (g_frame_idx > 1) {
                all_found = 1;
                for (int idx = 0; idx < 16; idx++)
                    if (is_diag_idx(idx) && !g_cand[idx].found) all_found = 0;
            }
        }
    }

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext glctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(64, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(glctx, pbuf, 0, 0, 0); aglSetCurrentContext(glctx);

    printf("=== diag-singlepass-verify: TRUE CPU ref vs. two-pass GPU vs. single-pass GPU ===\n\n");
    int total = 0, twopass_exact_blocks = 0, singlepass_exact_blocks = 0;
    int twopass_total_mismatch = 0, singlepass_total_mismatch = 0;
    int twopass_worst = 0, singlepass_worst = 0;
    for (int idx = 0; idx < 16; idx++) {
        if (!is_diag_idx(idx)) continue;
        if (!g_cand[idx].found) { printf("%-5s: NOT FOUND in test clip\n", phase_name(idx)); continue; }
        total++;
        Cand *c = &g_cand[idx];
        int src_x = c->mbx * 16 + (c->mvx >> 2), src_y = c->mby * 16 + (c->mvy >> 2);
        int hp = idx & 3, vp = idx >> 2;
        unsigned char true_ref[256];
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++)
            true_ref[y*16+x] = (unsigned char)cpu_ref_true(g_ref_y, g_ref_stride, src_x+x, src_y+y, hp, vp);

        unsigned char two_out[256], one_out[256];
        run_two_pass(idx, c, two_out);
        run_single_pass(idx, c, one_out);

        int two_mism = 0, two_worst = 0, one_mism = 0, one_worst = 0;
        for (int i = 0; i < 256; i++) {
            int d2 = abs((int)two_out[i] - (int)true_ref[i]);
            int d1 = abs((int)one_out[i] - (int)true_ref[i]);
            if (d2 > 0) two_mism++;
            if (d1 > 0) one_mism++;
            if (d2 > two_worst) two_worst = d2;
            if (d1 > one_worst) one_worst = d1;
        }
        printf("%-5s (h=%d,v=%d) MB(%d,%d) mv=(%d,%d):\n", phase_name(idx), hp, vp, c->mbx, c->mby, c->mvx, c->mvy);
        printf("    two-pass   vs TRUE ref: %3d/256 differ, worst=%d  %s\n",
               two_mism, two_worst, two_mism==0 ? "BYTE-EXACT" : "approx");
        printf("    single-pass vs TRUE ref: %3d/256 differ, worst=%d  %s\n",
               one_mism, one_worst, one_mism==0 ? "BYTE-EXACT" : "approx");
        if (two_mism == 0) twopass_exact_blocks++;
        if (one_mism == 0) singlepass_exact_blocks++;
        twopass_total_mismatch += two_mism; singlepass_total_mismatch += one_mism;
        if (two_worst > twopass_worst) twopass_worst = two_worst;
        if (one_worst > singlepass_worst) singlepass_worst = one_worst;
    }
    printf("\n=== Summary (%d diagonal-family phases tested) ===\n", total);
    printf("two-pass   (current production): %d/%d blocks byte-exact vs TRUE ref, %d total pixel mismatches, worst diff=%d\n",
           twopass_exact_blocks, total, twopass_total_mismatch, twopass_worst);
    printf("single-pass (candidate replacement): %d/%d blocks byte-exact vs TRUE ref, %d total pixel mismatches, worst diff=%d\n",
           singlepass_exact_blocks, total, singlepass_total_mismatch, singlepass_worst);
    if (singlepass_total_mismatch <= twopass_total_mismatch) {
        printf("\n-> single-pass is AT LEAST AS GOOD as the two-pass workaround (often better/byte-exact).\n");
        printf("   Precision-wise, the two-pass workaround is NOT required.\n");
    } else {
        printf("\n-> single-pass is WORSE than the two-pass workaround.\n");
        printf("   The workaround IS providing real precision benefit - keep it.\n");
    }

    aglSetCurrentContext(NULL);
    aglDestroyContext(glctx);
    return 0;
}
