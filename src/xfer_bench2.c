/* Shared-memory transfer variants (issue #1). Frame = 480x1632 RGBA8 (3.1 MB). CPU rewrites the buffer each iteration (as the deblocker would).
 * Upload: (A) glTexSubImage2D baseline; (B) client_storage+texture_range+shared hint, page-aligned buffer, TexSubImage to flag dirty; (C) same, no TexSubImage.
 * Cost = CPU write + GL work + a sampling draw + glFinish, so lazy DMA is charged. Readback: (D) glReadPixels to RAM; (E) PBO pack + map, sync; (F) PBO issue then CPU work then map. */
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
    int iters = argc > 1 ? atoi(argv[1]) : 20, it, w = 480, h = 1632; size_t n = (size_t)w * h * 4, k;
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs); AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pb; aglCreatePBuffer(2048, 2048, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pb); aglSetPBuffer(ctx, pb, 0, 0, 0); aglSetCurrentContext(ctx);
    GLenum T = GL_TEXTURE_RECTANGLE_ARB;
    GLhandleARB prog = glCreateProgramObjectARB();
    glAttachObjectARB(prog, compile(GL_VERTEX_SHADER_ARB, "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}"));
    glAttachObjectARB(prog, compile(GL_FRAGMENT_SHADER_ARB, "uniform sampler2DRect t; void main(){gl_FragColor=texture2DRect(t,floor(gl_FragCoord.xy));}")); glLinkProgramARB(prog);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_DITHER);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, w, 0, h, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glViewport(0, 0, w, h);
    uint8_t *buf = valloc(n), *out = valloc(n);
    #define FILL(seed) do { uint32_t *p_ = (uint32_t *)buf; size_t j_; for (j_ = 0; j_ < n / 4; j_++) p_[j_] = (uint32_t)(j_ * 2654435761u) ^ (seed); } while (0)
    #define DRAWQ() do { glUseProgramObjectARB(prog); glUniform1iARB(glGetUniformLocationARB(prog, "t"), 0); \
        glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(w, 0); glVertex2f(w, h); glVertex2f(0, h); glEnd(); } while (0)
    GLuint tex; glGenTextures(1, &tex); glBindTexture(T, tex);
    glTexParameteri(T, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(T, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    double tf = 0, t; int v;
    { double a = now_ms(); for (it = 0; it < iters; it++) FILL(it); tf = (now_ms() - a) / iters; }
    printf("CPU fill of 3.1 MB (cost common to all variants): %.2f ms\n", tf);
    const char *names[3] = {"A TexSubImage (baseline)", "B client_storage+range+shared, TexSubImage flag", "C client_storage+range+shared, no TexSubImage"};
    for (v = 0; v < 3; v++) {
        glDeleteTextures(1, &tex); glGenTextures(1, &tex); glBindTexture(T, tex);
        glTexParameteri(T, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(T, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (v > 0) { glTextureRangeAPPLE(T, n, buf); glTexParameteri(T, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE); glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE); }
        glTexImage2D(T, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        if (v > 0) glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
        double tg = 0; int bad = 0;
        for (it = 0; it < iters + 2; it++) {
            double a = now_ms(); FILL(it + 100);
            if (v < 2) glTexSubImage2D(T, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            DRAWQ(); glFinish(); double b = now_ms(); if (it >= 2) tg += b - a - 0;
            if (it == iters + 1) { glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out); bad = memcmp(buf, out, n) != 0; }
        }
        printf("%-52s fill+upload+draw+finish %.2f ms (upload-attributable ~%.2f ms) data %s\n", names[v], tg / iters, tg / iters - tf, bad ? "STALE/MISMATCH" : "correct");
    }
    /* readback variants from the pbuffer (draw the texture first) */
    double tD = 0, tE = 0, tF = 0, tFw = 0; GLuint pbo; glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, pbo); glBufferData(GL_PIXEL_PACK_BUFFER_ARB, n, NULL, GL_STREAM_READ); glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, 0);
    for (it = 0; it < iters + 2; it++) {
        DRAWQ(); glFinish(); double a = now_ms(); glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, out); double b = now_ms(); if (it >= 2) tD += b - a;
        DRAWQ(); glFinish(); a = now_ms(); glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, pbo); glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, (void *)0);
        void *m = glMapBuffer(GL_PIXEL_PACK_BUFFER_ARB, GL_READ_ONLY); volatile uint8_t s = ((uint8_t *)m)[n - 1]; (void)s; glUnmapBuffer(GL_PIXEL_PACK_BUFFER_ARB); glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, 0); b = now_ms(); if (it >= 2) tE += b - a;
        DRAWQ(); glFinish(); a = now_ms(); glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, pbo); glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, (void *)0); glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, 0);
        b = now_ms(); FILL(it); double c = now_ms();   /* 'other CPU work' ~ tf ms */
        glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, pbo); m = glMapBuffer(GL_PIXEL_PACK_BUFFER_ARB, GL_READ_ONLY); s = ((uint8_t *)m)[n - 1]; glUnmapBuffer(GL_PIXEL_PACK_BUFFER_ARB); glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, 0); double d = now_ms();
        if (it >= 2) { tF += b - a; tFw += d - c; }
    }
    printf("D glReadPixels to RAM (BGRA):                        %.2f ms\n", tD / iters);
    printf("E PBO pack + map, synchronous:                       %.2f ms\n", tE / iters);
    printf("F PBO async: issue %.2f ms, then %.1f ms of CPU work, then map waits %.2f ms\n", tF / iters, tf, tFw / iters);
    return 0;
}
