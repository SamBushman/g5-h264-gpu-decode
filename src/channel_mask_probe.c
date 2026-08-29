/*
 * channel-mask-probe: minimal correctness test for the core mechanism
 * behind "pack multiple reference frames into different channels of one
 * shared texture, updated on the GPU without a full re-upload" - does
 * glColorMask() actually let a render pass update ONE channel of an
 * FBO-attached texture while leaving the other channels' existing content
 * untouched, on this driver?
 *
 * This is a pure mechanism/correctness check, not a timing test - if this
 * doesn't work (or corrupts data), the whole channel-packing idea is dead
 * regardless of any potential speed win, so it's the right thing to
 * verify first, before building any caching/bookkeeping machinery.
 *
 * Sequence: (1) render a known, distinguishable RGBA pattern into an
 * FBO-attached texture with no masking (baseline - all 4 channels get
 * real, different values). (2) with glColorMask(R only), render a NEW,
 * different R value - G/B/A should be structurally protected. (3) with
 * glColorMask(G only), render a NEW, different G value - tests that a
 * SECOND masked update composes correctly on top of the first, and that
 * R (already updated), B, and A (never touched) all survive. (4) sample
 * the result via a second draw to the DEFAULT framebuffer (this
 * project's established safe FBO-readback pattern - quirk #6 found
 * direct glReadPixels against an FBO unreliable) and verify all 4
 * channels hold exactly the expected final values.
 */

#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Baseline: writes a known, distinct value to every channel. */
static const char *fs_baseline =
"void main(){ gl_FragColor = vec4(50.0/255.0, 100.0/255.0, 150.0/255.0, 200.0/255.0); }\n";
/* R-update candidate value (222) - G/B/A values here are irrelevant when
 * R-only color masking is active, but set to something distinguishable
 * anyway so a masking FAILURE (writing all channels) is obvious. */
static const char *fs_write_r =
"void main(){ gl_FragColor = vec4(222.0/255.0, 1.0, 1.0, 1.0); }\n";
/* G-update candidate value (111). */
static const char *fs_write_g =
"void main(){ gl_FragColor = vec4(1.0, 111.0/255.0, 1.0, 1.0); }\n";
/* Passthrough: samples the FBO texture and writes it straight to the
 * default framebuffer - this project's established safe FBO-readback
 * pattern (quirk #6: direct glReadPixels against an FBO is unreliable). */
static const char *fs_passthrough =
"uniform sampler2DRect srcTex;\n"
"void main(){ gl_FragColor = texture2DRect(srcTex, floor(gl_FragCoord.xy)+vec2(0.5,0.5)); }\n";

int main(void) {
    const int W = 64, H = 4; /* small, well within quirk #16's known-safe FBO width */

    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(256, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    GLuint fboTex; glGenTextures(1,&fboTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,fboTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    GLuint fbo; glGenFramebuffersEXT(1,&fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,fboTex,0);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT) { fprintf(stderr, "FBO incomplete: 0x%x\n", status); return 1; }
    checkgl("fbo setup");

    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    GLhandleARB progBaseline = linkp(vs_plain, fs_baseline);
    GLhandleARB progWriteR = linkp(vs_plain, fs_write_r);
    GLhandleARB progWriteG = linkp(vs_plain, fs_write_g);
    GLhandleARB progPass = linkp(vs_plain, fs_passthrough);

    /* Pass 1: baseline, all channels, no masking. */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgramObjectARB(progBaseline);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W,0);glVertex2f(W,H);glVertex2f(0,H); glEnd();
    checkgl("baseline pass");

    /* Pass 2: R-only masked update. */
    glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
    glUseProgramObjectARB(progWriteR);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W,0);glVertex2f(W,H);glVertex2f(0,H); glEnd();
    checkgl("R-masked pass");

    /* Pass 3: G-only masked update (on top of the R-updated state). */
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
    glUseProgramObjectARB(progWriteG);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W,0);glVertex2f(W,H);glVertex2f(0,H); glEnd();
    checkgl("G-masked pass");

    /* Restore full color mask before any further normal drawing (good
     * hygiene - color mask state persists on the context otherwise). */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    /* Pass 4: sample the result into the DEFAULT framebuffer (safe
     * readback pattern), then glReadPixels from there. */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glUseProgramObjectARB(progPass);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, fboTex);
    glUniform1iARB(glGetUniformLocationARB(progPass,"srcTex"),0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(W,0);glVertex2f(W,H);glVertex2f(0,H); glEnd();
    glFinish(); checkgl("passthrough draw");

    unsigned char *pixels = (unsigned char*)malloc((size_t)W*H*4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    checkgl("readback");

    printf("=== channel-mask-probe: %dx%d, expect R=222 G=111 B=150(untouched) A=200(untouched) ===\n", W, H);
    int expected[4] = {222, 111, 150, 200};
    const char *names[4] = {"R", "G", "B", "A"};
    int mismatches[4] = {0,0,0,0};
    int total = W*H;
    for (int i = 0; i < total; i++) {
        for (int c = 0; c < 4; c++) {
            if (pixels[i*4+c] != expected[c]) mismatches[c]++;
        }
    }
    for (int c = 0; c < 4; c++)
        printf("  %s: expected=%d, %d/%d pixels wrong\n", names[c], expected[c], mismatches[c], total);
    printf("  sample pixel (0,0): R=%d G=%d B=%d A=%d\n", pixels[0], pixels[1], pixels[2], pixels[3]);

    int all_ok = (mismatches[0]==0 && mismatches[1]==0 && mismatches[2]==0 && mismatches[3]==0);
    if (all_ok) {
        printf("\n-> CONFIRMED: glColorMask correctly isolates channel writes on this driver - R and G were\n"
               "   independently updated via masked passes, B and A survived BOTH passes untouched.\n"
               "   The core mechanism for GPU-side channel-packed reference caching works. This does NOT\n"
               "   yet prove it's a net performance win - only that it's mechanically possible/correct.\n");
    } else {
        printf("\n-> FAILED: color masking did not behave as expected on this driver - at least one channel\n"
               "   was corrupted or not updated correctly. The channel-packing idea is not viable as designed;\n"
               "   a new, real driver quirk if confirmed. Do not build further on this mechanism.\n");
    }

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return all_ok ? 0 : 1;
}
