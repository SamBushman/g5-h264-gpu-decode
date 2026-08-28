/*
 * gpu-present-test: Milestone 9 capstone. Decodes a real frame from the
 * test clip (stock FFmpeg CPU path - the x1900_hook isn't wired to take
 * over reconstruction yet, that's future integration work), runs the
 * already-verified YUV->RGB GLSL shader on the real X1900 in a real
 * on-screen NSOpenGLView window (not an offscreen Pbuffer), and leaves
 * it on screen for visual confirmation before exiting.
 */

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>

#include "mp4box.h"
#include <libavcodec/avcodec.h>
#include <string.h>

static const char *vs_plain = "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex; gl_TexCoord[0]=gl_MultiTexCoord0;}";

static const char *fs_yuv2rgb =
"uniform sampler2DRect yTex;\n"
"uniform sampler2DRect uTex;\n"
"uniform sampler2DRect vTex;\n"
"void main() {\n"
"  vec2 lumaPos = gl_TexCoord[0].xy;\n"
"  vec2 chromaPos = lumaPos * 0.5;\n"
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

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.mp4> [frame_number]\n", argv[0]); return 1; }
    int want_frame = argc > 2 ? atoi(argv[2]) : 30;

    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) { fprintf(stderr, "demux failed\n"); return 1; }
    int alen = 0; unsigned char *avcc = mp4_build_avcc(&mov, &alen);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(alen + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)alen); ctx->extradata_size = alen;
    avcodec_open2(ctx, codec, NULL);

    AVPacket *pkt = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    int frame_idx = 0, got = 0;
    unsigned char *Yp = NULL, *Up = NULL, *Vp = NULL;
    int ys = 0, us = 0, vs = 0, W = mov.width, H = mov.height;
    for (uint32_t i = 0; i < mov.sample_count && !got; i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt); av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (frame_idx == want_frame) {
                ys = frame->linesize[0]; us = frame->linesize[1]; vs = frame->linesize[2];
                Yp = (unsigned char *)malloc((size_t)ys * H);
                Up = (unsigned char *)malloc((size_t)us * H / 2);
                Vp = (unsigned char *)malloc((size_t)vs * H / 2);
                memcpy(Yp, frame->data[0], (size_t)ys * H);
                memcpy(Up, frame->data[1], (size_t)us * H / 2);
                memcpy(Vp, frame->data[2], (size_t)vs * H / 2);
                got = 1;
            }
            frame_idx++;
            av_frame_unref(frame);
        }
    }
    if (!got) { fprintf(stderr, "frame %d not reached (only decoded %d)\n", want_frame, frame_idx); return 1; }
    fprintf(stderr, "decoded frame %d (%dx%d), presenting on screen\n", want_frame, W, H);

    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    NSRect frect = NSMakeRect(100, 100, W, H);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frect
        styleMask:(NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask)
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:@"X1900 GPU Decode - real decoded frame"];
    NSOpenGLPixelFormatAttribute attrs[] = { NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0 };
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    NSOpenGLView *glView = [[NSOpenGLView alloc] initWithFrame:[[window contentView] bounds] pixelFormat:pf];
    [window setContentView:glView];
    [[glView openGLContext] makeCurrentContext];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    /* Upload full-frame Y/U/V planes as float rectangle textures (real
     * decoded pixel data, not synthetic). */
    float *yd = (float *)malloc(sizeof(float) * W * H * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int idx = (y*W+x)*4; float v = Yp[y*ys+x] / 255.0f;
        yd[idx]=v; yd[idx+1]=yd[idx+2]=0; yd[idx+3]=1;
    }
    int cw = W/2, ch = H/2;
    float *ud = (float *)malloc(sizeof(float) * cw * ch * 4);
    float *vd = (float *)malloc(sizeof(float) * cw * ch * 4);
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
        int idx = (y*cw+x)*4;
        ud[idx] = Up[y*us+x] / 255.0f; ud[idx+1]=ud[idx+2]=0; ud[idx+3]=1;
        vd[idx] = Vp[y*vs+x] / 255.0f; vd[idx+1]=vd[idx+2]=0; vd[idx+3]=1;
    }
    GLuint yTex, uTex, vTex;
    glGenTextures(1, &yTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, yTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, W, H, 0, GL_RGBA, GL_FLOAT, yd);
    glGenTextures(1, &uTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, uTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, cw, ch, 0, GL_RGBA, GL_FLOAT, ud);
    glGenTextures(1, &vTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, vTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, cw, ch, 0, GL_RGBA, GL_FLOAT, vd);

    GLhandleARB prog = linkp(vs_plain, fs_yuv2rgb);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, yTex);
    glUniform1iARB(glGetUniformLocationARB(prog, "yTex"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, uTex);
    glUniform1iARB(glGetUniformLocationARB(prog, "uTex"), 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, vTex);
    glUniform1iARB(glGetUniformLocationARB(prog, "vTex"), 2);

    for (int i = 0; i < 40; i++) { /* ~20s on screen */
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        /* flip V so the image isn't upside down: row 0 (top of source
         * image) should land at the TOP of the window, but window-space
         * y=0 is the bottom - so texcoord v increases as vertex y
         * decreases, matching source row order to visual top-down. */
        glTexCoord2f(0, H);   glVertex2f(0, 0);
        glTexCoord2f(W, H);   glVertex2f(W, 0);
        glTexCoord2f(W, 0);   glVertex2f(W, H);
        glTexCoord2f(0, 0);   glVertex2f(0, H);
        glEnd();
        [[glView openGLContext] flushBuffer];
        fprintf(stderr, "presented frame, tick %d\n", i);
        fflush(stderr);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    }

    fprintf(stderr, "done, exiting cleanly\n");
    [pool release];
    return 0;
}
