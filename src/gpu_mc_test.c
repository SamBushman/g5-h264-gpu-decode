/*
 * gpu-mc-test: Milestone 7 (scoped start). Captures a real reference
 * frame's actual decoded pixels (frame 0) plus a real macroblock's motion
 * vector from frame 1 (via the x1900_hook, now fixed - see h264_mb.c's
 * updated comment: mv_cache/ref_cache need the scan8[0]=12 offset, not
 * raw index 0, which is why every MV read as (0,0) before this fix even
 * on genuinely-moving content), then runs H.264's horizontal half-pel
 * luma interpolation (the core 6-tap primitive; pure-vertical, diagonal,
 * and quarter-pel-averaging cases are structurally analogous and left for
 * a follow-up pass - this milestone proves the core primitive) through
 * both a CPU port of FFmpeg's h264qpel_template.c and a GLSL shader on
 * the real X1900, using real reference pixels and a real captured MV.
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

/* ---- capture: frame 0's Y plane, then frame 1's first MB with fx=2,fy=0 ---- */

static int g_frame_idx = 0;
static unsigned char *g_ref_y = NULL;
static int g_ref_w, g_ref_h, g_ref_stride;

static int g_captured = 0;
static int g_cap_mb_x, g_cap_mb_y, g_cap_mvx, g_cap_mvy;

static int mc_hook(int mb_x, int mb_y, int mb_type, int qscale,
                    const int16_t *coeffs, const uint8_t *nnz,
                    const int16_t *mv_l0, const int8_t *ref_l0,
                    void *userdata) {
    (void)mb_type; (void)qscale; (void)coeffs; (void)nnz; (void)userdata;
    if (g_frame_idx == 1 && !g_captured && ref_l0[0] == 0 &&
        (mv_l0[0] & 3) == 2 && (mv_l0[1] & 3) == 0) { /* fx=2 (half-pel H), fy=0 (full-pel V) */
        g_cap_mb_x = mb_x; g_cap_mb_y = mb_y;
        g_cap_mvx = mv_l0[0]; g_cap_mvy = mv_l0[1];
        g_captured = 1;
    }
    return 0;
}

/* ---- (a) CPU reference: port of put_h264_qpel_h_lowpass (h264qpel_template.c) ---- */

static int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static int h_lowpass_ref(const unsigned char *src, int stride, int x, int y) {
    /* half-pel position between pixel x and x+1, at row y */
    const unsigned char *p = src + y * stride + x;
    int v = (p[0] + p[1]) * 20 - (p[-1] + p[2]) * 5 + (p[-2] + p[3]);
    return clip255((v + 16) >> 5);
}

static void check_gl(const char *where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) fprintf(stderr, "GL error at %s: 0x%lx\n", where, (unsigned long)e);
}

static GLhandleARB compile(GLenum type, const char *src) {
    GLhandleARB s = glCreateShaderObjectARB(type);
    glShaderSourceARB(s, 1, &src, NULL);
    glCompileShaderARB(s);
    GLint ok = 0;
    glGetObjectParameterivARB(s, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) {
        char log[4096]; GLsizei n;
        glGetInfoLogARB(s, sizeof(log), &n, log);
        fprintf(stderr, "compile failed:\n%s\n", log);
        exit(1);
    }
    return s;
}

static GLhandleARB link_prog(const char *vs, const char *fs) {
    GLhandleARB prog = glCreateProgramObjectARB();
    glAttachObjectARB(prog, compile(GL_VERTEX_SHADER_ARB, vs));
    glAttachObjectARB(prog, compile(GL_FRAGMENT_SHADER_ARB, fs));
    glLinkProgramARB(prog);
    GLint ok = 0;
    glGetObjectParameterivARB(prog, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) {
        char log[4096]; GLsizei n;
        glGetInfoLogARB(prog, sizeof(log), &n, log);
        fprintf(stderr, "link failed:\n%s\n", log);
        exit(1);
    }
    return prog;
}

static const char *vs_plain =
"void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; "
"gl_TexCoord[0] = gl_MultiTexCoord0; }";

/* Samples a 10x10 patch of the real reference frame (centered so every
 * output pixel's -2..+3 taps are in range), computes the H.264 6-tap
 * horizontal half-pel filter per output pixel - scalar ops only (no
 * matrix math, per quirk #12), CLIP done via explicit min/max (no
 * texture2DRect call inside a branch, per quirk #2 - all fetches are
 * unconditional). */
static const char *fs_hpel =
"uniform sampler2DRect refTex;\n"
"void main() {\n"
"  vec2 base = gl_TexCoord[0].xy;\n" /* already offset to this output pixel's src position */
"  float p_2 = texture2DRect(refTex, base + vec2(-2.0, 0.0)).r * 255.0;\n"
"  float p_1 = texture2DRect(refTex, base + vec2(-1.0, 0.0)).r * 255.0;\n"
"  float p0  = texture2DRect(refTex, base + vec2( 0.0, 0.0)).r * 255.0;\n"
"  float p1  = texture2DRect(refTex, base + vec2( 1.0, 0.0)).r * 255.0;\n"
"  float p2  = texture2DRect(refTex, base + vec2( 2.0, 0.0)).r * 255.0;\n"
"  float p3  = texture2DRect(refTex, base + vec2( 3.0, 0.0)).r * 255.0;\n"
"  float v = (p0 + p1) * 20.0 - (p_1 + p2) * 5.0 + (p_2 + p3);\n"
"  float result = floor((v + 16.0) / 32.0);\n"
"  result = max(0.0, min(255.0, result));\n"
"  gl_FragColor = vec4(result / 255.0, 0.0, 0.0, 1.0);\n"
"}\n";

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.mp4>\n", argv[0]); return 1; }

    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) { fprintf(stderr, "demux failed\n"); return 1; }
    g_ref_w = mov.width; g_ref_h = mov.height;

    int avcc_len = 0;
    unsigned char *avcc = mp4_build_avcc(&mov, &avcc_len);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(avcc_len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)avcc_len);
    ctx->extradata_size = avcc_len;
    avcodec_open2(ctx, codec, NULL);
    ff_x1900_set_mb_hook(mc_hook, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    for (uint32_t i = 0; i < mov.sample_count && !(g_frame_idx > 1 || (g_frame_idx == 1 && g_captured)); i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (g_frame_idx == 0) {
                /* save frame 0's real decoded Y plane as our reference frame */
                g_ref_stride = frame->linesize[0];
                g_ref_y = (unsigned char *)malloc((size_t)g_ref_stride * frame->height);
                memcpy(g_ref_y, frame->data[0], (size_t)g_ref_stride * frame->height);
            }
            g_frame_idx++;
            av_frame_unref(frame);
        }
    }
    if (!g_captured) { fprintf(stderr, "no fx=2,fy=0 MB found in frame 1\n"); return 1; }

    printf("Reference: real frame 0 Y plane (%dx%d, stride %d)\n", g_ref_w, g_ref_h, g_ref_stride);
    printf("Captured real MB(%d,%d) in frame 1: mv=(%d,%d) -> half-pel horizontal case\n",
           g_cap_mb_x, g_cap_mb_y, g_cap_mvx, g_cap_mvy);

    /* Integer part of the MV (quarter-pel units -> pixels), plus the
     * macroblock's own pixel origin, gives the source position this
     * macroblock's prediction reads from. */
    int src_x = g_cap_mb_x * 16 + (g_cap_mvx >> 2);
    int src_y = g_cap_mb_y * 16 + (g_cap_mvy >> 2);
    printf("Predicting a 4x4 luma block at reference position (%d,%d)\n\n", src_x, src_y);

    /* --- (a) CPU reference for a 4x4 block --- */
    int cpu_out[16];
    printf("(a) CPU reference (port of put_h264_qpel_h_lowpass):\n  ");
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            cpu_out[y * 4 + x] = h_lowpass_ref(g_ref_y, g_ref_stride, src_x + x, src_y + y);
            printf("%4d ", cpu_out[y * 4 + x]);
        }
    printf("\n\n");

    /* --- (b) GPU, same reference pixels, same algorithm --- */
    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext glctx = aglCreateContext(pf, NULL);
    aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf;
    aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(glctx, pbuf, 0, 0, 0);
    aglSetCurrentContext(glctx);

    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    /* Upload a patch of the real reference frame wide enough to cover all
     * 4 output columns' -2..+3 taps: width 4+5=9, height 4. Store as
     * normalized [0,1] luminance in .r (matches the shader's *255.0). */
    int patch_w = 9, patch_h = 4;
    float *patch = (float *)malloc(sizeof(float) * patch_w * patch_h * 4);
    for (int y = 0; y < patch_h; y++)
        for (int x = 0; x < patch_w; x++) {
            unsigned char v = g_ref_y[(src_y + y) * g_ref_stride + (src_x - 2 + x)];
            int idx = (y * patch_w + x) * 4;
            patch[idx + 0] = v / 255.0f;
            patch[idx + 1] = patch[idx + 2] = 0; patch[idx + 3] = 1;
        }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, patch_w, patch_h, 0,
                 GL_RGBA, GL_FLOAT, patch);
    check_gl("upload ref patch");

    GLhandleARB prog = link_prog(vs_plain, fs_hpel);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glUniform1iARB(glGetUniformLocationARB(prog, "refTex"), 0);

    /* Texcoords: output pixel (x,y) in [0,4) should sample base=(x+2,y+0.5)
     * in the uploaded patch (patch column 0 = src_x-2, row 0 = src_y;
     * +0.5 to land on texel centers for GL_NEAREST). Draw the quad with
     * texcoords offset accordingly so gl_TexCoord[0] IS that base coord. */
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(2.5f, 0.5f); glVertex2f(0, 0);
    glTexCoord2f(6.5f, 0.5f); glVertex2f(4, 0);
    glTexCoord2f(6.5f, 4.5f); glVertex2f(4, 4);
    glTexCoord2f(2.5f, 4.5f); glVertex2f(0, 4);
    glEnd();
    glFinish();
    check_gl("mc draw");

    unsigned char pixels[4 * 4 * 4];
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    int gpu_out[16];
    printf("(b) GPU (real X1900 hardware, same reference pixels + MV):\n  ");
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            /* GL row 0 = bottom in our ortho setup matching M6; our quad
             * was drawn with texcoord row 0 at y=0 (bottom), consistent
             * with the CPU loop's y=0 being the top reference row - since
             * we set glOrtho(0,4,0,4,...) with matching vertex y=0..4 and
             * texcoord v=0.5..4.5 increasing together, row index maps
             * directly without a flip. */
            int idx = row * 4 + col;
            unsigned char r8 = pixels[(row * 4 + col) * 4 + 0];
            gpu_out[idx] = r8;
            printf("%4d ", gpu_out[idx]);
        }
    printf("\n\n");

    int mismatches = 0;
    for (int i = 0; i < 16; i++)
        if (abs(gpu_out[i] - cpu_out[i]) > 1) mismatches++;

    printf("%s (%d/16 differ by >1)\n",
           mismatches == 0 ? "RESULT: GPU horizontal half-pel motion comp matches CPU reference"
                            : "RESULT: MISMATCH",
           mismatches);

    aglSetCurrentContext(NULL);
    aglDestroyContext(glctx);
    return mismatches != 0;
}
