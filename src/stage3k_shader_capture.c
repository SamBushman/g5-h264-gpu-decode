/* PROMO4 Stage 3k: READ-ONLY capture of the stock compiler's native code for a texture-sampling fragment shader.
 * Binds a GLSL program that samples a 4x4 GL_TEXTURE_RECTANGLE_ARB texture (shader selectable), draws one quad, and saves the live
 * command buffer (base..cursor) BEFORE the flush, so the US_VECTOR_DATA writes (ctx program upload) can be decoded on the host
 * (tools/promo4_sim/decode_us.py). Writes nothing to the command buffer. Also reports the pixel read back.
 * Usage: stage3k-shader-capture VARIANT CAPFILE      VARIANT: 1 = passthrough texture2DRect, 2 = v*2+0.25 (needs ALU), 3 = 4-tap sum
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static GLhandleARB compile(GLenum t, const char *s) {
    GLhandleARB h = glCreateShaderObjectARB(t); glShaderSourceARB(h, 1, &s, NULL); glCompileShaderARB(h);
    GLint ok = 0; glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) { char log[2048]; GLsizei n; glGetInfoLogARB(h, sizeof log, &n, log); fprintf(stderr, "compile fail:\n%s\n", log); exit(1); }
    return h;
}
int main(int argc, char **argv) {
    int variant = argc > 1 ? atoi(argv[1]) : 1; const char *cap = argc > 2 ? argv[2] : "cap_3k.bin";
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glViewport(0, 0, 4, 4); glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    float tex[4 * 4 * 4]; int i;
    for (i = 0; i < 16; i++) { tex[i * 4 + 0] = (i % 4) / 4.0f; tex[i * 4 + 1] = (i / 4) / 4.0f; tex[i * 4 + 2] = 0.5f; tex[i * 4 + 3] = 1.0f; }
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, t);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_FLOAT, tex);
    const char *fs[] = {NULL,
      "uniform sampler2DRect t; void main(){gl_FragColor=texture2DRect(t,gl_TexCoord[0].xy);}",
      "uniform sampler2DRect t; void main(){gl_FragColor=texture2DRect(t,gl_TexCoord[0].xy)*2.0+vec4(0.25);}",
      "uniform sampler2DRect t; void main(){vec2 p=gl_TexCoord[0].xy; gl_FragColor=(texture2DRect(t,p)+texture2DRect(t,p+vec2(1.0,0.0))+texture2DRect(t,p+vec2(0.0,1.0))+texture2DRect(t,p+vec2(1.0,1.0)))*0.25;}"};
    GLhandleARB p = glCreateProgramObjectARB();
    glAttachObjectARB(p, compile(GL_VERTEX_SHADER_ARB, "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex; gl_TexCoord[0]=gl_MultiTexCoord0;}"));
    glAttachObjectARB(p, compile(GL_FRAGMENT_SHADER_ARB, fs[variant])); glLinkProgramARB(p);
    GLint ok = 0; glGetObjectParameterivARB(p, GL_OBJECT_LINK_STATUS_ARB, &ok); if (!ok) { fprintf(stderr, "link fail\n"); return 1; }
    glUseProgramObjectARB(p); glUniform1iARB(glGetUniformLocationARB(p, "t"), 0);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glVertex2f(0, 0); glMultiTexCoord2f(GL_TEXTURE0, 4, 0); glVertex2f(4, 0);
    glMultiTexCoord2f(GL_TEXTURE0, 4, 4); glVertex2f(4, 4); glMultiTexCoord2f(GL_TEXTURE0, 0, 4); glVertex2f(0, 4);
    glEnd();
    unsigned char *c = (unsigned char *)ctx;
    uint32_t link = *(uint32_t *)(c + 0x17d8), cur = *(uint32_t *)(c + 0x17dc), lim = *(uint32_t *)(c + 0x17e0), base = *(uint32_t *)(c + 0x17e4);
    fprintf(stderr, "[3k] variant %d: link=+0x%x cursor=+0x%x\n", variant, link - base, cur - base);
    if (base && cur > base && cur - base < 0x40000) {
        FILE *f = fopen(cap, "wb");
        if (f) { uint32_t h[5] = {link - base, cur - base, lim - base, 0x50414330u, 0}; fwrite(h, 4, 5, f); fwrite((void *)base, 1, (cur - base) + 16, f); fflush(f); fcntl(fileno(f), F_FULLFSYNC, 0); fclose(f); fprintf(stderr, "[3k] capture %s\n", cap); }
    }
    glFinish();
    unsigned char px[4 * 4 * 4]; glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (i = 0; i < 16; i++) fprintf(stderr, "px[%d,%d]=%3d,%3d,%3d,%3d%s", i % 4, i / 4, px[i*4], px[i*4+1], px[i*4+2], px[i*4+3], (i % 4 == 3) ? "\n" : "  ");
    aglSetCurrentContext(NULL); aglDestroyContext(ctx);
    return 0;
}
