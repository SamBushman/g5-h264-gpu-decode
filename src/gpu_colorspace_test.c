/*
 * gpu-colorspace-test: Milestone 9, colorspace half. Captures real
 * decoded Y/U/V plane data (4:2:0, from an actual AVFrame - no hook
 * needed here, this runs after full frames are already correctly
 * reconstructed by the untouched CPU decode path) and converts a 4x4
 * luma block (+ its 2x2 chroma block) to RGB via standard BT.601
 * limited-range, both as a CPU reference and a GLSL shader on the real
 * X1900, diffed. Small-magnitude linear combination (coefficients all
 * <2.1, inputs/outputs 0-255) - no large unrounded intermediate, so none
 * of quirk #14's FP24 precision risk applies here.
 */

#include "mp4box.h"
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

static int clip255i(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void yuv2rgb_ref(int Y, int U, int V, int *r, int *g, int *b) {
    int y = Y - 16, u = U - 128, v = V - 128;
    *r = clip255i((int)((1.164 * y + 1.596 * v) + 0.5));
    *g = clip255i((int)((1.164 * y - 0.392 * u - 0.813 * v) + 0.5));
    *b = clip255i((int)((1.164 * y + 2.017 * u) + 0.5));
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

/* Y and UV are two separate rectangle textures (matching how a real
 * decoder would keep 4:2:0 planes - no interleaving/packing). Each
 * fragment reads its own luma texel plus the correspondingly-halved
 * chroma texel (integer division by 2, exact for these small values). */
static const char *fs_yuv2rgb =
"uniform sampler2DRect yTex;\n"
"uniform sampler2DRect uTex;\n"
"uniform sampler2DRect vTex;\n"
"void main() {\n"
"  vec2 lumaPos = floor(gl_FragCoord.xy) + vec2(0.5,0.5);\n"
"  vec2 chromaPos = floor(lumaPos * 0.5) + vec2(0.5,0.5);\n"
"  float Y = texture2DRect(yTex, lumaPos).r * 255.0;\n"
"  float U = texture2DRect(uTex, chromaPos).r * 255.0;\n"
"  float V = texture2DRect(vTex, chromaPos).r * 255.0;\n"
"  float y = Y - 16.0, u = U - 128.0, v = V - 128.0;\n"
"  float r = 1.164*y + 1.596*v;\n"
"  float g = 1.164*y - 0.392*u - 0.813*v;\n"
"  float b = 1.164*y + 2.017*u;\n"
"  r = max(0.0, min(255.0, r)); g = max(0.0, min(255.0, g)); b = max(0.0, min(255.0, b));\n"
"  gl_FragColor = vec4(r/255.0, g/255.0, b/255.0, 1.0);\n"
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

    AVPacket *pkt = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    int got = 0;
    unsigned char *Yp = NULL, *Up = NULL, *Vp = NULL;
    int ys, us, vs;
    for (uint32_t i = 0; i < mov.sample_count && !got; i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt); av_packet_unref(pkt);
        if (avcodec_receive_frame(ctx, frame) == 0) {
            ys = frame->linesize[0]; us = frame->linesize[1]; vs = frame->linesize[2];
            Yp = (unsigned char *)malloc((size_t)ys * frame->height);
            Up = (unsigned char *)malloc((size_t)us * frame->height / 2);
            Vp = (unsigned char *)malloc((size_t)vs * frame->height / 2);
            memcpy(Yp, frame->data[0], (size_t)ys * frame->height);
            memcpy(Up, frame->data[1], (size_t)us * frame->height / 2);
            memcpy(Vp, frame->data[2], (size_t)vs * frame->height / 2);
            got = 1;
            av_frame_unref(frame);
        }
    }
    if (!got) { fprintf(stderr, "no frame decoded\n"); return 1; }

    /* Pick a real, non-edge 8x8 luma region (needs even alignment for
     * clean 4x4 chroma correspondence) well inside the frame. */
    int ox = 64, oy = 32, N = 8;
    printf("Real captured YUV block at (%d,%d), %dx%d:\n", ox, oy, N, N);

    int cpu_r[8][8], cpu_g[8][8], cpu_b[8][8];
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int Y = Yp[(oy+y)*ys + (ox+x)];
            int U = Up[((oy+y)/2)*us + (ox+x)/2];
            int V = Vp[((oy+y)/2)*vs + (ox+x)/2];
            yuv2rgb_ref(Y, U, V, &cpu_r[y][x], &cpu_g[y][x], &cpu_b[y][x]);
        }
    printf("(a) CPU reference (row 0): ");
    for (int x = 0; x < N; x++) printf("(%d,%d,%d) ", cpu_r[0][x], cpu_g[0][x], cpu_b[0][x]);
    printf("\n");

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext glctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(16, 16, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(glctx, pbuf, 0, 0, 0); aglSetCurrentContext(glctx);

    glViewport(0, 0, N, N);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, N, 0, N, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    /* Upload luma (NxN) and chroma (N/2 x N/2) as separate float
     * rectangle textures, cropped to exactly this region (fragment 0,0
     * = ox,oy directly, no extra pad offset needed - no filter taps
     * beyond the texel itself for a pure colorspace conversion). */
    float *yd = (float *)malloc(sizeof(float) * N * N * 4);
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int idx = (y*N+x)*4; float v = Yp[(oy+y)*ys+(ox+x)] / 255.0f;
        yd[idx]=v; yd[idx+1]=yd[idx+2]=0; yd[idx+3]=1;
    }
    float *ud = (float *)malloc(sizeof(float) * (N/2) * (N/2) * 4);
    float *vd = (float *)malloc(sizeof(float) * (N/2) * (N/2) * 4);
    for (int y = 0; y < N/2; y++) for (int x = 0; x < N/2; x++) {
        int idx = (y*(N/2)+x)*4;
        ud[idx] = Up[((oy/2)+y)*us+((ox/2)+x)] / 255.0f; ud[idx+1]=ud[idx+2]=0; ud[idx+3]=1;
        vd[idx] = Vp[((oy/2)+y)*vs+((ox/2)+x)] / 255.0f; vd[idx+1]=vd[idx+2]=0; vd[idx+3]=1;
    }
    GLuint yTex, uTex, vTex;
    glGenTextures(1, &yTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, yTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, N, N, 0, GL_RGBA, GL_FLOAT, yd);
    glGenTextures(1, &uTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, uTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, N/2, N/2, 0, GL_RGBA, GL_FLOAT, ud);
    glGenTextures(1, &vTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, vTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, N/2, N/2, 0, GL_RGBA, GL_FLOAT, vd);
    checkgl("upload planes");

    GLhandleARB prog = linkp(vs_plain, fs_yuv2rgb);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, yTex);
    glUniform1iARB(glGetUniformLocationARB(prog, "yTex"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, uTex);
    glUniform1iARB(glGetUniformLocationARB(prog, "uTex"), 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, vTex);
    glUniform1iARB(glGetUniformLocationARB(prog, "vTex"), 2);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(N, 0); glVertex2f(N, N); glVertex2f(0, N);
    glEnd(); glFinish(); checkgl("draw");

    unsigned char pixels[8*8*4];
    glReadPixels(0, 0, N, N, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    printf("(b) GPU       (row 0): ");
    int mismatches = 0;
    /* row 0 in the output buffer = window-bottom = our uploaded row 0
     * (top reference row), matching every prior test's convention. */
    for (int x = 0; x < N; x++) {
        int idx = (0*N+x)*4;
        int r = pixels[idx], g = pixels[idx+1], b = pixels[idx+2];
        printf("(%d,%d,%d) ", r, g, b);
        if (abs(r-cpu_r[0][x]) > 1 || abs(g-cpu_g[0][x]) > 1 || abs(b-cpu_b[0][x]) > 1) mismatches++;
    }
    printf("\n");
    for (int row = 0; row < N; row++) for (int x = 0; x < N; x++) {
        int idx = (row*N+x)*4;
        int r = pixels[idx], g = pixels[idx+1], b = pixels[idx+2];
        if (abs(r-cpu_r[row][x]) > 1 || abs(g-cpu_g[row][x]) > 1 || abs(b-cpu_b[row][x]) > 1) {
            if (row != 0) mismatches++; /* row 0 already counted above */
        }
    }

    printf("\n%s (%d/%d values differ by >1)\n",
           mismatches == 0 ? "RESULT: GPU YUV->RGB matches CPU reference" : "RESULT: MISMATCH",
           mismatches, N*N);

    aglSetCurrentContext(NULL);
    aglDestroyContext(glctx);
    return mismatches != 0;
}
