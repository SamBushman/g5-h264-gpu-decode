/* Transfer-cost bench (issue #1): what does a full-frame GPU->CPU readback and CPU->GPU upload cost on the stock driver?
 * Decides whether "GPU MC+residual, CPU deblock, re-upload" (exact deblock is a 256-wave wavefront on GPU) is viable.
 * Frame 1080p 4:2:0 = luma 1920x1088 + chroma 2x960x544 = 3.13 MB; packed 4 px/texel as RGBA8: luma 480x1088, chroma 240x544 x2 (stacked 240x1088).
 * Readback goes via a sampling pass to the pbuffer (quirk 6), never glReadPixels on the FBO. */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
static double now_ms(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec * 1000.0 + t.tv_usec / 1000.0; }
static GLhandleARB compile(GLenum t, const char *s) { GLhandleARB h = glCreateShaderObjectARB(t); glShaderSourceARB(h, 1, &s, NULL); glCompileShaderARB(h);
    GLint ok = 0; glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok); if (!ok) { fprintf(stderr, "compile fail\n"); exit(1); } return h; }
int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 20, i, it;
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs); AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pb; aglCreatePBuffer(2048, 2048, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pb); aglSetPBuffer(ctx, pb, 0, 0, 0); aglSetCurrentContext(ctx);
    GLenum T = GL_TEXTURE_RECTANGLE_ARB;
    struct { const char *name; int w, h; } sz[] = {{"luma packed 480x1088 (2.0MB)", 480, 1088}, {"frame packed 480x1632 (3.1MB: luma + stacked chroma)", 480, 1632}, {"unpacked RGBA 1920x1088 (8.3MB)", 1920, 1088}};
    GLuint prog = glCreateProgramObjectARB();
    glAttachObjectARB(prog, compile(GL_VERTEX_SHADER_ARB, "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}"));
    glAttachObjectARB(prog, compile(GL_FRAGMENT_SHADER_ARB, "uniform sampler2DRect t; void main(){gl_FragColor=texture2DRect(t,floor(gl_FragCoord.xy));}")); glLinkProgramARB(prog);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_DITHER);
    for (i = 0; i < 3; i++) {
        int w = sz[i].w, h = sz[i].h; size_t n = (size_t)w * h * 4; uint8_t *buf = malloc(n), *out = malloc(n); size_t k;
        for (k = 0; k < n; k++) buf[k] = (uint8_t)(k * 7 + (k >> 9));
        GLuint tex; glGenTextures(1, &tex); glBindTexture(T, tex);
        glTexParameteri(T, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(T, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(T, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, w, 0, h, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glViewport(0, 0, w, h);
        struct { const char *fmtname; GLenum fmt, type; } fm[] = {{"RGBA/UBYTE", GL_RGBA, GL_UNSIGNED_BYTE}, {"BGRA/8888_REV", GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV}};
        int f;
        for (f = 0; f < 2; f++) {
            double tu = 0, tu2 = 0, tr = 0, tp = 0;
            for (it = 0; it < iters + 2; it++) {
                double a = now_ms(); glTexSubImage2D(T, 0, 0, 0, w, h, fm[f].fmt, fm[f].type, buf); glFinish(); double b = now_ms();
                glUseProgramObjectARB(prog); glBindTexture(T, tex); glUniform1iARB(glGetUniformLocationARB(prog, "t"), 0);
                glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(w, 0); glVertex2f(w, h); glVertex2f(0, h); glEnd(); glFinish(); double c = now_ms();
                glReadPixels(0, 0, w, h, fm[f].fmt, fm[f].type, out); double d = now_ms();
                if (it >= 2) { tu += b - a; tp += c - b; tr += d - c; }
            }
            int bad = memcmp(buf, out, n) != 0;
            printf("%-52s %-14s upload %.2f ms | pass %.2f ms | readPixels %.2f ms | roundtrip data %s\n", sz[i].name, fm[f].fmtname, tu / iters, tp / iters, tr / iters, bad ? "MISMATCH" : "exact");
        }
        /* upload variants: client_storage + texture_range / storage hint */
        { double t2 = 0; glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE); glTexParameteri(T, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE);
          glTexImage2D(T, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, buf);
          for (it = 0; it < iters + 2; it++) { double a = now_ms(); glTexSubImage2D(T, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, buf); glFinish(); if (it >= 2) t2 += now_ms() - a; }
          glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
          printf("%-52s client_storage+shared hint, BGRA: upload %.2f ms\n", sz[i].name, t2 / iters); }
        free(buf); free(out);
    }
    return 0;
}
