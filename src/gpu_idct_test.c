/*
 * gpu-idct-test: Milestone 6. Captures a real macroblock's dequantized
 * luma coefficients (via the x1900_hook, still returning 0 - non-invasive,
 * decode proceeds normally) from an actual decode of the test clip, then
 * runs the SAME 16 coefficients through both:
 *   (a) a byte-for-byte C transliteration of FFmpeg's own
 *       ff_h264_idct_add (h264idct_template.c) - our correctness oracle,
 *   (b) a GLSL 1.10 fragment shader implementing the identical algorithm
 *       on the real ATI X1900 hardware (GL_RGBA_FLOAT32_ATI input texture,
 *       scalar ops only - no matrix multiply, per quirk #12; arithmetic
 *       right shifts emulated as floor(x / 2^n), exact for values this
 *       small since float32 has 24 bits of exact integer mantissa),
 * and diffs the two. This is the first real GPU reconstruction math this
 * project has run, using real bitstream-derived data, not synthetic input.
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

/* ---- capture one real macroblock's first luma 4x4 block ---- */

static int g_captured = 0;
static int g_target_mb_x = 1, g_target_mb_y = 0; /* MB(1,0), frame 0 - had real nonzero coeffs per M5's log */
static int g_captured_coeffs[16];

static int capture_hook(int mb_x, int mb_y, int mb_type, int qscale,
                         const int16_t *coeffs, const uint8_t *nnz,
                         const int16_t *mv_l0, const int8_t *ref_l0,
                         void *userdata) {
    (void)mb_type; (void)qscale; (void)nnz; (void)mv_l0; (void)ref_l0; (void)userdata;
    if (!g_captured && mb_x == g_target_mb_x && mb_y == g_target_mb_y) {
        for (int i = 0; i < 16; i++) g_captured_coeffs[i] = coeffs[i];
        g_captured = 1;
    }
    return 0; /* never take over - just observe */
}

/* ---- (a) CPU reference: byte-for-byte port of ff_h264_idct_add ---- */

static void h264_idct4x4_ref(const int block_in[16], int out[16]) {
    int block[16];
    for (int k = 0; k < 16; k++) block[k] = block_in[k];
    block[0] += 32;

    for (int i = 0; i < 4; i++) {
        int z0 = block[i + 4*0] + block[i + 4*2];
        int z1 = block[i + 4*0] - block[i + 4*2];
        int z2 = (block[i + 4*1] >> 1) - block[i + 4*3];
        int z3 =  block[i + 4*1] + (block[i + 4*3] >> 1);
        block[i + 4*0] = z0 + z3;
        block[i + 4*1] = z1 + z2;
        block[i + 4*2] = z1 - z2;
        block[i + 4*3] = z0 - z3;
    }
    for (int i = 0; i < 4; i++) {
        int z0 = block[0 + 4*i] + block[2 + 4*i];
        int z1 = block[0 + 4*i] - block[2 + 4*i];
        int z2 = (block[1 + 4*i] >> 1) - block[3 + 4*i];
        int z3 =  block[1 + 4*i] + (block[3 + 4*i] >> 1);
        out[i + 0*4] = (z0 + z3) >> 6;
        out[i + 1*4] = (z1 + z2) >> 6;
        out[i + 2*4] = (z1 - z2) >> 6;
        out[i + 3*4] = (z0 - z3) >> 6;
    }
}

/* ---- (b) GPU: identical algorithm, GLSL 1.10, scalar-only ---- */

static void check_gl(const char *where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) fprintf(stderr, "GL error at %s: 0x%x\n", where, e);
}

static GLhandleARB compile(GLenum type, const char *src) {
    GLhandleARB s = glCreateShaderObjectARB(type);
    glShaderSourceARB(s, 1, &src, NULL);
    glCompileShaderARB(s);
    GLint ok = 0;
    glGetObjectParameterivARB(s, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n;
        glGetInfoLogARB(s, sizeof(log), &n, log);
        fprintf(stderr, "shader compile failed:\n%s\n", log);
        exit(1);
    }
    return s;
}

static GLhandleARB link_prog(const char *vs_src, const char *fs_src) {
    GLhandleARB prog = glCreateProgramObjectARB();
    glAttachObjectARB(prog, compile(GL_VERTEX_SHADER_ARB, vs_src));
    glAttachObjectARB(prog, compile(GL_FRAGMENT_SHADER_ARB, fs_src));
    glLinkProgramARB(prog);
    GLint ok = 0;
    glGetObjectParameterivARB(prog, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n;
        glGetInfoLogARB(prog, sizeof(log), &n, log);
        fprintf(stderr, "link failed:\n%s\n", log);
        exit(1);
    }
    return prog;
}

/* c0..c15 = block[0..15] (already includes the +32 DC rounding term, added
 * host-side before upload - trivial and keeps the shader a pure port).
 * Every fragment redundantly computes the full 4x4 transform and picks its
 * own output value via floor(gl_FragCoord.xy) - wasteful but trivial at
 * this scale, and avoids any GLSL 1.10 dynamic-array-indexing risk on this
 * driver by using named scalars throughout instead of indexable arrays. */
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
/* pass 1: column pass, i=0..3 selects block[i+4*0..3] */
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
/* pass 2: row pass, i=0..3 selects block[4*i+0..3] (now m[]), writes out[i+j*4] */
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m2;  z1=m0-m2;  z2=floor(m1/2.0)-m3;   z3=m1+floor(m3/2.0);\n"
"  o0=floor((z0+z3)/64.0); o4=floor((z1+z2)/64.0); o8=floor((z1-z2)/64.0); o12=floor((z0-z3)/64.0);\n"
"  z0=m4+m6;  z1=m4-m6;  z2=floor(m5/2.0)-m7;   z3=m5+floor(m7/2.0);\n"
"  o1=floor((z0+z3)/64.0); o5=floor((z1+z2)/64.0); o9=floor((z1-z2)/64.0); o13=floor((z0-z3)/64.0);\n"
"  z0=m8+m10; z1=m8-m10; z2=floor(m9/2.0)-m11;  z3=m9+floor(m11/2.0);\n"
"  o2=floor((z0+z3)/64.0); o6=floor((z1+z2)/64.0); o10=floor((z1-z2)/64.0); o14=floor((z0-z3)/64.0);\n"
"  z0=m12+m14;z1=m12-m14;z2=floor(m13/2.0)-m15; z3=m13+floor(m15/2.0);\n"
"  o3=floor((z0+z3)/64.0); o7=floor((z1+z2)/64.0); o11=floor((z1-z2)/64.0); o15=floor((z0-z3)/64.0);\n"
"  vec2 p = floor(gl_FragCoord.xy - vec2(0.0,0.0));\n" /* viewport is 4x4, origin (0,0) */
"  int idx = int(p.x) + int(p.y) * 4;\n"
"  float result = o0;\n"
"  if (idx == 1) result = o1; else if (idx == 2) result = o2; else if (idx == 3) result = o3;\n"
"  else if (idx == 4) result = o4; else if (idx == 5) result = o5; else if (idx == 6) result = o6; else if (idx == 7) result = o7;\n"
"  else if (idx == 8) result = o8; else if (idx == 9) result = o9; else if (idx == 10) result = o10; else if (idx == 11) result = o11;\n"
"  else if (idx == 12) result = o12; else if (idx == 13) result = o13; else if (idx == 14) result = o14; else if (idx == 15) result = o15;\n"
/* encode result (can be negative, small magnitude) into a visible-range color: r = result/8.0 + 0.5 */
"  gl_FragColor = vec4(result / 64.0 + 0.5, 0.0, 0.0, 1.0);\n"
"}\n";

static const char *vs_plain =
"void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; "
"gl_TexCoord[0] = gl_MultiTexCoord0; }";

static void draw_quad(void) {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(4, 0); glVertex2f(4, 0);
    glTexCoord2f(4, 4); glVertex2f(4, 4);
    glTexCoord2f(0, 4); glVertex2f(0, 4);
    glEnd();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.mp4>\n", argv[0]);
        return 1;
    }

    /* --- Step 1: capture a real macroblock's coefficients via decode --- */
    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) { fprintf(stderr, "demux failed\n"); return 1; }
    int avcc_len = 0;
    unsigned char *avcc = mp4_build_avcc(&mov, &avcc_len);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(avcc_len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)avcc_len);
    ctx->extradata_size = avcc_len;
    avcodec_open2(ctx, codec, NULL);
    ff_x1900_set_mb_hook(capture_hook, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    for (uint32_t i = 0; i < mov.sample_count && !g_captured; i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) av_frame_unref(frame);
    }
    if (!g_captured) { fprintf(stderr, "target MB(%d,%d) never seen\n", g_target_mb_x, g_target_mb_y); return 1; }

    printf("Captured real MB(%d,%d) first luma 4x4 block coefficients:\n  ", g_target_mb_x, g_target_mb_y);
    for (int i = 0; i < 16; i++) printf("%d ", g_captured_coeffs[i]);
    printf("\n\n");

    /* --- Step 2: (a) CPU reference --- */
    int cpu_out[16];
    h264_idct4x4_ref(g_captured_coeffs, cpu_out);
    printf("(a) CPU reference (exact port of ff_h264_idct_add):\n  ");
    for (int i = 0; i < 16; i++) printf("%4d ", cpu_out[i]);
    printf("\n\n");

    /* --- Step 3: (b) GPU, same coefficients, same algorithm --- */
    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext glctx = aglCreateContext(pf, NULL);
    aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf;
    aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(glctx, pbuf, 0, 0, 0);
    aglSetCurrentContext(glctx);

    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Upload the SAME 16 coefficients (with the +32 DC rounding term
     * pre-added, matching block[0]+=32 in the reference) into a 4x4 float
     * rectangle texture. */
    float texel_data[16];
    for (int i = 0; i < 16; i++) texel_data[i] = (float)g_captured_coeffs[i];
    texel_data[0] += 32.0f;
    /* RGBA texture, only .r used - pack r=coeff, g=b=0, a=1 */
    float rgba[16 * 4];
    for (int i = 0; i < 16; i++) {
        rgba[i * 4 + 0] = texel_data[i];
        rgba[i * 4 + 1] = 0; rgba[i * 4 + 2] = 0; rgba[i * 4 + 3] = 1;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, 4, 4, 0,
                 GL_RGBA, GL_FLOAT, rgba);
    check_gl("upload coeff texture");

    GLhandleARB prog = link_prog(vs_plain, fs_idct);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glUniform1iARB(glGetUniformLocationARB(prog, "coeffTex"), 0);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_quad();
    glFinish();
    check_gl("idct draw");

    unsigned char pixels[4 * 4 * 4];
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    int gpu_out[16];
    printf("(b) GPU (real X1900 hardware, GLSL, same coefficients):\n  ");
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int idx = row * 4 + col;
            unsigned char r8 = pixels[(row * 4 + col) * 4 + 0];
            /* decode: r8/255 = result/8 + 0.5  =>  result = (r8/255 - 0.5) * 8 */
            float decoded = ((r8 / 255.0f) - 0.5f) * 64.0f;
            gpu_out[idx] = (int)(decoded < 0 ? decoded - 0.5f : decoded + 0.5f);
            printf("%4d ", gpu_out[idx]);
        }
    }
    printf("\n\n");

    int mismatches = 0;
    for (int i = 0; i < 16; i++) {
        int diff = abs(gpu_out[i] - cpu_out[i]);
        if (diff > 1) mismatches++; /* allow +-1 for the 8-bit color quantization round trip */
    }
    printf("%s (%d/16 differ by >1; note: GPU values were quantized through an\n"
           "8-bit color channel for this test's readback encoding, so exact\n"
           "byte-for-byte match isn't expected - the shader math itself would\n"
           "feed the next pass directly as float in the real pipeline, not\n"
           "through this test's lossy display-friendly encoding)\n",
           mismatches == 0 ? "RESULT: GPU IDCT matches CPU reference" : "RESULT: GPU IDCT MISMATCH",
           mismatches);

    aglSetCurrentContext(NULL);
    aglDestroyContext(glctx);
    return mismatches != 0;
}
