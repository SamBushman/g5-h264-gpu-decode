/*
 * gl-probe: Milestone 4. Creates an offscreen AGL context (no window, so
 * this is safe to run over SSH - avoids the CoreDrag/NSWindow deadlock
 * documented in the tiger-ssh skill, since that's specific to real windows)
 * and reports what the X1900's actual Tiger OpenGL driver exposes.
 */

#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report_agl_error(const char *where) {
    GLenum e = aglGetError();
    fprintf(stderr, "%s failed: AGL error %d (%s)\n", where, e, aglErrorString(e));
}

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    if (!pf) {
        report_agl_error("aglChoosePixelFormat");
        return 1;
    }
    AGLContext ctx = aglCreateContext(pf, NULL);
    aglDestroyPixelFormat(pf);
    if (!ctx) {
        report_agl_error("aglCreateContext");
        return 1;
    }

    int width = 64, height = 64;
    AGLPbuffer pbuf;
    if (!aglCreatePBuffer(width, height, GL_TEXTURE_2D, GL_RGBA, 0, &pbuf)) {
        report_agl_error("aglCreatePBuffer");
        return 1;
    }
    if (!aglSetPBuffer(ctx, pbuf, 0, 0, 0)) {
        report_agl_error("aglSetPBuffer");
        return 1;
    }
    if (!aglSetCurrentContext(ctx)) {
        report_agl_error("aglSetCurrentContext");
        return 1;
    }

    printf("GL_VENDOR:   %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VERSION:  %s\n", (const char *)glGetString(GL_VERSION));

    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    printf("GL_EXTENSIONS length: %lu\n\n", ext ? (unsigned long)strlen(ext) : 0UL);

    const char *want[] = {
        "GL_ARB_fragment_program", "GL_ARB_vertex_program",
        "GL_ARB_shader_objects", "GL_ARB_fragment_shader", "GL_ARB_vertex_shader",
        "GL_ARB_shading_language_100",
        "GL_EXT_framebuffer_object",
        "GL_ARB_texture_float", "GL_APPLE_float_pixels", "GL_ATI_texture_float",
        "GL_ARB_texture_rectangle", "GL_EXT_texture_rectangle", "GL_NV_texture_rectangle",
        "GL_ARB_texture_non_power_of_two",
        "GL_ARB_color_buffer_float", "GL_APPLE_client_storage",
        "GL_ARB_draw_buffers", "GL_ARB_pixel_buffer_object",
        "GL_APPLE_fence", "GL_APPLE_vertex_array_object",
        NULL};
    for (int i = 0; want[i]; i++) {
        int has = ext && strstr(ext, want[i]) != NULL;
        printf("  %-34s %s\n", want[i], has ? "YES" : "no");
    }

    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    printf("\nGL_MAX_TEXTURE_SIZE: %d\n", max_tex);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
