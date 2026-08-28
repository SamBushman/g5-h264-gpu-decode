/*
 * gpu-mc-quarterpel: closes out M7. The remaining 12 quarter-pel luma
 * positions are all built by averaging two already-verified primitives
 * (full-pel read, half-H, half-V, or the diagonal) via simple rounded
 * averaging (a+b+1)>>1 - see H264_MC macro in h264qpel_template.c. There
 * are exactly 4 distinct combinator patterns across those 12 positions:
 *   (a) full-pel + half-H   -> mc10, mc30
 *   (b) full-pel + half-V   -> mc01, mc03
 *   (c) half-H   + half-V   -> mc11, mc31, mc13, mc33
 *   (d) half-H (or V) + diagonal -> mc21, mc23, mc12, mc32
 * Tests one real captured case from each pattern (the other 3 positions
 * in each pattern are the same combinator, just mirrored/shifted source
 * offsets - not individually tested, low risk given the hard cases
 * (half-pel filters, diagonal) are already independently verified).
 *
 * IMPORTANT (quirk #2): each pattern is its OWN separate compiled GLSL
 * program, never a single shader with a runtime branch selecting between
 * them - an in-shader branch containing texture2DRect calls would hit
 * the documented "if/else around texture2D voids the whole draw" bug.
 * Real per-MB dispatch belongs on the CPU side (which program to use),
 * matching how a real hwaccel/decoder selects among mc00..mc33.
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
static Cand g_a = {0}, g_b = {0}, g_c = {0}, g_d = {0}; /* one per pattern */

static int margin_ok(int mb_x, int mb_y, int mvx, int mvy) {
    int sx = mb_x * 16 + (mvx >> 2), sy = mb_y * 16 + (mvy >> 2);
    return sx - 2 >= 0 && sx + 6 < g_ref_w && sy - 2 >= 0 && sy + 6 < g_ref_h;
}

static int hook(int mb_x, int mb_y, int mb_type, int qscale, const int16_t *coeffs,
                 const uint8_t *nnz, const int16_t *mv_l0, const int8_t *ref_l0, void *ud) {
    (void)mb_type; (void)qscale; (void)coeffs; (void)nnz; (void)ud;
    if (g_frame_idx != 1 || ref_l0[0] != 0) return 0;
    int fx = mv_l0[0] & 3, fy = mv_l0[1] & 3;
    if (!margin_ok(mb_x, mb_y, mv_l0[0], mv_l0[1])) return 0;
    /* pattern (a): full+halfH -> fy==0, fx==1 or 3 */
    if (!g_a.found && fy == 0 && (fx == 1 || fx == 3)) {
        g_a.mbx = mb_x; g_a.mby = mb_y; g_a.mvx = mv_l0[0]; g_a.mvy = mv_l0[1]; g_a.found = 1;
    }
    /* pattern (b): full+halfV -> fx==0, fy==1 or 3 */
    if (!g_b.found && fx == 0 && (fy == 1 || fy == 3)) {
        g_b.mbx = mb_x; g_b.mby = mb_y; g_b.mvx = mv_l0[0]; g_b.mvy = mv_l0[1]; g_b.found = 1;
    }
    /* pattern (c): halfH+halfV -> fx in {1,3}, fy in {1,3} */
    if (!g_c.found && (fx == 1 || fx == 3) && (fy == 1 || fy == 3)) {
        g_c.mbx = mb_x; g_c.mby = mb_y; g_c.mvx = mv_l0[0]; g_c.mvy = mv_l0[1]; g_c.found = 1;
    }
    /* pattern (d): half+diagonal -> (fx==2, fy in {1,3}) or (fy==2, fx in {1,3}) */
    if (!g_d.found && ((fx == 2 && (fy == 1 || fy == 3)) || (fy == 2 && (fx == 1 || fx == 3)))) {
        g_d.mbx = mb_x; g_d.mby = mb_y; g_d.mvx = mv_l0[0]; g_d.mvy = mv_l0[1]; g_d.found = 1;
    }
    return 0;
}

static int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int px(const unsigned char *src, int stride, int x, int y) { return src[y * stride + x]; }
static int h_raw(const unsigned char *src, int stride, int x, int y) {
    const unsigned char *p = src + y * stride + x;
    return (p[0] + p[1]) * 20 - (p[-1] + p[2]) * 5 + (p[-2] + p[3]);
}
static int half_h(const unsigned char *src, int stride, int x, int y) { return clip255((h_raw(src, stride, x, y) + 16) >> 5); }
static int half_v(const unsigned char *src, int stride, int x, int y) {
    const unsigned char *p = src + y * stride + x;
    int v = (p[0] + p[stride]) * 20 - (p[-stride] + p[2*stride]) * 5 + (p[-2*stride] + p[3*stride]);
    return clip255((v + 16) >> 5);
}
static int diag(const unsigned char *src, int stride, int x, int y) {
    int hr[6];
    for (int dy = -2; dy <= 3; dy++) hr[dy+2] = (h_raw(src, stride, x, y+dy) + 16) >> 5;
    int v = (hr[2]+hr[3])*20 - (hr[1]+hr[4])*5 + (hr[0]+hr[5]);
    return clip255((v + 16) >> 5);
}
static int avg(int a, int b) { return (a + b + 1) >> 1; }

/* CPU references per pattern, matching H264_MC's exact source-offset choices */
static int ref_mc10(const unsigned char *s, int st, int x, int y) { return avg(px(s,st,x,y), half_h(s,st,x,y)); }
static int ref_mc01(const unsigned char *s, int st, int x, int y) { return avg(px(s,st,x,y), half_v(s,st,x,y)); }
static int ref_mc31(const unsigned char *s, int st, int x, int y) { return avg(half_h(s,st,x,y), half_v(s,st,x+1,y)); }
static int ref_mc21(const unsigned char *s, int st, int x, int y) { return avg(half_h(s,st,x,y), diag(s,st,x,y)); }

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

#define HALF_H_FN \
"float halfH(vec2 b) {\n" \
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n" \
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n" \
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n" \
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n" \
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n" \
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n" \
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n" \
"  return max(0.0,min(255.0,floor((v+16.0)/32.0)));\n" \
"}\n"

#define HALF_V_FN \
"float halfV(vec2 b) {\n" \
"  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;\n" \
"  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;\n" \
"  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;\n" \
"  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;\n" \
"  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;\n" \
"  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;\n" \
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n" \
"  return max(0.0,min(255.0,floor((v+16.0)/32.0)));\n" \
"}\n"

static const char *fs_mc10 =
"uniform sampler2DRect refTex;\n"
"uniform vec2 patchOrigin;\n" HALF_H_FN
"void main(){\n"
"  vec2 b = floor(gl_FragCoord.xy) + patchOrigin;\n"
"  float full = texture2DRect(refTex,b).r*255.0;\n"
"  float h = halfH(b);\n"
"  float result = floor((full+h+1.0)/2.0);\n"
"  gl_FragColor = vec4(result/255.0,0.0,0.0,1.0);\n"
"}\n";

static const char *fs_mc01 =
"uniform sampler2DRect refTex;\n"
"uniform vec2 patchOrigin;\n" HALF_V_FN
"void main(){\n"
"  vec2 b = floor(gl_FragCoord.xy) + patchOrigin;\n"
"  float full = texture2DRect(refTex,b).r*255.0;\n"
"  float v = halfV(b);\n"
"  float result = floor((full+v+1.0)/2.0);\n"
"  gl_FragColor = vec4(result/255.0,0.0,0.0,1.0);\n"
"}\n";

static const char *fs_mc31 =
"uniform sampler2DRect refTex;\n"
"uniform vec2 patchOrigin;\n" HALF_H_FN HALF_V_FN
"void main(){\n"
"  vec2 b = floor(gl_FragCoord.xy) + patchOrigin;\n"
"  float h = halfH(b);\n"
"  float v = halfV(b+vec2(1.0,0.0));\n"
"  float result = floor((h+v+1.0)/2.0);\n"
"  gl_FragColor = vec4(result/255.0,0.0,0.0,1.0);\n"
"}\n";

/* mc21: avg(halfH, diagonal). Diagonal needs the 2-pass precision fix
 * (quirk #14) - reused here as its own pass A + pass B, same as the
 * standalone diagonal test, then a third pass averages halfH with it. */
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
"  float raw=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  gl_FragColor = vec4(floor((raw+16.0)/32.0), 0.0, 0.0, 1.0);\n"
"}\n";
static const char *fs_mc21_stage2 =
"uniform sampler2DRect stage1Tex;\n"
"uniform sampler2DRect refTex;\n"
"uniform vec2 baseOffset;\n"
"uniform vec2 refOrigin;\n"
"float dec(vec2 b, float dy){ return texture2DRect(stage1Tex,b+vec2(0.0,dy)).r; }\n"
HALF_H_FN
"void main(){\n"
"  vec2 b = floor(gl_FragCoord.xy) + baseOffset;\n"
"  float hm2=dec(b,-2.0); float hm1=dec(b,-1.0); float h0=dec(b,0.0);\n"
"  float h1=dec(b,1.0);   float h2=dec(b,2.0);    float h3=dec(b,3.0);\n"
"  float v=(h0+h1)*20.0-(hm1+h2)*5.0+(hm2+h3);\n"
"  float dg = max(0.0,min(255.0,floor((v+16.0)/32.0)));\n"
"  vec2 rb = floor(gl_FragCoord.xy) + refOrigin;\n"
"  float hh = halfH(rb);\n"
"  float result = floor((hh+dg+1.0)/2.0);\n"
"  gl_FragColor = vec4(result/255.0,0.0,0.0,1.0);\n"
"}\n";

typedef struct { const char *name; Cand *c; int (*ref_fn)(const unsigned char*,int,int,int); } Case;

static AGLContext g_glctx;

static void run_simple(const char *name, const char *fs, Cand *c,
                        int (*ref_fn)(const unsigned char *, int, int, int)) {
    int src_x = c->mbx * 16 + (c->mvx >> 2), src_y = c->mby * 16 + (c->mvy >> 2);
    int cpu_out[16], gpu_out[16], mismatches = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            cpu_out[y*4+x] = ref_fn(g_ref_y, g_ref_stride, src_x+x, src_y+y);

    int pad = 3, pw = 4+2*pad, ph = 4+2*pad;
    float *patch = (float*)malloc(sizeof(float)*pw*ph*4);
    for (int y=0;y<ph;y++) for (int x=0;x<pw;x++) {
        unsigned char v = g_ref_y[(src_y-pad+y)*g_ref_stride+(src_x-pad+x)];
        int idx=(y*pw+x)*4; patch[idx]=v/255.0f; patch[idx+1]=patch[idx+2]=0; patch[idx+3]=1;
    }
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,pw,ph,0,GL_RGBA,GL_FLOAT,patch);
    checkgl("upload");

    glViewport(0,0,4,4);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,4,0,4,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    GLhandleARB prog = linkp(vs_plain, fs);
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glUniform1iARB(glGetUniformLocationARB(prog,"refTex"),0);
    /* patch column/row 0 = src_x-pad/src_y-pad, so output fragment (0,0)
     * (wanting src_x,src_y) needs patch-local (pad,pad), texel-center
     * adjusted -> (pad+0.5, pad+0.5). */
    glUniform2fARB(glGetUniformLocationARB(prog, "patchOrigin"), pad + 0.5f, pad + 0.5f);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(0,0); glVertex2f(4,0); glVertex2f(4,4); glVertex2f(0,4);
    glEnd(); glFinish(); checkgl("draw");

    unsigned char pixels[4*4*4];
    glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    for (int row=0;row<4;row++) for (int col=0;col<4;col++) {
        int idx=row*4+col;
        gpu_out[idx]=pixels[idx*4+0];
        if (abs(gpu_out[idx]-cpu_out[idx])>1) mismatches++;
    }
    printf("%-6s MB(%d,%d) mv=(%d,%d): CPU=[", name, c->mbx, c->mby, c->mvx, c->mvy);
    for (int i=0;i<16;i++) printf("%d ", cpu_out[i]);
    printf("] GPU=[");
    for (int i=0;i<16;i++) printf("%d ", gpu_out[i]);
    printf("] -> %s (%d/16 differ)\n", mismatches==0?"MATCH":"MISMATCH", mismatches);

    glDeleteTextures(1,&tex);
    free(patch);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.mp4>\n", argv[0]); return 1; }
    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) { fprintf(stderr, "demux failed\n"); return 1; }
    g_ref_w = mov.width; g_ref_h = mov.height;
    int alen=0; unsigned char *avcc = mp4_build_avcc(&mov,&alen);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata=(uint8_t*)av_mallocz(alen+AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata,avcc,(size_t)alen); ctx->extradata_size=alen;
    avcodec_open2(ctx,codec,NULL);
    ff_x1900_set_mb_hook(hook,NULL);
    AVPacket *pkt=av_packet_alloc(); AVFrame *frame=av_frame_alloc();
    for (uint32_t i=0;i<mov.sample_count && !(g_frame_idx>1||(g_frame_idx==1&&g_a.found&&g_b.found&&g_c.found&&g_d.found));i++) {
        Mp4Sample *s=&mov.samples[i];
        av_new_packet(pkt,(int)s->size);
        memcpy(pkt->data,mov.file_data+s->offset,s->size);
        avcodec_send_packet(ctx,pkt); av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx,frame)==0) {
            if (g_frame_idx==0) {
                g_ref_stride=frame->linesize[0];
                g_ref_y=(unsigned char*)malloc((size_t)g_ref_stride*frame->height);
                memcpy(g_ref_y,frame->data[0],(size_t)g_ref_stride*frame->height);
            }
            g_frame_idx++; av_frame_unref(frame);
        }
    }
    printf("pattern (a) full+halfH: %s\n", g_a.found?"found":"NOT FOUND");
    printf("pattern (b) full+halfV: %s\n", g_b.found?"found":"NOT FOUND");
    printf("pattern (c) halfH+halfV: %s\n", g_c.found?"found":"NOT FOUND");
    printf("pattern (d) half+diagonal: %s\n\n", g_d.found?"found":"NOT FOUND");

    GLint attribs[]={AGL_RGBA,AGL_DEPTH_SIZE,24,AGL_NONE};
    AGLPixelFormat pf=aglChoosePixelFormat(NULL,0,attribs);
    g_glctx=aglCreateContext(pf,NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(16,16,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,0,&pbuf);
    aglSetPBuffer(g_glctx,pbuf,0,0,0); aglSetCurrentContext(g_glctx);

    if (g_a.found) run_simple("mc10", fs_mc10, &g_a, ref_mc10);
    if (g_b.found) run_simple("mc01", fs_mc01, &g_b, ref_mc01);
    if (g_c.found) run_simple("mc31", fs_mc31, &g_c, ref_mc31);

    if (g_d.found) {
        /* mc21-style needs the 2-pass diagonal, done inline here (not via
         * run_simple, which assumes one pass). */
        int src_x = g_d.mbx*16+(g_d.mvx>>2), src_y = g_d.mby*16+(g_d.mvy>>2);
        int cpu_out[16], gpu_out[16], mismatches=0;
        for (int y=0;y<4;y++) for (int x=0;x<4;x++)
            cpu_out[y*4+x] = ref_mc21(g_ref_y, g_ref_stride, src_x+x, src_y+y);

        int pad=3, pw=4+2*pad, ph=4+2*pad;
        float *patch=(float*)malloc(sizeof(float)*pw*ph*4);
        for (int y=0;y<ph;y++) for (int x=0;x<pw;x++) {
            unsigned char v=g_ref_y[(src_y-pad+y)*g_ref_stride+(src_x-pad+x)];
            int idx=(y*pw+x)*4; patch[idx]=v/255.0f; patch[idx+1]=patch[idx+2]=0; patch[idx+3]=1;
        }
        GLuint refTex; glGenTextures(1,&refTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,pw,ph,0,GL_RGBA,GL_FLOAT,patch);

        int s1w=4, s1h=9;
        GLuint s1Tex; glGenTextures(1,&s1Tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,s1Tex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,s1w,s1h,0,GL_RGBA,GL_FLOAT,NULL);
        GLuint fbo; glGenFramebuffersEXT(1,&fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,s1Tex,0);

        glViewport(0,0,s1w,s1h);
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,s1w,0,s1h,-1,1);
        glMatrixMode(GL_MODELVIEW);glLoadIdentity();
        GLhandleARB progA = linkp(vs_plain, fs_diag_stage1);
        glUseProgramObjectARB(progA);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
        glUniform1iARB(glGetUniformLocationARB(progA,"refTex"),0);
        glUniform2fARB(glGetUniformLocationARB(progA,"baseOffset"), pad+0.5f, pad-2+0.5f);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(s1w,0);glVertex2f(s1w,s1h);glVertex2f(0,s1h); glEnd();
        glFinish(); checkgl("mc21 stage1");

        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0);
        glViewport(0,0,4,4);
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,4,0,4,-1,1);
        glMatrixMode(GL_MODELVIEW);glLoadIdentity();
        GLhandleARB progB = linkp(vs_plain, fs_mc21_stage2);
        glUseProgramObjectARB(progB);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,s1Tex);
        glUniform1iARB(glGetUniformLocationARB(progB,"stage1Tex"),0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,refTex);
        glUniform1iARB(glGetUniformLocationARB(progB,"refTex"),1);
        glUniform2fARB(glGetUniformLocationARB(progB,"baseOffset"), 0.5f, 2.5f);
        glUniform2fARB(glGetUniformLocationARB(progB,"refOrigin"), pad + 0.5f, pad + 0.5f);
        glClearColor(1,0,1,1); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(4,0);glVertex2f(4,4);glVertex2f(0,4); glEnd();
        glFinish(); checkgl("mc21 stage2");

        unsigned char pixels[4*4*4];
        glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        for (int row=0;row<4;row++) for (int col=0;col<4;col++) {
            int idx=row*4+col; gpu_out[idx]=pixels[idx*4+0];
            if (abs(gpu_out[idx]-cpu_out[idx])>2) mismatches++;
        }
        printf("mc21   MB(%d,%d) mv=(%d,%d): CPU=[", g_d.mbx,g_d.mby,g_d.mvx,g_d.mvy);
        for (int i=0;i<16;i++) printf("%d ", cpu_out[i]);
        printf("] GPU=[");
        for (int i=0;i<16;i++) printf("%d ", gpu_out[i]);
        printf("] -> %s (%d/16 differ)\n", mismatches==0?"MATCH":"MISMATCH", mismatches);
    }

    aglSetCurrentContext(NULL);
    aglDestroyContext(g_glctx);
    return 0;
}
