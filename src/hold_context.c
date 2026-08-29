/* PROMO4 Stage 1 support program: hold a real, live AGL/GL context open
 * (the normal Apple-driver connection, via _gldCreateContext) for a fixed
 * duration, doing one real draw + glFinish to confirm it's genuinely
 * active, not just created-and-idle. A second, independent process
 * (stage1_probe) opens its own IOKit connection to the SAME kext while
 * this one is alive, to test real multi-client coexistence - the actual
 * question Stage 1 exists to answer.
 *
 * Usage: hold-context <seconds>
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void checkgl(const char *w) { GLenum e = glGetError(); if (e) fprintf(stderr, "GL err %s: 0x%x\n", w, (unsigned)e); }

int main(int argc, char **argv) {
    int secs = (argc > 1) ? atoi(argv[1]) : 15;
    fprintf(stderr, "[hold-context] pid=%d starting, will hold context for %d s\n", (int)getpid(), secs);

    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8,
                        AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    if (!pf) { fprintf(stderr, "[hold-context] aglChoosePixelFormat failed\n"); return 1; }
    AGLContext ctx = aglCreateContext(pf, NULL);
    aglDestroyPixelFormat(pf);
    if (!ctx) { fprintf(stderr, "[hold-context] aglCreateContext failed\n"); return 1; }

    AGLPbuffer pbuf;
    aglCreatePBuffer(256, 256, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0);
    aglSetCurrentContext(ctx);

    glViewport(0, 0, 256, 256);
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(0, 1);
    glEnd();
    glFinish();
    checkgl("initial draw+finish");
    fprintf(stderr, "[hold-context] context live and confirmed active (draw+finish OK)\n");

    for (int i = 0; i < secs; i++) {
        sleep(1);
        /* light heartbeat draw so the context stays genuinely "in use",
         * not just open-and-dormant */
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        fprintf(stderr, "[hold-context] heartbeat %d/%d\n", i + 1, secs);
    }

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    fprintf(stderr, "[hold-context] clean exit\n");
    return 0;
}
