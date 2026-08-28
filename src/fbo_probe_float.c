/*
 * fbo-probe-float: extends fbo_probe.c to the float-format case Module D
 * actually needs (GL_ATI_texture_float, GL_RGBA_FLOAT32_ATI) rather than
 * the fixed-point GL_RGBA8 already verified working.
 *
 * Fixed-point RGBA8's [0,1]-clamped default framebuffer can't prove
 * out-of-range values survive a float texture (the final display stage
 * would clamp them regardless of what the intermediate texture held), so
 * this probe writes values spanning [-1, 3] into the float FBO texture in
 * pass 1, then in pass 2 samples the texture UNCONDITIONALLY (the
 * texture2DRect() call itself is never inside a branch, per quirk #2) and
 * only branches on the ALREADY-SAMPLED value to classify it into a
 * flag color (red = >1, green = <0, blue = in [0,1]) - a clamp-safe way
 * to detect whether the true float magnitude survived the round trip.
 */

#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 8

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

    /* Pass 1: render r = (x/W)*4.0 - 1.0 (spans -1..3, well outside [0,1])
     * into an FBO-attached float-format rectangle texture. */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, W, H, 0,
                 GL_RGBA, GL_FLOAT, NULL);
    check_gl("glTexImage2D rectangle float32");

    GLuint fbo;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                               GL_TEXTURE_RECTANGLE_ARB, tex, 0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    printf("Float FBO status: 0x%x (%s)\n", status,
           status == GL_FRAMEBUFFER_COMPLETE_EXT ? "COMPLETE" : "INCOMPLETE");
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT) {
        printf("Cannot use a float texture as an FBO color attachment on this driver.\n");
        return 1;
    }

    const char *vs_pass1 =
        "void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; "
        "gl_TexCoord[0] = gl_MultiTexCoord0; }";
    const char *fs_pass1 =
        "void main() { float r = (gl_TexCoord[0].x / float(64.0)) * 4.0 - 1.0; "
        "gl_FragColor = vec4(r, 0.25, -0.5, 1.0); }";
    GLhandleARB prog1 = link_prog(vs_pass1, fs_pass1);
    glUseProgramObjectARB(prog1);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_quad(0, 0, W, H, 0, 0, W, H);
    glFinish();
    check_gl("pass1 draw (float)");

    /* Pass 2: unconditionally sample the float texture (never inside a
     * branch, per quirk #2), THEN branch on the already-sampled value to
     * classify it into a clamp-safe flag color, drawn into the Pbuffer's
     * ordinary fixed-point default framebuffer. */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glViewport(0, 0, W, H);
    glClearColor(1, 1, 1, 1); /* white clear - distinct from all 3 flag colors */
    glClear(GL_COLOR_BUFFER_BIT);

    const char *fs_pass2 =
        "#extension GL_ARB_texture_rectangle : enable\n"
        "uniform sampler2DRect tex;\n"
        "void main() {\n"
        "  vec4 c = texture2DRect(tex, gl_TexCoord[0].xy);\n"
        "  if (c.r > 1.0) { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n"
        "  else if (c.r < 0.0) { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n"
        "  else { gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0); }\n"
        "}";
    GLhandleARB prog2 = link_prog(vs_pass1, fs_pass2);
    glUseProgramObjectARB(prog2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    GLint loc = glGetUniformLocationARB(prog2, "tex");
    glUniform1iARB(loc, 0);
    draw_quad(0, 0, W, H, 0, 0, W, H);
    glFinish();
    check_gl("pass2 draw (float)");

    unsigned char row[W * 4];
    glReadPixels(0, H / 2, W, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);

    printf("\nx : true_r        : classification (R=>1 G=<0 B=in[0,1])\n");
    int mismatches = 0;
    for (int x = 0; x < W; x += 4) {
        float true_r = (x / (float)W) * 4.0f - 1.0f;
        int r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2];
        const char *got = (r > 200 && g < 50) ? "RED(>1)" : (g > 200 && r < 50) ? "GREEN(<0)" : (b > 200 && r < 50 && g < 50) ? "BLUE([0,1])" : "???";
        const char *expect = true_r > 1.0f ? "RED(>1)" : true_r < 0.0f ? "GREEN(<0)" : "BLUE([0,1])";
        int ok = strcmp(got, expect) == 0;
        if (!ok) mismatches++;
        printf("%2d : %8.3f      : got=%-11s expect=%-11s  %s\n",
               x, true_r, got, expect, ok ? "OK" : "MISMATCH");
    }

    printf("\n%s: %d mismatches out of %d samples\n",
           mismatches == 0
               ? "RESULT: float FBO texture correctly preserves out-of-[0,1]-range values"
               : "RESULT: float FBO texture does NOT reliably preserve out-of-range values",
           mismatches, W / 4);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return mismatches != 0;
}
