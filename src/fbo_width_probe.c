/* Standalone probe: does this driver's FBO-attached-texture rendering
 * have a real width limit distinct from quirk #15's Pbuffer-size
 * dependency? Renders column-index-encoded data into a GL_RGBA_FLOAT32_ATI
 * FBO texture of varying width W, then blits it to the default
 * framebuffer (a Pbuffer already sized generously, 4096x32) via a
 * passthrough shader and reads back via GL_UNSIGNED_BYTE, checking each
 * column's value against its own index. */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>

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
static const char *fs_colidx = "void main(){ float c = floor(gl_FragCoord.x); gl_FragColor = vec4(c/255.0, 0.0, 0.0, 1.0); }\n";
static const char *fs_pass = "uniform sampler2DRect t;\nvoid main(){ gl_FragColor = vec4(texture2DRect(t, floor(gl_FragCoord.xy)+vec2(0.5,0.5)).r, 0.0,0.0,1.0); }\n";

static int test_width(int W) {
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,W,21,0,GL_RGBA,GL_FLOAT,NULL);
    GLuint fbo; glGenFramebuffersEXT(1,&fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,tex,0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT) { fprintf(stderr, "W=%d FBO incomplete 0x%x\n", W, status); return -1; }

    glViewport(0,0,W,21);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,W,0,21,-1,1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    static GLhandleARB progA = 0; if (!progA) progA = linkp(vs_plain, fs_colidx);
    glUseProgramObjectARB(progA);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W,0);glVertex2f(W,21);glVertex2f(0,21); glEnd();
    glFinish(); checkgl("draw to FBO");

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0);
    glViewport(0,0,W,21);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,W,0,21,-1,1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    static GLhandleARB progB = 0; if (!progB) progB = linkp(vs_plain, fs_pass);
    glUseProgramObjectARB(progB);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glUniform1iARB(glGetUniformLocationARB(progB,"t"),0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W,0);glVertex2f(W,21);glVertex2f(0,21); glEnd();
    glFinish(); checkgl("passthrough draw");

    unsigned char *pix = (unsigned char*)malloc((size_t)W*21*4);
    glReadPixels(0,0,W,21,GL_RGBA,GL_UNSIGNED_BYTE,pix);
    int first_bad = -1;
    for (int c = 0; c < W; c++) {
        int v = pix[(10*W + c)*4]; /* middle row */
        int expect = c % 256;
        if (v != expect) { first_bad = c; break; }
    }
    free(pix);
    glDeleteFramebuffersEXT(1,&fbo);
    glDeleteTextures(1,&tex);
    return first_bad;
}

int main(int argc, char **argv) {
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4096, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    if (argc > 1) {
        int W = atoi(argv[1]);
        int bad = test_width(W);
        printf("fresh-process W=%d: first_bad=%d\n", W, bad);
        aglSetCurrentContext(NULL);
        return 0;
    }

    printf("--- same width (64) repeated 5x ---\n");
    for (int i = 0; i < 5; i++) {
        int bad = test_width(64);
        printf("  try%d W=64: first_bad=%d\n", i, bad);
    }
    printf("--- descending widths ---\n");
    int widths2[] = {256,128,64,32,16};
    for (unsigned i = 0; i < sizeof(widths2)/sizeof(widths2[0]); i++) {
        int W = widths2[i];
        int bad = test_width(W);
        printf("  W=%4d: first_bad=%d\n", W, bad);
    }
    printf("--- ascending widths (original) ---\n");
    int widths[] = {16,32,40,44,46,47,48,49,50,52,56,60,63,64,65,72,80,96,128,192,256};
    for (unsigned i = 0; i < sizeof(widths)/sizeof(widths[0]); i++) {
        int W = widths[i];
        int bad = test_width(W);
        printf("W=%4d: %s%s\n", W, bad < 0 ? "FBO INCOMPLETE" : (bad < W ? "BROKEN at col " : "all columns OK"),
               (bad >= 0 && bad < W) ? "" : "");
        if (bad >= 0) printf("        first bad col = %d\n", bad);
    }
    aglSetCurrentContext(NULL);
    return 0;
}
