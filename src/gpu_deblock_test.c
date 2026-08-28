/*
 * gpu-deblock-test: Milestone 8 (scoped start). Captures ONE real
 * macroblock vertical-luma-edge filter call (real alpha/beta/tc0, real
 * unfiltered pixel window) via the new x1900 deblock hook, fired right
 * before FFmpeg's own CPU h264_v_loop_filter_luma would run (h264_loop-
 * filter.c's filter_mb_edgeh, the "normal" bS<4 luma path - bS=4 strong
 * intra filtering and chroma are out of scope for this pass, matching
 * the plan's pre-approved "start with a representative case" scope).
 *
 * Same methodology as M6/M7: byte-for-byte CPU port of the real FFmpeg
 * function (h264dsp_template.c's h264_loop_filter_luma) as the oracle,
 * fed the SAME real captured alpha/beta/tc0 + pixel window as the GPU
 * shader, diffed directly - not trying to correlate against FFmpeg's own
 * live output buffer after the fact (which would need fragile pointer
 * tracking across the receive_frame boundary; unnecessary when we
 * already have a faithful oracle for the same real inputs).
 *
 * Only the FIRST 4-row group of a full 16-row macroblock edge is tested
 * (tc0[0], rows 0-3) - a representative slice of the filter, not the
 * whole macroblock-tall edge; see the plan for why.
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

/* ---- capture ---- */

static int g_captured = 0;
static int g_alpha, g_beta;
static int8_t g_tc0[4];
static int g_window[4][6]; /* [row][p2,p1,p0,q0,q1,q2] */

static int deblock_hook(uint8_t *pix, int stride, int alpha, int beta,
                         const int8_t *tc0, void *userdata) {
    (void)userdata;
    if (g_captured || tc0[0] <= 0) return 0; /* tc0>0 so the p1'/q1' branches
        are also exercised, not just the p0'/q0' delta - a stronger test */
    /* Only accept if at least one row produces a genuinely NONZERO pixel
     * change (not just "condition triggers but delta rounds to zero") -
     * want real, visible evidence the modification math itself is
     * exercised and correct, not just the pass-through path. */
    int any_changed = 0;
    for (int row = 0; row < 4; row++) {
        uint8_t *p = pix + row * stride;
        int win[6] = {p[-3], p[-2], p[-1], p[0], p[1], p[2]};
        int out[4];
        /* reuse the same reference function declared below via forward
         * use is not possible in C89 order, so inline the same check: */
        int p2=win[0],p1=win[1],p0=win[2],q0=win[3],q1=win[4],q2=win[5];
        (void)p2; (void)q2; (void)out;
        if (abs(p0-q0) < alpha && abs(p1-p0) < beta && abs(q1-q0) < beta) {
            int tc = tc0[0];
            if (abs(p2-p0) < beta) tc++;
            if (abs(q2-q0) < beta) tc++;
            int delta = ((q0-p0)*4 + (p1-q1) + 4) >> 3;
            delta = delta < -tc ? -tc : (delta > tc ? tc : delta);
            if (delta != 0) any_changed = 1;
        }
    }
    if (!any_changed) return 0;
    g_alpha = alpha; g_beta = beta;
    memcpy(g_tc0, tc0, sizeof(g_tc0));
    for (int row = 0; row < 4; row++) {
        uint8_t *p = pix + row * stride;
        g_window[row][0] = p[-3]; g_window[row][1] = p[-2]; g_window[row][2] = p[-1];
        g_window[row][3] = p[0];  g_window[row][4] = p[1];  g_window[row][5] = p[2];
    }
    g_captured = 1;
    return 0; /* observe only - real filter still runs normally */
}

/* ---- (a) CPU reference: byte-for-byte port of h264_loop_filter_luma
 * (h264dsp_template.c), single tc0 value, 4 rows, BIT_DEPTH=8 so the
 * <<=(BIT_DEPTH-8) shifts are no-ops and omitted. ---- */

static int clip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clip255(int v) { return clip(v, 0, 255); }

static void loop_filter_luma_ref(int alpha, int beta, int tc_orig, const int in[6], int out[4]) {
    int p2 = in[0], p1 = in[1], p0 = in[2], q0 = in[3], q1 = in[4], q2 = in[5];
    out[0] = p1; out[1] = p0; out[2] = q0; out[3] = q1; /* default: unchanged */
    if (tc_orig < 0) return;
    if (abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta) {
        int tc = tc_orig;
        int out_p1 = p1, out_q1 = q1;
        if (abs(p2 - p0) < beta) {
            if (tc_orig) out_p1 = p1 + clip((((p2 + ((p0 + q0 + 1) >> 1)) >> 1) - p1), -tc_orig, tc_orig);
            tc++;
        }
        if (abs(q2 - q0) < beta) {
            if (tc_orig) out_q1 = q1 + clip((((q2 + ((p0 + q0 + 1) >> 1)) >> 1) - q1), -tc_orig, tc_orig);
            tc++;
        }
        int delta = clip((((q0 - p0) * 4) + (p1 - q1) + 4) >> 3, -tc, tc);
        out[0] = out_p1; out[1] = clip255(p0 + delta); out[2] = clip255(q0 - delta); out[3] = out_q1;
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

/* One fragment per output row (4 rows, viewport 1x4... use 4x4 and only
 * read column 0 for simplicity, matching earlier tests' style). Each
 * fragment independently reads its own row's p2..q2 from a 6x4 input
 * texture and computes the full filter - no cross-row dependency in this
 * "normal" luma filter (each row is independent; only different rows
 * potentially use a different tc0[i] group, which this scoped test keeps
 * fixed at tc0[0] for all 4 rows). Scalar ops only, no matrix math
 * (quirk #12); no texture2DRect calls inside a branch (quirk #2) - all
 * six taps are fetched unconditionally before any conditional logic. */
static const char *fs_deblock =
"uniform sampler2DRect winTex;\n"
"uniform float alpha, beta, tc0;\n"
"void main() {\n"
"  float row = floor(gl_FragCoord.y);\n"
"  float p2 = texture2DRect(winTex, vec2(0.5, row+0.5)).r*255.0;\n"
"  float p1 = texture2DRect(winTex, vec2(1.5, row+0.5)).r*255.0;\n"
"  float p0 = texture2DRect(winTex, vec2(2.5, row+0.5)).r*255.0;\n"
"  float q0 = texture2DRect(winTex, vec2(3.5, row+0.5)).r*255.0;\n"
"  float q1 = texture2DRect(winTex, vec2(4.5, row+0.5)).r*255.0;\n"
"  float q2 = texture2DRect(winTex, vec2(5.5, row+0.5)).r*255.0;\n"
"  float outP1 = p1, outP0 = p0, outQ0 = q0, outQ1 = q1;\n"
"  if (tc0 >= 0.0) {\n"
"    if (abs(p0-q0) < alpha && abs(p1-p0) < beta && abs(q1-q0) < beta) {\n"
"      float tc = tc0;\n"
"      if (abs(p2-p0) < beta) {\n"
"        if (tc0 > 0.0) { float d = floor((p2+floor((p0+q0+1.0)/2.0))/2.0) - p1;\n"
"          d = max(-tc0, min(tc0, d)); outP1 = p1 + d; }\n"
"        tc += 1.0;\n"
"      }\n"
"      if (abs(q2-q0) < beta) {\n"
"        if (tc0 > 0.0) { float d = floor((q2+floor((p0+q0+1.0)/2.0))/2.0) - q1;\n"
"          d = max(-tc0, min(tc0, d)); outQ1 = q1 + d; }\n"
"        tc += 1.0;\n"
"      }\n"
"      float delta = floor((((q0-p0)*4.0)+(p1-q1)+4.0)/8.0);\n"
"      delta = max(-tc, min(tc, delta));\n"
"      outP0 = max(0.0, min(255.0, p0+delta));\n"
"      outQ0 = max(0.0, min(255.0, q0-delta));\n"
"    }\n"
"  }\n"
/* Pack all 4 outputs into the 4 color channels of ONE fragment - avoids
 * needing 4 separate output positions/passes for this small test. */
"  gl_FragColor = vec4(outP1/255.0, outP0/255.0, outQ0/255.0, outQ1/255.0);\n"
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
    ff_x1900_set_deblock_hook(deblock_hook, NULL);

    AVPacket *pkt = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    for (uint32_t i = 0; i < mov.sample_count && !g_captured; i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt); av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) av_frame_unref(frame);
    }
    if (!g_captured) { fprintf(stderr, "no active deblock edge found\n"); return 1; }

    printf("Captured real deblock edge: alpha=%d beta=%d tc0=[%d %d %d %d]\n",
           g_alpha, g_beta, g_tc0[0], g_tc0[1], g_tc0[2], g_tc0[3]);
    printf("Real unfiltered window (p2 p1 p0 | q0 q1 q2) per row:\n");
    for (int r = 0; r < 4; r++)
        printf("  row%d: %3d %3d %3d | %3d %3d %3d\n", r,
               g_window[r][0], g_window[r][1], g_window[r][2],
               g_window[r][3], g_window[r][4], g_window[r][5]);

    /* (a) CPU reference, using tc0[0] for all 4 rows (scoped test) */
    int cpu_out[4][4];
    printf("\n(a) CPU reference (byte-for-byte port of h264_loop_filter_luma):\n");
    for (int r = 0; r < 4; r++) {
        loop_filter_luma_ref(g_alpha, g_beta, g_tc0[0], g_window[r], cpu_out[r]);
        printf("  row%d: p1'=%3d p0'=%3d q0'=%3d q1'=%3d\n", r,
               cpu_out[r][0], cpu_out[r][1], cpu_out[r][2], cpu_out[r][3]);
    }

    /* (b) GPU */
    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext glctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(16, 16, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(glctx, pbuf, 0, 0, 0); aglSetCurrentContext(glctx);

    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    float win[6 * 4 * 4];
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 6; col++) {
            int idx = (row * 6 + col) * 4;
            win[idx + 0] = g_window[row][col] / 255.0f;
            win[idx + 1] = win[idx + 2] = 0; win[idx + 3] = 1;
        }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, 6, 4, 0, GL_RGBA, GL_FLOAT, win);
    checkgl("upload window");

    GLhandleARB prog = linkp(vs_plain, fs_deblock);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glUniform1iARB(glGetUniformLocationARB(prog, "winTex"), 0);
    glUniform1fARB(glGetUniformLocationARB(prog, "alpha"), (float)g_alpha);
    glUniform1fARB(glGetUniformLocationARB(prog, "beta"), (float)g_beta);
    glUniform1fARB(glGetUniformLocationARB(prog, "tc0"), (float)g_tc0[0]);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4);
    glEnd(); glFinish(); checkgl("draw");

    unsigned char pixels[4 * 4 * 4];
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    printf("\n(b) GPU (real X1900 hardware):\n");
    int mismatches = 0;
    for (int row = 0; row < 4; row++) {
        /* window texture row 'row' maps directly to output row 'row' via
         * floor(gl_FragCoord.y) - all 4 columns of this viewport row
         * compute the identical result (no per-column variation needed
         * for this scoped test), read column 0. */
        int idx = (row * 4 + 0) * 4;
        int gp1 = pixels[idx + 0], gp0 = pixels[idx + 1], gq0 = pixels[idx + 2], gq1 = pixels[idx + 3];
        printf("  row%d: p1'=%3d p0'=%3d q0'=%3d q1'=%3d\n", row, gp1, gp0, gq0, gq1);
        if (abs(gp1 - cpu_out[row][0]) > 1) mismatches++;
        if (abs(gp0 - cpu_out[row][1]) > 1) mismatches++;
        if (abs(gq0 - cpu_out[row][2]) > 1) mismatches++;
        if (abs(gq1 - cpu_out[row][3]) > 1) mismatches++;
    }

    printf("\n%s (%d/16 values differ by >1)\n",
           mismatches == 0 ? "RESULT: GPU deblock filter matches CPU reference" : "RESULT: MISMATCH",
           mismatches);

    aglSetCurrentContext(NULL);
    aglDestroyContext(glctx);
    return mismatches != 0;
}
