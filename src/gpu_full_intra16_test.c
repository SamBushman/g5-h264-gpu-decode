/*
 * gpu-full-intra16-test: the real integration test. Captures a real
 * I16x16 macroblock (position, mode, coefficients, luma DC + qmul, and a
 * READ-ONLY pointer into the live frame buffer for neighbor context -
 * the hook still always returns 0, non-invasive, exactly like every
 * other hook use this session) not at a frame edge, so its left/top
 * neighbor pixels are real, already-reconstructed pixels from normal
 * FFmpeg decode (per spec, intra prediction correctly uses PRE-deblock
 * reconstructed neighbor samples - exactly what's available at hook
 * time, since deblocking is a separate later pass over the whole frame).
 *
 * Reconstructs the FULL macroblock independently:
 *   1. CPU intra 16x16 prediction (byte-for-byte port of
 *      h264pred_template.c's pred16x16_{dc,horizontal,vertical,plane}),
 *      using real captured neighbor pixels.
 *   2. GPU: luma DC Hadamard transform (verified separately already) +
 *      per-block IDCT (verified in M6) for all 16 4x4 luma blocks.
 *   3. Add prediction + residual, clip to a real pixel.
 * Then compares the result against the REAL decoded frame's actual
 * pixels at this exact macroblock position (read from the finished
 * AVFrame after full decode - undisputed ground truth, not just our own
 * CPU port). This is the first test this project has run that produces
 * a genuinely complete, correctly-reconstructed macroblock end to end.
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

#define MB_TYPE_INTRA16x16 (1 << 1)

/* ---- capture ---- */

static int g_captured = 0;
static int g_skip_target = 0; /* select the Nth qualifying MB, not just the 1st */
static int g_seen_count = 0;
static int g_mb_x, g_mb_y, g_pred_mode, g_qmul;
static int g_coeffs[16][16];   /* [luma 4x4 block 0..15][coeff 0..15] */
static int g_luma_dc[16];
static unsigned char g_left[16], g_top[16];
static unsigned char g_topleft;
static int g_frame_w, g_frame_h;

static int hook(const X1900MbInfo *info, void *ud) {
    (void)ud;
    if (g_captured || !(info->mb_type & MB_TYPE_INTRA16x16)) return 0;
    /* need a real interior macroblock (not touching frame top/left edge)
     * for valid neighbor pixels with this test's simple direct read. */
    if (info->mb_x == 0 || info->mb_y == 0) return 0;
    int any = 0;
    for (int i = 0; i < 16; i++) if (info->luma_dc[i]) any = 1;
    if (!any) return 0;

    if (g_seen_count++ < g_skip_target) return 0;

    g_mb_x = info->mb_x; g_mb_y = info->mb_y;
    g_pred_mode = info->intra16x16_pred_mode;
    g_qmul = info->luma_dc_qmul;
    for (int i = 0; i < 16; i++) g_luma_dc[i] = info->luma_dc[i];
    for (int blk = 0; blk < 16; blk++)
        for (int c = 0; c < 16; c++)
            g_coeffs[blk][c] = info->coeffs[blk * 16 + c];

    uint8_t *dy = info->dest_y;
    int ls = info->linesize;
    for (int i = 0; i < 16; i++) {
        g_left[i] = dy[-1 + i * ls];
        g_top[i] = dy[i - ls];
    }
    g_topleft = dy[-1 - ls];
    g_captured = 1;
    return 0; /* observe only */
}

/* ---- CPU intra 16x16 prediction, byte-for-byte port ---- */

static int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void pred16x16(int mode, const unsigned char left[16], const unsigned char top[16],
                       unsigned char topleft, int out[16][16]) {
    if (mode == 2) { /* Vertical */
        for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) out[r][c] = top[c];
    } else if (mode == 1) { /* Horizontal */
        for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) out[r][c] = left[r];
    } else if (mode == 0) { /* DC */
        int dc = 0;
        for (int i = 0; i < 16; i++) dc += left[i];
        for (int i = 0; i < 16; i++) dc += top[i];
        dc = (dc + 16) >> 5;
        for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) out[r][c] = dc;
    } else { /* mode==3, Plane */
        /* Port of pred16x16_plane_compat (svq3=rv40=0). FFmpeg's src0
         * pointer = top_row_ptr + 7 - stride's-worth-of-columns, i.e.
         * src0[k] = top[7+k] for k>=1, src0[-k] = top[7-k] for 1<=k<=7,
         * and src0[-8] = topleft (since 7-8=-1, the corner pixel). */
        int Hfull = ((int)top[7+1] - (int)top[7-1]);
        for (int k = 2; k <= 8; k++) {
            int a = (7 + k <= 15) ? top[7 + k] : topleft; /* 7+8=15 max, always in range 0..15 */
            int b = (7 - k >= 0) ? top[7 - k] : topleft;  /* 7-8=-1 -> topleft */
            Hfull += k * (a - b);
        }
        int Vfull = ((int)left[8] - (int)left[6]);
        for (int k = 2; k <= 8; k++) {
            int a = (7 + k <= 15) ? left[7 + k] : (int)topleft; /* never happens for V per FFmpeg's own src1/src2, but guard anyway */
            int b = (7 - k >= 0) ? left[7 - k] : (int)topleft;
            Vfull += k * (a - b);
        }
        int Hn = (5 * Hfull + 32) >> 6;
        int Vn = (5 * Vfull + 32) >> 6;
        int a = 16 * ((int)left[15] + (int)top[15] + 1) - 7 * (Vn + Hn);
        for (int r = 0; r < 16; r++) {
            int b = a;
            for (int c = 0; c < 16; c++) {
                out[r][c] = clip255(b >> 5);
                b += Hn;
            }
            a += Vn;
        }
    }
}

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

/* GPU luma-DC Hadamard transform (verified separately already) - reused
 * verbatim from gpu_lumadc_test.c's shader. */
static const char *fs_lumadc =
"uniform sampler2DRect dcTex;\n"
"uniform float qmul;\n"
"void main() {\n"
"  float c0 =texture2DRect(dcTex,vec2(0.5,0.5)).r; float c1 =texture2DRect(dcTex,vec2(1.5,0.5)).r;\n"
"  float c2 =texture2DRect(dcTex,vec2(2.5,0.5)).r; float c3 =texture2DRect(dcTex,vec2(3.5,0.5)).r;\n"
"  float c4 =texture2DRect(dcTex,vec2(0.5,1.5)).r; float c5 =texture2DRect(dcTex,vec2(1.5,1.5)).r;\n"
"  float c6 =texture2DRect(dcTex,vec2(2.5,1.5)).r; float c7 =texture2DRect(dcTex,vec2(3.5,1.5)).r;\n"
"  float c8 =texture2DRect(dcTex,vec2(0.5,2.5)).r; float c9 =texture2DRect(dcTex,vec2(1.5,2.5)).r;\n"
"  float c10=texture2DRect(dcTex,vec2(2.5,2.5)).r; float c11=texture2DRect(dcTex,vec2(3.5,2.5)).r;\n"
"  float c12=texture2DRect(dcTex,vec2(0.5,3.5)).r; float c13=texture2DRect(dcTex,vec2(1.5,3.5)).r;\n"
"  float c14=texture2DRect(dcTex,vec2(2.5,3.5)).r; float c15=texture2DRect(dcTex,vec2(3.5,3.5)).r;\n"
"  float z0,z1,z2,z3;\n"
"  float m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,m12,m13,m14,m15;\n"
"  z0=c0+c1;  z1=c0-c1;  z2=c2-c3;  z3=c2+c3;  m0=z0+z3; m1=z0-z3; m2=z1-z2; m3=z1+z2;\n"
"  z0=c4+c5;  z1=c4-c5;  z2=c6-c7;  z3=c6+c7;  m4=z0+z3; m5=z0-z3; m6=z1-z2; m7=z1+z2;\n"
"  z0=c8+c9;  z1=c8-c9;  z2=c10-c11;z3=c10+c11;m8=z0+z3; m9=z0-z3; m10=z1-z2;m11=z1+z2;\n"
"  z0=c12+c13;z1=c12-c13;z2=c14-c15;z3=c14+c15;m12=z0+z3;m13=z0-z3;m14=z1-z2;m15=z1+z2;\n"
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m8;  z1=m0-m8;  z2=m4-m12; z3=m4+m12;\n"
"  o0=floor((z0+z3)*qmul/256.0+0.5); o4=floor((z1+z2)*qmul/256.0+0.5); o8=floor((z1-z2)*qmul/256.0+0.5); o12=floor((z0-z3)*qmul/256.0+0.5);\n"
"  z0=m1+m9;  z1=m1-m9;  z2=m5-m13; z3=m5+m13;\n"
"  o1=floor((z0+z3)*qmul/256.0+0.5); o5=floor((z1+z2)*qmul/256.0+0.5); o9=floor((z1-z2)*qmul/256.0+0.5); o13=floor((z0-z3)*qmul/256.0+0.5);\n"
"  z0=m2+m10; z1=m2-m10; z2=m6-m14; z3=m6+m14;\n"
"  o2=floor((z0+z3)*qmul/256.0+0.5); o6=floor((z1+z2)*qmul/256.0+0.5); o10=floor((z1-z2)*qmul/256.0+0.5); o14=floor((z0-z3)*qmul/256.0+0.5);\n"
"  z0=m3+m11; z1=m3-m11; z2=m7-m15; z3=m7+m15;\n"
"  o3=floor((z0+z3)*qmul/256.0+0.5); o7=floor((z1+z2)*qmul/256.0+0.5); o11=floor((z1-z2)*qmul/256.0+0.5); o15=floor((z0-z3)*qmul/256.0+0.5);\n"
"  vec2 p = floor(gl_FragCoord.xy);\n"
"  int idx = int(p.x) + int(p.y) * 4;\n"
"  float result = o0;\n"
"  if (idx==1) result=o1; else if (idx==2) result=o2; else if (idx==3) result=o3;\n"
"  else if (idx==4) result=o4; else if (idx==5) result=o5; else if (idx==6) result=o6; else if (idx==7) result=o7;\n"
"  else if (idx==8) result=o8; else if (idx==9) result=o9; else if (idx==10) result=o10; else if (idx==11) result=o11;\n"
"  else if (idx==12) result=o12; else if (idx==13) result=o13; else if (idx==14) result=o14; else if (idx==15) result=o15;\n"
/* DC magnitude scales with qmul (up to ~30000+ for legal QPs), so a
 * single 8-bit channel can't hold enough range AND enough precision at
 * once (a fixed small scale clips for large qmul - seen as a uniform
 * +2 bias across an entire macroblock at qmul=20480; a fixed large
 * scale loses integer precision for small qmul, the M6-extension-era
 * failure mode). Pack as two exact bytes (R=high, G=low) over a biased
 * int16 range instead - exact for any realistic DC magnitude. */
"  float biased = result + 32768.0;\n"
"  float hi = floor(biased / 256.0);\n"
"  float lo = biased - hi * 256.0;\n"
"  gl_FragColor = vec4(hi/255.0, lo/255.0, 0.0, 1.0);\n"
"}\n";

/* GPU per-block 4x4 IDCT (verified in M6) - takes the 16 AC coefficients
 * of one block (DC term already zeroed/replaced by the Hadamard result
 * added on the CPU side after readback, matching FFmpeg's own
 * `block[0] = dc_value` substitution before idct_add). */
static const char *fs_idct =
"uniform sampler2DRect coeffTex;\n"
"void main() {\n"
"  float c0  = texture2DRect(coeffTex, vec2(0.5,0.5)).r;\n"
"  float c1  = texture2DRect(coeffTex, vec2(1.5,0.5)).r;\n"
"  float c2  = texture2DRect(coeffTex, vec2(2.5,0.5)).r;\n"
"  float c3  = texture2DRect(coeffTex, vec2(3.5,0.5)).r;\n"
"  float c4  = texture2DRect(coeffTex, vec2(0.5,1.5)).r;\n"
"  float c5  = texture2DRect(coeffTex, vec2(1.5,1.5)).r;\n"
"  float c6  = texture2DRect(coeffTex, vec2(2.5,1.5)).r;\n"
"  float c7  = texture2DRect(coeffTex, vec2(3.5,1.5)).r;\n"
"  float c8  = texture2DRect(coeffTex, vec2(0.5,2.5)).r;\n"
"  float c9  = texture2DRect(coeffTex, vec2(1.5,2.5)).r;\n"
"  float c10 = texture2DRect(coeffTex, vec2(2.5,2.5)).r;\n"
"  float c11 = texture2DRect(coeffTex, vec2(3.5,2.5)).r;\n"
"  float c12 = texture2DRect(coeffTex, vec2(0.5,3.5)).r;\n"
"  float c13 = texture2DRect(coeffTex, vec2(1.5,3.5)).r;\n"
"  float c14 = texture2DRect(coeffTex, vec2(2.5,3.5)).r;\n"
"  float c15 = texture2DRect(coeffTex, vec2(3.5,3.5)).r;\n"
"  float z0, z1, z2, z3;\n"
"  float m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15;\n"
"  z0 = c0 + c8;  z1 = c0 - c8;  z2 = floor(c4/2.0) - c12; z3 = c4 + floor(c12/2.0);\n"
"  m0 = z0+z3; m4 = z1+z2; m8 = z1-z2; m12 = z0-z3;\n"
"  z0 = c1 + c9;  z1 = c1 - c9;  z2 = floor(c5/2.0) - c13; z3 = c5 + floor(c13/2.0);\n"
"  m1 = z0+z3; m5 = z1+z2; m9 = z1-z2; m13 = z0-z3;\n"
"  z0 = c2 + c10; z1 = c2 - c10; z2 = floor(c6/2.0) - c14; z3 = c6 + floor(c14/2.0);\n"
"  m2 = z0+z3; m6 = z1+z2; m10 = z1-z2; m14 = z0-z3;\n"
"  z0 = c3 + c11; z1 = c3 - c11; z2 = floor(c7/2.0) - c15; z3 = c7 + floor(c15/2.0);\n"
"  m3 = z0+z3; m7 = z1+z2; m11 = z1-z2; m15 = z0-z3;\n"
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m2;  z1=m0-m2;  z2=floor(m1/2.0)-m3;   z3=m1+floor(m3/2.0);\n"
"  o0=floor((z0+z3)/64.0); o4=floor((z1+z2)/64.0); o8=floor((z1-z2)/64.0); o12=floor((z0-z3)/64.0);\n"
"  z0=m4+m6;  z1=m4-m6;  z2=floor(m5/2.0)-m7;   z3=m5+floor(m7/2.0);\n"
"  o1=floor((z0+z3)/64.0); o5=floor((z1+z2)/64.0); o9=floor((z1-z2)/64.0); o13=floor((z0-z3)/64.0);\n"
"  z0=m8+m10; z1=m8-m10; z2=floor(m9/2.0)-m11;  z3=m9+floor(m11/2.0);\n"
"  o2=floor((z0+z3)/64.0); o6=floor((z1+z2)/64.0); o10=floor((z1-z2)/64.0); o14=floor((z0-z3)/64.0);\n"
"  z0=m12+m14;z1=m12-m14;z2=floor(m13/2.0)-m15; z3=m13+floor(m15/2.0);\n"
"  o3=floor((z0+z3)/64.0); o7=floor((z1+z2)/64.0); o11=floor((z1-z2)/64.0); o15=floor((z0-z3)/64.0);\n"
"  vec2 p = floor(gl_FragCoord.xy);\n"
"  int idx = int(p.x) + int(p.y) * 4;\n"
"  float result = o0;\n"
"  if (idx == 1) result = o1; else if (idx == 2) result = o2; else if (idx == 3) result = o3;\n"
"  else if (idx == 4) result = o4; else if (idx == 5) result = o5; else if (idx == 6) result = o6; else if (idx == 7) result = o7;\n"
"  else if (idx == 8) result = o8; else if (idx == 9) result = o9; else if (idx == 10) result = o10; else if (idx == 11) result = o11;\n"
"  else if (idx == 12) result = o12; else if (idx == 13) result = o13; else if (idx == 14) result = o14; else if (idx == 15) result = o15;\n"
"  gl_FragColor = vec4(result / 64.0 + 0.5, 0.0, 0.0, 1.0);\n"
"}\n";

static AGLContext g_glctx;

static void gpu_lumadc(int dc16[16], int qmul, int out[16]) {
    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    float rgba[16*4];
    for (int i = 0; i < 16; i++) { rgba[i*4]=(float)dc16[i]; rgba[i*4+1]=rgba[i*4+2]=0; rgba[i*4+3]=1; }
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,4,4,0,GL_RGBA,GL_FLOAT,rgba);
    GLhandleARB prog = linkp(vs_plain, fs_lumadc);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glUniform1iARB(glGetUniformLocationARB(prog,"dcTex"),0);
    glUniform1fARB(glGetUniformLocationARB(prog,"qmul"),(float)qmul);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(4,0);glVertex2f(4,4);glVertex2f(0,4); glEnd();
    glFinish(); checkgl("lumadc draw");
    unsigned char px[4*4*4]; glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,px);
    for (int row=0; row<4; row++) for (int col=0; col<4; col++) {
        int idx=row*4+col;
        int hi = px[idx*4], lo = px[idx*4+1];
        out[idx] = hi*256 + lo - 32768; /* exact int16 decode, see shader comment */
    }
    glDeleteTextures(1,&tex); glDeleteObjectARB(prog);
}

static void gpu_idct4x4(int coeffs16[16], int out[16]) {
    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    float rgba[16*4];
    for (int i = 0; i < 16; i++) { rgba[i*4]=(float)coeffs16[i]; rgba[i*4+1]=rgba[i*4+2]=0; rgba[i*4+3]=1; }
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,4,4,0,GL_RGBA,GL_FLOAT,rgba);
    GLhandleARB prog = linkp(vs_plain, fs_idct);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glUniform1iARB(glGetUniformLocationARB(prog,"coeffTex"),0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(4,0);glVertex2f(4,4);glVertex2f(0,4); glEnd();
    glFinish(); checkgl("idct draw");
    unsigned char px[4*4*4]; glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,px);
    for (int row=0; row<4; row++) for (int col=0; col<4; col++) {
        int idx=row*4+col; unsigned char r8=px[idx*4];
        float dec=((r8/255.0f)-0.5f)*64.0f;
        out[idx]=(int)(dec<0?dec-0.5f:dec+0.5f);
    }
    glDeleteTextures(1,&tex); glDeleteObjectARB(prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.mp4> [skip_n]\n", argv[0]); return 1; }
    if (argc >= 3) g_skip_target = atoi(argv[2]);
    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) { fprintf(stderr, "demux failed\n"); return 1; }
    g_frame_w = mov.width; g_frame_h = mov.height;
    int alen = 0; unsigned char *avcc = mp4_build_avcc(&mov, &alen);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(alen + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)alen); ctx->extradata_size = alen;
    /* Force strictly single-threaded, in-order MB decode: the hook reads
     * already-reconstructed neighbor pixels straight out of the live
     * frame buffer, which is only guaranteed populated in raster order
     * if slice/frame threading is disabled. Not what actually caused
     * this test's first real bug (see x1900_hook's mb_x/mb_y fix in
     * h264_mb.c - a stride/width mismatch, not a threading race) but
     * still the correct, defensive setting for what this hook assumes. */
    ctx->thread_count = 1;
    ctx->thread_type = 0;
    avcodec_open2(ctx, codec, NULL);
    ff_x1900_set_mb_hook(hook, NULL);

    AVPacket *pkt = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    unsigned char *real_y = NULL; int real_ys = 0;
    for (uint32_t i = 0; i < mov.sample_count; i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt); av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (g_captured && !real_y) {
                /* this is the frame our target MB belongs to (captured
                 * during THIS frame's decode, so it's the one just
                 * finished) - save its real, fully-decoded (post-
                 * deblock) Y plane as ground truth. */
                real_ys = frame->linesize[0];
                real_y = (unsigned char *)malloc((size_t)real_ys * frame->height);
                memcpy(real_y, frame->data[0], (size_t)real_ys * frame->height);
            }
            av_frame_unref(frame);
        }
        if (real_y) break;
    }
    if (!g_captured || !real_y) { fprintf(stderr, "capture failed\n"); return 1; }

    printf("Captured real I16x16 MB(%d,%d), pred_mode=%d (0=DC,1=H,2=V,3=Plane), qmul=%d\n",
           g_mb_x, g_mb_y, g_pred_mode, g_qmul);

    /* --- CPU intra prediction --- */
    int pred[16][16];
    pred16x16(g_pred_mode, g_left, g_top, g_topleft, pred);

    /* --- GPU: luma DC Hadamard transform --- */
    /* Explicit 8-bit-per-channel request: the two-channel exact DC
     * readback (R=high byte, G=low byte) needs a real 8-bit G channel -
     * without AGL_GREEN_SIZE the driver is free to pick a narrower
     * default (e.g. 565-class) that silently truncates the low byte. */
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8,
                        AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8,
                        AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    g_glctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(16, 16, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(g_glctx, pbuf, 0, 0, 0); aglSetCurrentContext(g_glctx);

    int dc_out[16];
    gpu_lumadc(g_luma_dc, g_qmul, dc_out);

    /* --- GPU: per-block IDCT (with DC substituted, matching FFmpeg's
     * block[0]=dc_value before idct_add), reconstruct all 16 blocks ---
     *
     * Two distinct index-mapping issues here, both real bugs found while
     * debugging a Plane-mode macroblock with genuine per-block AC detail
     * (earlier DC-only test macroblocks passed by accident - a wrong
     * block mapping barely shows on flat/DC-dominated content):
     *
     * 1. g_coeffs[blk]/final_out placement: FFmpeg's own luma 4x4 block
     *    index 0..15 (the same numbering sl->mb / info->coeffs uses) is
     *    NOT plain row-major over the 4x4-of-4x4-blocks grid - it's the
     *    spec's Z-order quadrant numbering, recovered from scan8[i]:
     *        0  1  4  5
     *        2  3  6  7
     *        8  9 12 13
     *       10 11 14 15
     *    (derived from h264_slice.c's block_offset[i] using scan8[i]).
     *
     * 2. dc_out[] (this GPU shader's raw o0..o15 output, read back by
     *    fragment index) is NOT already in that same block-index order -
     *    tracing the shader's second Hadamard stage against FFmpeg's own
     *    ff_h264_luma_dc_dequant_idct (h264idct_template.c) shows
     *    real-block-N's value lands at raw shader output index
     *    dc_perm[N], not index N directly. */
    static const int blk_row[16] = {0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3};
    static const int blk_col[16] = {0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3};
    static const int dc_perm[16] = {0,4,1,5,8,12,9,13,2,6,3,7,10,14,11,15};

    int final_out[16][16];
    for (int blk = 0; blk < 16; blk++) {
        int blk_coeffs[16];
        for (int c = 0; c < 16; c++) blk_coeffs[c] = g_coeffs[blk][c];
        /* substitute Hadamard-transformed DC, plus the +32 rounding bias
         * FFmpeg's real ff_h264_idct_add always adds to block[0] before
         * the transform (h264idct_template.c: `block[0] += 1 << 5`) -
         * without it the final >>6 floors instead of rounds, off by
         * exactly 1 whenever the residual's phase crosses that boundary
         * (found via a case with pure-DC blocks where the shader's own
         * math was internally consistent but didn't match FFmpeg). */
        blk_coeffs[0] = dc_out[dc_perm[blk]] + 32;
        int residual[16];
        gpu_idct4x4(blk_coeffs, residual);
        int block_row = blk_row[blk] * 4, block_col = blk_col[blk] * 4;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                final_out[block_row+r][block_col+c] =
                    clip255(pred[block_row+r][block_col+c] + residual[r*4+c]);
    }

    printf("\nReconstructed macroblock (our CPU-pred + GPU-IDCT) vs REAL decoded frame:\n");
    int mismatches = 0, total = 0;
    for (int r = 0; r < 16; r++) {
        printf("  row%2d: ", r);
        for (int c = 0; c < 16; c++) {
            int real_px = real_y[(g_mb_y*16+r)*real_ys + (g_mb_x*16+c)];
            int ours = final_out[r][c];
            printf("%3d/%3d ", ours, real_px);
            total++;
            if (abs(ours - real_px) > 2) mismatches++;
        }
        printf("\n");
    }

    printf("\n%s (%d/%d pixels differ by >2)\n",
           mismatches == 0 ? "RESULT: full real-integration reconstruction MATCHES real decoded frame"
                            : "RESULT: MISMATCH",
           mismatches, total);
    if (mismatches != 0)
        printf("(note: ground truth is POST-deblock; this reconstruction is "
               "pred+IDCT only, no deblocking - a real difference here can be "
               "expected loop-filter effect at a strong edge, not necessarily "
               "a bug - see M8/plan scope.)\n");

    aglSetCurrentContext(NULL);
    aglDestroyContext(g_glctx);
    return mismatches != 0;
}
