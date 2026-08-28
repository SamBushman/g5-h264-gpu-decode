/*
 * gpu-mc-test2: Milestone 7 continued. Covers the two remaining hard
 * motion-comp primitives (vertical half-pel, and the true diagonal
 * two-stage case) using real captured reference pixels + real MVs, same
 * discipline as gpu_mc_test.c's horizontal case. Simpler shader structure
 * than M6's "every fragment computes all 16 outputs": each fragment only
 * needs taps relative to ITS OWN gl_FragCoord position, so no redundant
 * whole-block computation is needed here.
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

/* capture two real MBs in one decode pass: first fx=0,fy=2 (pure V) and
 * first fx=2,fy=2 (true diagonal) seen in frame 1 */
static int g_have_v = 0, g_have_d = 0;
static int g_v_mbx, g_v_mby, g_v_mvx, g_v_mvy;
static int g_d_mbx, g_d_mby, g_d_mvx, g_d_mvy;

/* The 6-tap span needs an asymmetric -2..+6 margin around a 4x4 block
 * (see run_case's pad comment). Reading real reference-frame pixels with
 * no edge-extension (real decoders replicate border pixels via an
 * edge-emulation buffer for MC near frame edges - not implemented in
 * this test, which just reads the raw decoded buffer directly), so only
 * accept a candidate MB far enough from every edge that the -2..+6 taps
 * stay in-bounds without needing that machinery. */
static int margin_ok(int mb_x, int mb_y, int mvx, int mvy) {
    int src_x = mb_x * 16 + (mvx >> 2);
    int src_y = mb_y * 16 + (mvy >> 2);
    return src_x - 2 >= 0 && src_x + 3 + 3 < g_ref_w &&
           src_y - 2 >= 0 && src_y + 3 + 3 < g_ref_h;
}

static int mc_hook(int mb_x, int mb_y, int mb_type, int qscale,
                    const int16_t *coeffs, const uint8_t *nnz,
                    const int16_t *mv_l0, const int8_t *ref_l0,
                    void *userdata) {
    (void)mb_type; (void)qscale; (void)coeffs; (void)nnz; (void)userdata;
    if (g_frame_idx == 1 && ref_l0[0] == 0) {
        int fx = mv_l0[0] & 3, fy = mv_l0[1] & 3;
        if (!g_have_v && fx == 0 && fy == 2 && margin_ok(mb_x, mb_y, mv_l0[0], mv_l0[1])) {
            g_v_mbx = mb_x; g_v_mby = mb_y; g_v_mvx = mv_l0[0]; g_v_mvy = mv_l0[1];
            g_have_v = 1;
        }
        if (!g_have_d && fx == 2 && fy == 2 && margin_ok(mb_x, mb_y, mv_l0[0], mv_l0[1])) {
            g_d_mbx = mb_x; g_d_mby = mb_y; g_d_mvx = mv_l0[0]; g_d_mvy = mv_l0[1];
            g_have_d = 1;
        }
    }
    return 0;
}

static int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static int v_lowpass_ref(const unsigned char *src, int stride, int x, int y) {
    const unsigned char *p = src + y * stride + x;
    int v = (p[0] + p[stride]) * 20 - (p[-stride] + p[2 * stride]) * 5 + (p[-2 * stride] + p[3 * stride]);
    return clip255((v + 16) >> 5);
}

static int h_raw(const unsigned char *src, int stride, int x, int y) {
    const unsigned char *p = src + y * stride + x;
    return (p[0] + p[1]) * 20 - (p[-1] + p[2]) * 5 + (p[-2] + p[3]);
}

static int diag_ref(const unsigned char *src, int stride, int x, int y) {
    int h_2 = h_raw(src, stride, x, y - 2);
    int h_1 = h_raw(src, stride, x, y - 1);
    int h0  = h_raw(src, stride, x, y);
    int h1  = h_raw(src, stride, x, y + 1);
    int h2  = h_raw(src, stride, x, y + 2);
    int h3  = h_raw(src, stride, x, y + 3);
    int v = (h0 + h1) * 20 - (h_1 + h2) * 5 + (h_2 + h3);
    return clip255((v + 512) >> 10);
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

/* vertical half-pel: taps in Y around this fragment's own sample position */
static const char *fs_vpel =
"uniform sampler2DRect refTex;\n"
"void main() {\n"
"  vec2 b = gl_TexCoord[0].xy;\n"
"  float p_2 = texture2DRect(refTex, b + vec2(0.0,-2.0)).r * 255.0;\n"
"  float p_1 = texture2DRect(refTex, b + vec2(0.0,-1.0)).r * 255.0;\n"
"  float p0  = texture2DRect(refTex, b + vec2(0.0, 0.0)).r * 255.0;\n"
"  float p1  = texture2DRect(refTex, b + vec2(0.0, 1.0)).r * 255.0;\n"
"  float p2  = texture2DRect(refTex, b + vec2(0.0, 2.0)).r * 255.0;\n"
"  float p3  = texture2DRect(refTex, b + vec2(0.0, 3.0)).r * 255.0;\n"
"  float v = (p0+p1)*20.0 - (p_1+p2)*5.0 + (p_2+p3);\n"
"  float result = floor((v+16.0)/32.0);\n"
"  result = max(0.0, min(255.0, result));\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

/* true diagonal: 9 raw horizontal sums (rows -2..6 relative to this
 * fragment), then one vertical 6-tap combine with the +512>>10 round -
 * matches h264qpel_template.c's hv_lowpass exactly. All texture fetches
 * unconditional (quirk #2), scalar-only (quirk #12). */
static const char *fs_diag =
"uniform sampler2DRect refTex;\n"
"float hraw(vec2 b, float dy) {\n"
"  float a2 = texture2DRect(refTex, b + vec2(-2.0, dy)).r * 255.0;\n"
"  float a1 = texture2DRect(refTex, b + vec2(-1.0, dy)).r * 255.0;\n"
"  float a0 = texture2DRect(refTex, b + vec2( 0.0, dy)).r * 255.0;\n"
"  float a3 = texture2DRect(refTex, b + vec2( 1.0, dy)).r * 255.0;\n"
"  float a4 = texture2DRect(refTex, b + vec2( 2.0, dy)).r * 255.0;\n"
"  float a5 = texture2DRect(refTex, b + vec2( 3.0, dy)).r * 255.0;\n"
"  return (a0+a3)*20.0 - (a1+a4)*5.0 + (a2+a5);\n"
"}\n"
"void main() {\n"
"  vec2 b = gl_TexCoord[0].xy;\n"
"  float hm2 = hraw(b, -2.0);\n"
"  float hm1 = hraw(b, -1.0);\n"
"  float h0  = hraw(b,  0.0);\n"
"  float h1  = hraw(b,  1.0);\n"
"  float h2  = hraw(b,  2.0);\n"
"  float h3  = hraw(b,  3.0);\n"
"  float v = (h0+h1)*20.0 - (hm1+h2)*5.0 + (hm2+h3);\n"
"  float result = floor((v+512.0)/1024.0);\n"
"  result = max(0.0, min(255.0, result));\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

static AGLContext g_glctx;

static void run_case(const char *label, const char *fs_src,
                      int (*ref_fn)(const unsigned char *, int, int, int),
                      int src_x, int src_y, int pad) {
    int cpu_out[16];
    printf("(a) CPU reference (%s):\n  ", label);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            cpu_out[y * 4 + x] = ref_fn(g_ref_y, g_ref_stride, src_x + x, src_y + y);
            printf("%4d ", cpu_out[y * 4 + x]);
        }
    printf("\n");

    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    int patch_w = 4 + 2 * pad, patch_h = 4 + 2 * pad;
    float *patch = (float *)malloc(sizeof(float) * patch_w * patch_h * 4);
    for (int y = 0; y < patch_h; y++)
        for (int x = 0; x < patch_w; x++) {
            unsigned char v = g_ref_y[(src_y - pad + y) * g_ref_stride + (src_x - pad + x)];
            int idx = (y * patch_w + x) * 4;
            patch[idx + 0] = v / 255.0f; patch[idx + 1] = patch[idx + 2] = 0; patch[idx + 3] = 1;
        }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, patch_w, patch_h, 0,
                 GL_RGBA, GL_FLOAT, patch);
    check_gl("upload patch");

    GLhandleARB prog = link_prog(vs_plain, fs_src);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glUniform1iARB(glGetUniformLocationARB(prog, "refTex"), 0);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(pad + 0.5f, pad + 0.5f); glVertex2f(0, 0);
    glTexCoord2f(pad + 4.5f, pad + 0.5f); glVertex2f(4, 0);
    glTexCoord2f(pad + 4.5f, pad + 4.5f); glVertex2f(4, 4);
    glTexCoord2f(pad + 0.5f, pad + 4.5f); glVertex2f(0, 4);
    glEnd();
    glFinish();
    check_gl("draw");

    unsigned char pixels[4 * 4 * 4];
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int gpu_out[16], mismatches = 0;
    printf("(b) GPU (%s):\n  ", label);
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            int idx = row * 4 + col;
            gpu_out[idx] = pixels[idx * 4 + 0];
            printf("%4d ", gpu_out[idx]);
            if (abs(gpu_out[idx] - cpu_out[idx]) > 1) mismatches++;
        }
    printf("\n%s: %s (%d/16 differ by >1)\n\n", label,
           mismatches == 0 ? "MATCH" : "MISMATCH", mismatches);

    glDeleteTextures(1, &tex);
    free(patch);
}

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
    for (uint32_t i = 0; i < mov.sample_count && !(g_frame_idx > 1 || (g_frame_idx == 1 && g_have_v && g_have_d)); i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (g_frame_idx == 0) {
                g_ref_stride = frame->linesize[0];
                g_ref_y = (unsigned char *)malloc((size_t)g_ref_stride * frame->height);
                memcpy(g_ref_y, frame->data[0], (size_t)g_ref_stride * frame->height);
            }
            g_frame_idx++;
            av_frame_unref(frame);
        }
    }
    if (!g_have_v || !g_have_d) { fprintf(stderr, "didn't find both target MBs\n"); return 1; }

    printf("Vertical case: MB(%d,%d) mv=(%d,%d)\n", g_v_mbx, g_v_mby, g_v_mvx, g_v_mvy);
    printf("Diagonal case: MB(%d,%d) mv=(%d,%d)\n\n", g_d_mbx, g_d_mby, g_d_mvx, g_d_mvy);

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    g_glctx = aglCreateContext(pf, NULL);
    aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf;
    aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(g_glctx, pbuf, 0, 0, 0);
    aglSetCurrentContext(g_glctx);

    int v_src_x = g_v_mbx * 16 + (g_v_mvx >> 2);
    int v_src_y = g_v_mby * 16 + (g_v_mvy >> 2);
    /* pad=3 (not 2): the 6-tap span for a 4-wide/tall output block needs
     * an asymmetric -2..+6 range (block width 4 + 3 extra beyond the far
     * edge), so a pad of 2 on both sides under-allocates by 1 on the far
     * side - use 3 both sides (safe, one texel of harmless extra margin
     * on the near side) rather than a tighter asymmetric allocation. */
    run_case("vertical half-pel", fs_vpel, v_lowpass_ref, v_src_x, v_src_y, 3);

    int d_src_x = g_d_mbx * 16 + (g_d_mvx >> 2);
    int d_src_y = g_d_mby * 16 + (g_d_mvy >> 2);
    run_case("diagonal (hv, 2-stage)", fs_diag, diag_ref, d_src_x, d_src_y, 3);

    aglSetCurrentContext(NULL);
    aglDestroyContext(g_glctx);
    return 0;
}
