/*
 * gpu-mc-singlepass-test: item 4, phases 4a/4b/4c (name kept from 4a for
 * continuity - it grew to cover the diagonal family and batching too
 * rather than fragmenting into more files, since all three phases share
 * the same hook/CPU-reference/GL-plumbing infrastructure).
 *
 * Phase 4a. Proves ONE unified GLSL shader
 * correctly computes all 11 "single-pass family" quarter-pel luma MC
 * phases (see the plan's "Item 4 (GPU-accelerating MC): DESIGN PASS") -
 * every phase needing no FBO round-trip (i.e. everything except the 5
 * phases that touch the true 2D diagonal, which need M7's separate
 * two-pass precision fix, quirk #14 - not attempted here, that's phase 4c).
 *
 * Unlike M7's gpu-mc-quarterpel.c (4 separate compiled programs, one real
 * captured case per COMBINATOR PATTERN, 3 of 4 "pattern d" positions never
 * independently verified), this tests ALL 11 phases individually, each
 * against this project's own already-verified CPU reference
 * (gpu_live_decode_test.c's mc_luma_wh, reimplemented here byte-for-byte
 * so this file stays a standalone test, matching the project's established
 * one-file-per-GPU-primitive pattern) - not the H.264 spec re-derived by
 * hand (this project's own history warns against that).
 *
 * Unbatched deliberately (one block per draw call, phase 4a) - proves the
 * shader FORMULA correct before phase 4b adds the batching plumbing
 * (colInfoTex/blockInfoTex) on top of a formula already known to be right.
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

/* One real captured (mb_x,mb_y,mv) example per phase, found[16] indexed by
 * h_phase + v_phase*4 (matching luma_xy convention used throughout this
 * project). All 16 entries get filled in - the 5 diagonal-family indices
 * (10, 9, 11, 6, 14) are used by phase 4c below, single-pass code paths
 * simply skip them. */
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
    if (info->mb_type & ((1<<14)|(1<<15))) return 0; /* MB_TYPE_L1 - list-0 only, see project scope */
    int mvx = info->mv_l0[0], mvy = info->mv_l0[1];
    int fx = mvx & 3, fy = mvy & 3;
    if (!margin_ok(info->mb_x, info->mb_y, mvx, mvy)) return 0;
    int idx = fx + fy * 4;
    if (!g_cand[idx].found) {
        g_cand[idx].mbx = info->mb_x; g_cand[idx].mby = info->mb_y;
        g_cand[idx].mvx = mvx; g_cand[idx].mvy = mvy;
        g_cand[idx].found = 1;
    }
    return 0;
}

/* ============ CPU reference (byte-for-byte port of mc_luma_wh, w=h=16) ============ */
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
/* Mirrors mc_luma_wh's dispatch exactly (single-pass family branches only -
 * diagonal-needing phases are unreachable here, g_cand never fills them). */
static int cpu_ref(const unsigned char *s, int st, int x, int y, int hp, int vp) {
    if (hp==0 && vp==0) return s[y*st+x];
    if (hp==2 && vp==0) return cpu_half_h(s,st,x,y);
    if (hp==0 && vp==2) return cpu_half_v(s,st,x,y);
    if (vp==0) { int co=(hp==3)?1:0; return cpu_mc_l2(s[y*st+x+co], cpu_half_h(s,st,x,y)); }
    if (hp==0) { int ro=(vp==3)?1:0; return cpu_mc_l2(s[(y+ro)*st+x], cpu_half_v(s,st,x,y)); }
    { int ro=(vp==3)?1:0, co=(hp==3)?1:0;
      return cpu_mc_l2(cpu_half_h(s,st,x,y+ro), cpu_half_v(s,st,x+co,y)); }
}

/* ============ GL plumbing (same helpers as every prior GPU test) ============ */
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

/* Single unified shader for all 11 single-pass-family phases - see the
 * plan's design pass for the derivation (7 unconditional values, then a
 * value-only if/else selects among them, matching fs_idct_batch's already-
 * proven "compute everything, branch only to select" pattern - never a
 * texture2DRect call inside a branch, quirk #2). */
static const char *fs_mc_singlepass =
"uniform sampler2DRect refTex;\n"
"uniform vec2 patchOrigin;\n"
"uniform float hPhase;\n"
"uniform float vPhase;\n"
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
"  vec2 b = floor(gl_FragCoord.xy) + patchOrigin;\n"
"  float full00 = texture2DRect(refTex, b).r*255.0;\n"
"  float full10 = texture2DRect(refTex, b+vec2(1.0,0.0)).r*255.0;\n"
"  float full01 = texture2DRect(refTex, b+vec2(0.0,1.0)).r*255.0;\n"
"  float halfH0 = halfH(b);\n"
"  float halfH1 = halfH(b+vec2(0.0,1.0));\n"
"  float halfV0 = halfV(b);\n"
"  float halfV1 = halfV(b+vec2(1.0,0.0));\n"
"  float result = full00;\n"
"  if (hPhase == 0.0 && vPhase == 0.0) {\n"
"    result = full00;\n"
"  } else if (hPhase == 2.0 && vPhase == 0.0) {\n"
"    result = halfH0;\n"
"  } else if (hPhase == 0.0 && vPhase == 2.0) {\n"
"    result = halfV0;\n"
"  } else if (vPhase == 0.0) {\n"
"    float fullOp = full00; if (hPhase == 3.0) fullOp = full10;\n"
"    result = floor((fullOp + halfH0 + 1.0) / 2.0);\n"
"  } else if (hPhase == 0.0) {\n"
"    float fullOp = full00; if (vPhase == 3.0) fullOp = full01;\n"
"    result = floor((fullOp + halfV0 + 1.0) / 2.0);\n"
"  } else {\n"
"    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;\n"
"    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;\n"
"    result = floor((hOp + vOp + 1.0) / 2.0);\n"
"  }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

static AGLContext g_glctx;
static const char *phase_name(int idx) {
    static const char *names[16] = {
        "mc00","mc10","mc20","mc30","mc01","mc11","mc21","mc31",
        "mc02","mc12","mc22","mc32","mc03","mc13","mc23","mc33"
    };
    return names[idx];
}

static int run_one(int idx, Cand *c) {
    int src_x = c->mbx * 16 + (c->mvx >> 2), src_y = c->mby * 16 + (c->mvy >> 2);
    int hp = idx & 3, vp = idx >> 2;

    int cpu_out[16*16], gpu_out[16*16], mismatches = 0, worst = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            cpu_out[y*16+x] = cpu_ref(g_ref_y, g_ref_stride, src_x+x, src_y+y, hp, vp);

    int pad = 3, pw = 16+2*pad+1, ph = 16+2*pad+1; /* +1 for the x+1/y+1 full-pel reads */
    float *patch = (float*)malloc(sizeof(float)*pw*ph*4);
    for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) {
        unsigned char v = g_ref_y[(src_y-pad+y)*g_ref_stride+(src_x-pad+x)];
        int i = (y*pw+x)*4; patch[i]=v/255.0f; patch[i+1]=patch[i+2]=0; patch[i+3]=1;
    }
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,pw,ph,0,GL_RGBA,GL_FLOAT,patch);
    checkgl("upload");

    glViewport(0,0,16,16);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,16,0,16,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    static GLhandleARB prog = 0;
    if (!prog) prog = linkp(vs_plain, fs_mc_singlepass);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glUniform1iARB(glGetUniformLocationARB(prog,"refTex"),0);
    glUniform2fARB(glGetUniformLocationARB(prog,"patchOrigin"), pad+0.5f, pad+0.5f);
    glUniform1fARB(glGetUniformLocationARB(prog,"hPhase"), (float)hp);
    glUniform1fARB(glGetUniformLocationARB(prog,"vPhase"), (float)vp);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(16,0);glVertex2f(16,16);glVertex2f(0,16); glEnd();
    glFinish(); checkgl("draw");

    unsigned char pixels[16*16*4];
    glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    for (int row=0;row<16;row++) for (int col=0;col<16;col++) {
        int i=row*16+col;
        gpu_out[i]=pixels[i*4+0];
        int d = abs(gpu_out[i]-cpu_out[i]);
        if (d > 1) mismatches++;
        if (d > worst) worst = d;
    }
    printf("%-5s (h=%d,v=%d) MB(%d,%d) mv=(%d,%d): %s (%d/256 differ, worst=%d)\n",
           phase_name(idx), hp, vp, c->mbx, c->mby, c->mvx, c->mvy,
           mismatches==0 ? "MATCH" : "MISMATCH", mismatches, worst);

    glDeleteTextures(1,&tex);
    free(patch);
    return mismatches == 0;
}

/* ============ Phase 4b: batch all found blocks into ONE draw call ============
 * Fixed block width (16, matching every candidate this test finds) -
 * proves the blockInfoTex per-block lookup mechanism itself; the plan's
 * design pass's colInfoTex (for genuinely mixed block widths) is deferred
 * to phase 4d, where the live decode path actually needs it. The whole
 * reference frame is uploaded ONCE as one resident texture (not per-block
 * patches) - real blocks can be anywhere in the frame, unlike phase 4a's
 * per-block extracted patches. */
static const char *fs_mc_batch =
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"
"uniform float blockWidth;\n"
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
"  float blockIdx = floor(col / blockWidth);\n"
"  float localX = col - blockIdx * blockWidth;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx + 0.5, 0.5));\n"
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
"  if (hPhase == 0.0 && vPhase == 0.0) {\n"
"    result = full00;\n"
"  } else if (hPhase == 2.0 && vPhase == 0.0) {\n"
"    result = halfH0;\n"
"  } else if (hPhase == 0.0 && vPhase == 2.0) {\n"
"    result = halfV0;\n"
"  } else if (vPhase == 0.0) {\n"
"    float fullOp = full00; if (hPhase == 3.0) fullOp = full10;\n"
"    result = floor((fullOp + halfH0 + 1.0) / 2.0);\n"
"  } else if (hPhase == 0.0) {\n"
"    float fullOp = full00; if (vPhase == 3.0) fullOp = full01;\n"
"    result = floor((fullOp + halfV0 + 1.0) / 2.0);\n"
"  } else {\n"
"    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;\n"
"    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;\n"
"    result = floor((hOp + vOp + 1.0) / 2.0);\n"
"  }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

static void run_batched(Cand *cands, int idxs[], int n) {
    /* Full reference frame as one resident RGBA float texture - same r-
     * channel-only convention as every other texture in this project. */
    float *refbuf = (float*)malloc(sizeof(float) * g_ref_w * g_ref_h * 4);
    for (int y = 0; y < g_ref_h; y++) for (int x = 0; x < g_ref_w; x++) {
        int i = (y*g_ref_w+x)*4;
        refbuf[i] = g_ref_y[y*g_ref_stride+x] / 255.0f;
        refbuf[i+1] = refbuf[i+2] = 0; refbuf[i+3] = 1;
    }
    GLuint refTex; glGenTextures(1,&refTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,g_ref_w,g_ref_h,0,GL_RGBA,GL_FLOAT,refbuf);
    checkgl("upload refTex");
    free(refbuf);

    float *infobuf = (float*)malloc(sizeof(float) * n * 4);
    for (int i = 0; i < n; i++) {
        Cand *c = &cands[idxs[i]];
        int hp = idxs[i] & 3, vp = idxs[i] >> 2;
        infobuf[i*4+0] = (float)(c->mbx*16 + (c->mvx>>2));
        infobuf[i*4+1] = (float)(c->mby*16 + (c->mvy>>2));
        infobuf[i*4+2] = (float)hp;
        infobuf[i*4+3] = (float)vp;
    }
    GLuint infoTex; glGenTextures(1,&infoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,infoTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,n,1,0,GL_RGBA,GL_FLOAT,infobuf);
    checkgl("upload infoTex");
    free(infobuf);

    int vw = n * 16, vh = 16;
    glViewport(0,0,vw,vh);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,vw,0,vh,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    GLhandleARB prog = linkp(vs_plain, fs_mc_batch);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
    glUniform1iARB(glGetUniformLocationARB(prog,"refTex"),0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,infoTex);
    glUniform1iARB(glGetUniformLocationARB(prog,"blockInfoTex"),1);
    glUniform1fARB(glGetUniformLocationARB(prog,"blockWidth"),16.0f);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(vw,0);glVertex2f(vw,vh);glVertex2f(0,vh); glEnd();
    glFinish(); checkgl("batched draw");

    unsigned char *pixels = (unsigned char*)malloc((size_t)vw*vh*4);
    glReadPixels(0,0,vw,vh,GL_RGBA,GL_UNSIGNED_BYTE,pixels);

    int total_mismatches = 0, blocks_ok = 0;
    for (int i = 0; i < n; i++) {
        Cand *c = &cands[idxs[i]];
        int hp = idxs[i] & 3, vp = idxs[i] >> 2;
        int src_x = c->mbx*16 + (c->mvx>>2), src_y = c->mby*16 + (c->mvy>>2);
        int mism = 0, worst = 0;
        for (int row = 0; row < 16; row++) for (int col = 0; col < 16; col++) {
            int cpu_v = cpu_ref(g_ref_y, g_ref_stride, src_x+col, src_y+row, hp, vp);
            int px_idx = (row*vw + i*16 + col) * 4;
            int gpu_v = pixels[px_idx];
            int d = abs(gpu_v - cpu_v);
            if (d > 1) mism++;
            if (d > worst) worst = d;
        }
        printf("batched %-5s block#%d: %s (%d/256 differ, worst=%d)\n",
               phase_name(idxs[i]), i, mism==0?"MATCH":"MISMATCH", mism, worst);
        total_mismatches += mism;
        if (mism == 0) blocks_ok++;
    }
    printf("\nbatched: %d/%d blocks matched in ONE draw call (%d total pixel mismatches)\n",
           blocks_ok, n, total_mismatches);

    free(pixels);
    glDeleteTextures(1,&refTex);
    glDeleteTextures(1,&infoTex);
}

/* ============ Phase 4c: diagonal family (5 phases needing quirk #14's
 * two-pass precision fix) ============
 *
 * CPU reference deliberately does NOT match this project's own CPU-side
 * mc_luma_wh/hv_lowpass_wh bit-for-bit: those use FFmpeg's real unrounded-
 * intermediate algorithm (safe on real int32 arithmetic), but this FP24
 * GPU cannot do that (quirk #14, M7's original diagonal bug) - the
 * REALISTICALLY ACHIEVABLE target here is the same ROUNDED-intermediate
 * two-stage formula gpu-mc-diag-fixed.c already established as this
 * project's real, deliberate deviation. Wiring this into the live decode
 * path (phase 4d) will very slightly change diagonal-phase pixel values
 * from today's byte-exact CPU baseline - expected, not a bug, worth
 * flagging prominently when 4d lands. */
static int cpu_h_raw(const unsigned char *s, int st, int x, int y) {
    const unsigned char *p = s + y*st + x;
    return (p[0]+p[1])*20 - (p[-1]+p[2])*5 + (p[-2]+p[3]);
}
static int cpu_diag_two_stage(const unsigned char *s, int st, int x, int y) {
    int hr[6];
    for (int dy = -2; dy <= 3; dy++) hr[dy+2] = (cpu_h_raw(s, st, x, y+dy) + 16) >> 5;
    int v = (hr[2]+hr[3])*20 - (hr[1]+hr[4])*5 + (hr[0]+hr[5]);
    return clip255_i((v + 16) >> 5);
}
static int cpu_diag_ref(const unsigned char *s, int st, int x, int y, int hp, int vp) {
    if (hp==2 && vp==2) return cpu_diag_two_stage(s, st, x, y);
    if (hp==2) { int ro=(vp==3)?1:0; return cpu_mc_l2(cpu_half_h(s,st,x,y+ro), cpu_diag_two_stage(s,st,x,y)); }
    { int co=(hp==3)?1:0; return cpu_mc_l2(cpu_half_v(s,st,x+co,y), cpu_diag_two_stage(s,st,x,y)); }
}

/* Stage 1 reused VERBATIM from gpu-mc-diag-fixed.c (M7's already-verified
 * FP24 fix) - horizontal 6-tap, ROUNDED but NOT clamped (real range,
 * negative/>255 both possible), into a float-format intermediate texture. */
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

/* Stage 2: new for this project (gpu-mc-diag-fixed.c only ever tested the
 * pure-diagonal case) - vertical 6-tap over stage1's rounded intermediate
 * (gives the final diag pixel value, clipped to [0,255] here since it's
 * used directly as a real pixel operand below, not a further
 * intermediate), PLUS - for the 4 combinator phases - a FRESH halfH/halfV
 * evaluation straight from refTex (matching gpu-mc-quarterpel.c's mc21
 * stage2, which recomputes rather than reusing stage1 - an established,
 * if slightly redundant, already-proven choice, not a new one). Value-
 * only branch selects the final combination, same quirk-#2-safe pattern
 * as fs_mc_singlepass/fs_mc_batch. */
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

static int run_one_diag(int idx, Cand *c) {
    int src_x = c->mbx * 16 + (c->mvx >> 2), src_y = c->mby * 16 + (c->mvy >> 2);
    int hp = idx & 3, vp = idx >> 2;

    int cpu_out[16*16];
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++)
        cpu_out[y*16+x] = cpu_diag_ref(g_ref_y, g_ref_stride, src_x+x, src_y+y, hp, vp);

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
    checkgl("diag upload ref");

    /* Stage1: 16 wide (one column per output column), 21 tall (rows -2..18
     * relative to block, matching the CPU tmp[21][16] this project's own
     * hv_lowpass_wh already uses for h=16). */
    int s1w = 16, s1h = 21;
    GLuint s1Tex; glGenTextures(1,&s1Tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,s1Tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,s1w,s1h,0,GL_RGBA,GL_FLOAT,NULL);
    GLuint fbo; glGenFramebuffersEXT(1,&fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,s1Tex,0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT) fprintf(stderr, "diag FBO incomplete: 0x%x\n", status);

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
    glFinish(); checkgl("diag stage1 draw");

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
    glUniform2fARB(glGetUniformLocationARB(progB,"baseOffset"), 0.5f, 2.5f); /* stage1 row 2 = block row 0 */
    glUniform2fARB(glGetUniformLocationARB(progB,"refOrigin"), pad+0.5f, pad+0.5f);
    glUniform1fARB(glGetUniformLocationARB(progB,"hPhase"), (float)hp);
    glUniform1fARB(glGetUniformLocationARB(progB,"vPhase"), (float)vp);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(16,0);glVertex2f(16,16);glVertex2f(0,16); glEnd();
    glFinish(); checkgl("diag stage2 draw");

    unsigned char pixels[16*16*4];
    glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    int mismatches = 0, worst = 0;
    for (int row=0;row<16;row++) for (int col=0;col<16;col++) {
        int i=row*16+col;
        int gpu_v = pixels[i*4+0];
        int d = abs(gpu_v - cpu_out[i]);
        if (d > 1) mismatches++;
        if (d > worst) worst = d;
    }
    printf("%-5s (h=%d,v=%d) MB(%d,%d) mv=(%d,%d): %s (%d/256 differ, worst=%d)\n",
           phase_name(idx), hp, vp, c->mbx, c->mby, c->mvx, c->mvy,
           mismatches==0 ? "MATCH" : "MISMATCH", mismatches, worst);

    glDeleteFramebuffersEXT(1,&fbo);
    glDeleteTextures(1,&refTex);
    glDeleteTextures(1,&s1Tex);
    free(patch);
    return mismatches == 0;
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
                    if (!g_cand[idx].found) all_found = 0;
            }
        }
    }

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    g_glctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    /* Sized generously for both the batched test below AND phase 4c's
     * diagonal stage1 (16 wide x 21 tall) (quirk #15: this driver ties
     * FBO/render bounds to the Pbuffer's own drawable size - a too-small
     * Pbuffer silently truncates/corrupts wider/taller draws, no GL
     * error). 16 phases * 16px = 256 wide is the widest viewport used. */
    AGLPbuffer pbuf; aglCreatePBuffer(256, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(g_glctx, pbuf, 0, 0, 0); aglSetCurrentContext(g_glctx);

    int total = 0, matched = 0;
    int found_idxs[16], found_n = 0;
    for (int idx = 0; idx < 16; idx++) {
        if (idx==10||idx==9||idx==11||idx==6||idx==14) continue; /* diagonal family - phase 4c */
        if (!g_cand[idx].found) { printf("%-5s: NOT FOUND in test clip\n", phase_name(idx)); continue; }
        total++;
        if (run_one(idx, &g_cand[idx])) matched++;
        found_idxs[found_n++] = idx;
    }
    printf("\n%d/%d single-pass phases matched (unbatched, phase 4a)\n", matched, total);

    printf("\n--- phase 4b: same %d blocks, ONE batched draw call ---\n", found_n);
    run_batched(g_cand, found_idxs, found_n);

    printf("\n--- phase 4c: diagonal family (2-pass, quirk #14) ---\n");
    int diag_total = 0, diag_matched = 0;
    for (int idx = 0; idx < 16; idx++) {
        if (!is_diag_idx(idx)) continue;
        if (!g_cand[idx].found) { printf("%-5s: NOT FOUND in test clip\n", phase_name(idx)); continue; }
        diag_total++;
        if (run_one_diag(idx, &g_cand[idx])) diag_matched++;
    }
    printf("\n%d/%d diagonal-family phases matched\n", diag_matched, diag_total);

    aglSetCurrentContext(NULL);
    aglDestroyContext(g_glctx);
    return (matched == total && diag_matched == diag_total) ? 0 : 1;
}
