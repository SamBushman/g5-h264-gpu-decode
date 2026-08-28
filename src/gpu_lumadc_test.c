/*
 * gpu-lumadc-test: verifies the I16x16 luma DC Hadamard transform - a
 * distinct piece of math from the regular 4x4 IDCT (M6), needed to fully
 * cover I16x16 macroblocks (the dominant macroblock type in a typical
 * I-frame, per real content survey). Byte-for-byte port of
 * ff_h264_luma_dc_dequant_idct (h264idct_template.c), fed a real
 * captured I16x16 macroblock's DC coefficients + the exact qmul FFmpeg
 * itself uses, same discipline as every other primitive this session.
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

static int g_captured = 0;
static int g_dc[16];
static int g_qmul;

static int hook(int mb_x, int mb_y, int mb_type, int qscale,
                 const int16_t *coeffs, const uint8_t *nnz,
                 const int16_t *mv_l0, const int8_t *ref_l0,
                 const int16_t *luma_dc, int luma_dc_qmul, void *ud) {
    (void)mb_x; (void)mb_y; (void)qscale; (void)coeffs; (void)nnz;
    (void)mv_l0; (void)ref_l0; (void)ud;
    if (g_captured || !(mb_type & MB_TYPE_INTRA16x16)) return 0;
    /* only interesting if at least one DC coeff is actually nonzero */
    int any = 0;
    for (int i = 0; i < 16; i++) if (luma_dc[i]) any = 1;
    if (!any) return 0;
    for (int i = 0; i < 16; i++) g_dc[i] = luma_dc[i];
    g_qmul = luma_dc_qmul;
    g_captured = 1;
    return 0;
}

/* CPU reference: byte-for-byte port of ff_h264_luma_dc_dequant_idct,
 * re-indexed from FFmpeg's scattered memory-stride output layout into a
 * plain natural 4x4 grid (same values, just not spread across a 256-
 * element strided array meant for direct per-block-IDCT addition). */
static void luma_dc_ref(const int in16[16], int qmul, int out[4][4]) {
    int temp[16];
    for (int i = 0; i < 4; i++) {
        int z0 = in16[4*i+0] + in16[4*i+1];
        int z1 = in16[4*i+0] - in16[4*i+1];
        int z2 = in16[4*i+2] - in16[4*i+3];
        int z3 = in16[4*i+2] + in16[4*i+3];
        temp[4*i+0] = z0+z3; temp[4*i+1] = z0-z3; temp[4*i+2] = z1-z2; temp[4*i+3] = z1+z2;
    }
    for (int i = 0; i < 4; i++) {
        int z0 = temp[4*0+i] + temp[4*2+i];
        int z1 = temp[4*0+i] - temp[4*2+i];
        int z2 = temp[4*1+i] - temp[4*3+i];
        int z3 = temp[4*1+i] + temp[4*3+i];
        out[0][i] = (int)((z0+z3)*(int64_t)qmul + 128) >> 8;
        out[1][i] = (int)((z1+z2)*(int64_t)qmul + 128) >> 8;
        out[2][i] = (int)((z1-z2)*(int64_t)qmul + 128) >> 8;
        out[3][i] = (int)((z0-z3)*(int64_t)qmul + 128) >> 8;
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

/* Every fragment redundantly computes the full 4x4 Hadamard transform
 * and self-selects its own output value (same style as M6's IDCT
 * shader - trivial cost at this size, avoids GLSL dynamic array
 * indexing on this driver entirely). qmul folded in as a uniform (a
 * real per-QP value, not hardcoded) since it varies per macroblock. */
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
"  gl_FragColor = vec4(result/512.0 + 0.5, 0.0, 0.0, 1.0);\n"
"}\n";

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.mp4>\n", argv[0]); return 1; }
    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) { fprintf(stderr, "demux failed\n"); return 1; }
    int alen = 0; unsigned char *avcc = mp4_build_avcc(&mov, &alen);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(alen + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)alen); ctx->extradata_size = alen;
    avcodec_open2(ctx, codec, NULL);
    ff_x1900_set_mb_hook(hook, NULL);

    AVPacket *pkt = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    for (uint32_t i = 0; i < mov.sample_count && !g_captured; i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt); av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) av_frame_unref(frame);
    }
    if (!g_captured) { fprintf(stderr, "no I16x16 MB with nonzero DC found\n"); return 1; }

    printf("Captured real I16x16 luma DC coeffs (qmul=%d):\n  ", g_qmul);
    for (int i = 0; i < 16; i++) printf("%d ", g_dc[i]);
    printf("\n\n");

    int cpu_out[4][4];
    luma_dc_ref(g_dc, g_qmul, cpu_out);
    printf("(a) CPU reference:\n");
    for (int r = 0; r < 4; r++) {
        printf("  ");
        for (int c = 0; c < 4; c++) printf("%5d ", cpu_out[r][c]);
        printf("\n");
    }

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext glctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(16, 16, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(glctx, pbuf, 0, 0, 0); aglSetCurrentContext(glctx);

    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    float rgba[16 * 4];
    for (int i = 0; i < 16; i++) {
        rgba[i*4+0] = (float)g_dc[i]; rgba[i*4+1]=rgba[i*4+2]=0; rgba[i*4+3]=1;
    }
    GLuint tex;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, 4, 4, 0, GL_RGBA, GL_FLOAT, rgba);
    checkgl("upload dc");

    GLhandleARB prog = linkp(vs_plain, fs_lumadc);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glUniform1iARB(glGetUniformLocationARB(prog, "dcTex"), 0);
    glUniform1fARB(glGetUniformLocationARB(prog, "qmul"), (float)g_qmul);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4);
    glEnd(); glFinish(); checkgl("draw");

    unsigned char pixels[4*4*4];
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    printf("\n(b) GPU (real X1900 hardware):\n");
    int mismatches = 0;
    for (int row = 0; row < 4; row++) {
        printf("  ");
        for (int col = 0; col < 4; col++) {
            int idx = row*4+col;
            unsigned char r8 = pixels[idx*4+0];
            float decoded = ((r8/255.0f)-0.5f)*512.0f;
            int gv = (int)(decoded < 0 ? decoded-0.5f : decoded+0.5f);
            printf("%5d ", gv);
            if (abs(gv - cpu_out[row][col]) > 1) mismatches++;
        }
        printf("\n");
    }

    printf("\n%s (%d/16 differ by >1)\n",
           mismatches == 0 ? "RESULT: GPU luma-DC Hadamard transform matches CPU reference" : "RESULT: MISMATCH",
           mismatches);

    aglSetCurrentContext(NULL);
    aglDestroyContext(glctx);
    return mismatches != 0;
}
