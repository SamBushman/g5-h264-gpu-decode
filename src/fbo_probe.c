/*
 * fbo-probe: targeted test of ATI_RADEON_X1900_TIGER_DRIVER_QUIRKS.md's
 * quirk #4 ("FBO render-to-texture doesn't reliably reach that texture's
 * real sampled content") and quirk #6 (glReadPixels against an FBO is
 * separately unreliable), using OUR intended texture setup specifically
 * (GL_TEXTURE_RECTANGLE_ARB, NPOT dims - not Godot's GL_TEXTURE_2D/POT
 * setup where the bug was originally found), before designing the real
 * multi-pass GPU reconstruction pipeline around FBO chaining.
 *
 * Pass 1: render a distinctive per-pixel gradient into an FBO-attached
 *         GL_TEXTURE_RECTANGLE_ARB texture via a fragment shader.
 * Pass 2: sample that texture (texture2DRect) in a second full-screen
 *         draw into the Pbuffer's default framebuffer.
 * Verify: glReadPixels the default framebuffer (NOT the FBO - sidesteps
 *         quirk #6) after pass 2, check against the known-expected
 *         gradient formula at several sample points.
 */

#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 64
#define H 32

static void check_gl(const char *where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) fprintf(stderr, "GL error at %s: 0x%x\n", where, e);
}

static GLhandleARB compile(GLenum type, const char *src) {
    GLhandleARB s = glCreateShaderObjectARB(type);
    glShaderSourceARB(s, 1, &src, NULL);
    glCompileShaderARB(s);
    GLint ok = 0;
    glGetObjectParameterivARB(s, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n;
        glGetInfoLogARB(s, sizeof(log), &n, log);
        fprintf(stderr, "compile failed:\n%s\n", log);
        exit(1);
    }
    return s;
}

static GLhandleARB link_prog(const char *vs_src, const char *fs_src) {
    GLhandleARB prog = glCreateProgramObjectARB();
    glAttachObjectARB(prog, compile(GL_VERTEX_SHADER_ARB, vs_src));
    glAttachObjectARB(prog, compile(GL_FRAGMENT_SHADER_ARB, fs_src));
    glLinkProgramARB(prog);
    GLint ok = 0;
    glGetObjectParameterivARB(prog, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n;
        glGetInfoLogARB(prog, sizeof(log), &n, log);
        fprintf(stderr, "link failed:\n%s\n", log);
        exit(1);
    }
    return prog;
}

static void draw_quad(float x0, float y0, float x1, float y1,
                       float u0, float v0, float u1, float v1) {
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
    glEnd();
}

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL);
    aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf;
    aglCreatePBuffer(W, H, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0);
    aglSetCurrentContext(ctx);

    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Pass 1: render a gradient (r=x/W, g=y/H, b=0.5) into an FBO-attached
     * GL_TEXTURE_RECTANGLE_ARB texture. */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    check_gl("glTexImage2D rectangle NPOT");

    GLuint fbo;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                               GL_TEXTURE_RECTANGLE_ARB, tex, 0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    printf("FBO status: 0x%x (%s)\n", status,
           status == GL_FRAMEBUFFER_COMPLETE_EXT ? "COMPLETE" : "INCOMPLETE");

    const char *vs_pass1 =
        "void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; "
        "gl_TexCoord[0] = gl_MultiTexCoord0; }";
    const char *fs_pass1 =
        "void main() { gl_FragColor = vec4(gl_TexCoord[0].x / float(64.0), "
        "gl_TexCoord[0].y / float(32.0), 0.5, 1.0); }";
    GLhandleARB prog1 = link_prog(vs_pass1, fs_pass1);
    glUseProgramObjectARB(prog1);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_quad(0, 0, W, H, 0, 0, W, H);
    glFinish();
    check_gl("pass1 draw");

    /* Sanity: also try glReadPixels directly against the FBO (quirk #6). */
    unsigned char fbo_readback[4];
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fbo_readback);
    printf("glReadPixels DIRECTLY against FBO at (%d,%d): r=%d g=%d b=%d (expected r~%d g~%d)\n",
           W / 2, H / 2, fbo_readback[0], fbo_readback[1], fbo_readback[2],
           (int)(255.0 * (W / 2) / (float)W), (int)(255.0 * (H / 2) / (float)H));

    /* Pass 2: unbind FBO, sample the rectangle texture in a fresh draw into
     * the Pbuffer's default framebuffer, then verify via glReadPixels
     * against the DEFAULT framebuffer (not the FBO). */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glViewport(0, 0, W, H);
    glClearColor(1, 0, 1, 1); /* magenta clear - should be fully overdrawn */
    glClear(GL_COLOR_BUFFER_BIT);

    const char *vs_pass2 = vs_pass1;
    const char *fs_pass2 =
        "#extension GL_ARB_texture_rectangle : enable\n"
        "uniform sampler2DRect tex;\n"
        "void main() { gl_FragColor = texture2DRect(tex, gl_TexCoord[0].xy); }";
    GLhandleARB prog2 = link_prog(vs_pass2, fs_pass2);
    glUseProgramObjectARB(prog2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    GLint loc = glGetUniformLocationARB(prog2, "tex");
    glUniform1iARB(loc, 0);
    draw_quad(0, 0, W, H, 0, 0, W, H);
    glFinish();
    check_gl("pass2 draw");

    unsigned char row[W * 4];
    printf("\nVerifying pass-2 output (sampled through the FBO-rendered texture) via\n"
           "glReadPixels against the DEFAULT framebuffer:\n");
    int mismatches = 0;
    for (int y = 0; y < H; y += 8) {
        glReadPixels(0, y, W, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        for (int x = 0; x < W; x += 16) {
            int r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2];
            int exp_r = (int)(255.0 * x / (float)W);
            int exp_g = (int)(255.0 * y / (float)H);
            int ok = (abs(r - exp_r) <= 2 && abs(g - exp_g) <= 2 && b > 100 && b < 156);
            if (!ok) mismatches++;
            printf("  (%2d,%2d): got r=%3d g=%3d b=%3d, expected r~%3d g~%3d b~128  %s\n",
                   x, y, r, g, b, exp_r, exp_g, ok ? "OK" : "MISMATCH");
        }
    }

    printf("\n%s: %d mismatches\n",
           mismatches == 0 ? "RESULT: FBO render-then-sample WORKS for our rectangle/RGBA8 setup"
                            : "RESULT: FBO render-then-sample IS BROKEN for our setup (matches quirk #4)",
           mismatches);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return mismatches != 0;
}
