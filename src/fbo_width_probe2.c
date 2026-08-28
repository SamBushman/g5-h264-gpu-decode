/* Follow-up probe: does keeping the FBO-attached texture at a FIXED
 * allocated size (never re-calling glTexImage2D with a new width) and
 * only varying the glViewport/render-quad WIDTH avoid the corruption seen
 * when repeatedly resizing the texture itself to growing widths? */
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

#define TEXW 512
static GLuint g_tex = 0, g_fbo = 0;

static int test_width(int W) {
    if (!g_tex) {
        glGenTextures(1,&g_tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,g_tex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,TEXW,21,0,GL_RGBA,GL_FLOAT,NULL);
        glGenFramebuffersEXT(1,&g_fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,g_fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,g_tex,0);
        GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
        if (status != GL_FRAMEBUFFER_COMPLETE_EXT) { fprintf(stderr, "FBO incomplete 0x%x\n", status); return -1; }
    } else {
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,g_fbo);
    }

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
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,g_tex);
    glUniform1iARB(glGetUniformLocationARB(progB,"t"),0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W,0);glVertex2f(W,21);glVertex2f(0,21); glEnd();
    glFinish(); checkgl("passthrough draw");

    unsigned char *pix = (unsigned char*)malloc((size_t)W*21*4);
    glReadPixels(0,0,W,21,GL_RGBA,GL_UNSIGNED_BYTE,pix);
    int first_bad = -1;
    for (int c = 0; c < W; c++) {
        int v = pix[(10*W + c)*4];
        int expect = c % 256;
        if (v != expect) { first_bad = c; break; }
    }
    free(pix);
    return first_bad;
}

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4096, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    printf("--- fixed %dpx texture, ascending viewport widths ---\n", TEXW);
    int widths[] = {16,32,40,44,46,47,48,49,50,52,56,60,63,64,65,72,80,96,128,192,256,300,400};
    for (unsigned i = 0; i < sizeof(widths)/sizeof(widths[0]); i++) {
        int W = widths[i];
        int bad = test_width(W);
        printf("W=%4d: first_bad=%d\n", W, bad);
    }
    aglSetCurrentContext(NULL);
    return 0;
}
