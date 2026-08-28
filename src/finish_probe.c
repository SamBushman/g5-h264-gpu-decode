/* Item 9 investigation (2026-08-28): does glFinish() on this G5/X1900/Tiger
 * AGL driver busy-wait (consume real CPU cycles while the GPU works) or
 * genuinely block/yield (CPU free during the wait)? Directly determines
 * whether "free up the CPU during playback" is even achievable by
 * restructuring dispatch, or whether the driver's own synchronous model is
 * a hard floor regardless of application-side changes.
 *
 * Method: issue one deliberately expensive GPU dispatch (a large fragment
 * shader over a big viewport, with real per-fragment work so the GPU
 * actually takes measurable time), call glFinish(), and compare wall-clock
 * time (gettimeofday) against CPU time (getrusage) for JUST that call. If
 * CPU time ~= wall time, glFinish is busy-waiting. If CPU time << wall
 * time, the CPU is genuinely free during the wait.
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>

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

/* Deliberately expensive: a real loop of trig/sqrt ops per fragment so the
 * GPU has genuine work to do, not something the driver can trivially skip. */
static const char *fs_heavy =
"void main() {\n"
"  float x = gl_FragCoord.x * 0.001 + gl_FragCoord.y * 0.002;\n"
"  float acc = 0.0;\n"
"  for (int i = 0; i < 400; i++) {\n"
"    x = sin(x) * cos(x * 1.3) + sqrt(abs(x) + 0.001);\n"
"    acc += x;\n"
"  }\n"
"  gl_FragColor = vec4(acc, acc, acc, 1.0);\n"
"}\n";

static double wall_ms(struct timeval *a, struct timeval *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_usec - a->tv_usec) / 1000.0;
}
static double cpu_ms(struct rusage *a, struct rusage *b) {
    double au = a->ru_utime.tv_sec * 1000.0 + a->ru_utime.tv_usec / 1000.0;
    double as_ = a->ru_stime.tv_sec * 1000.0 + a->ru_stime.tv_usec / 1000.0;
    double bu = b->ru_utime.tv_sec * 1000.0 + b->ru_utime.tv_usec / 1000.0;
    double bs = b->ru_stime.tv_sec * 1000.0 + b->ru_stime.tv_usec / 1000.0;
    return (bu - au) + (bs - as_);
}

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8,
                        AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(1024, 1024, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    GLhandleARB prog = linkp(vs_plain, fs_heavy);
    glUseProgramObjectARB(prog);
    glViewport(0, 0, 1024, 1024);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 1024, 0, 1024, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    for (int trial = 0; trial < 3; trial++) {
        struct timeval w0, w1; struct rusage r0, r1;
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f(1024, 0); glVertex2f(1024, 1024); glVertex2f(0, 1024);
        glEnd();
        checkgl("draw (pre-finish)");

        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        glFinish();
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        checkgl("finish");

        double wms = wall_ms(&w0, &w1), cms = cpu_ms(&r0, &r1);
        printf("trial %d: glFinish() wall=%.1fms cpu=%.1fms (cpu/wall=%.1f%%) -> %s\n",
               trial, wms, cms, wms > 0 ? 100.0 * cms / wms : 0.0,
               cms > wms * 0.7 ? "BUSY-WAITS" : (cms < wms * 0.2 ? "genuinely yields/blocks" : "partially busy"));
    }

    /* Item 9 follow-up: the real pipeline's MC dispatch showed "draw+finish"
     * at 92% CPU even after caching uniform locations ruled that out as the
     * cause - so is glFinish()'s OWN per-call overhead (not busy-waiting,
     * but a real fixed kernel/IPC round-trip cost) the dominant factor once
     * actual GPU work is tiny, unlike the heavy-shader case above where a
     * long wait amortizes it away? Mimic the real dispatch scale (small
     * viewport, trivial shader, many repeated calls) and break the SAME
     * sequence into state-setup+draw vs. glFinish. */
    GLhandleARB lightProg = linkp(vs_plain, "void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}");
    glUseProgramObjectARB(lightProg);
    glViewport(0, 0, 16, 16);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 16, 0, 16, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    const int NREP = 200;
    double setup_wall = 0, setup_cpu = 0, finish_wall = 0, finish_cpu = 0;
    for (int i = 0; i < NREP; i++) {
        struct timeval w0, w1; struct rusage r0, r1;
        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f(16, 0); glVertex2f(16, 16); glVertex2f(0, 16);
        glEnd();
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        setup_wall += wall_ms(&w0, &w1); setup_cpu += cpu_ms(&r0, &r1);

        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        glFinish();
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        finish_wall += wall_ms(&w0, &w1); finish_cpu += cpu_ms(&r0, &r1);
    }
    checkgl("light loop");
    printf("\nSmall-dispatch loop (16x16, trivial shader, %d reps):\n", NREP);
    printf("  clear+draw (no finish): wall=%.2fms cpu=%.2fms total (%.3fms/%.3fms per call, cpu/wall=%.1f%%)\n",
           setup_wall, setup_cpu, setup_wall/NREP, setup_cpu/NREP, setup_wall>0?100.0*setup_cpu/setup_wall:0.0);
    printf("  glFinish() alone: wall=%.2fms cpu=%.2fms total (%.3fms/%.3fms per call, cpu/wall=%.1f%%)\n",
           finish_wall, finish_cpu, finish_wall/NREP, finish_cpu/NREP, finish_wall>0?100.0*finish_cpu/finish_wall:0.0);

    /* Follow-up: the tight loop above never changes viewport/program/
     * texture bindings (real dispatch_singlepass_group does, every call -
     * vw varies per chunk, textures/program get rebound). Isolate whether
     * STATE CHANGES (not the draw or the wait) are the real per-call cost
     * on this driver. */
    GLuint tex[3]; glGenTextures(3, tex);
    unsigned char texdata[16 * 16 * 4];
    for (int t = 0; t < 3; t++) {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[t]);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texdata);
    }
    GLint loc0 = glGetUniformLocationARB(lightProg, "unused0"); (void)loc0;
    double state_wall = 0, state_cpu = 0;
    for (int i = 0; i < NREP; i++) {
        struct timeval w0, w1; struct rusage r0, r1;
        int vw = 16 + (i % 64); /* vary viewport, matching real per-chunk resize */
        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        glViewport(0, 0, vw, 16);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw, 0, 16, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glUseProgramObjectARB(lightProg);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[0]);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[1]);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[2]);
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f(vw, 0); glVertex2f(vw, 16); glVertex2f(0, 16);
        glEnd();
        glFinish();
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        state_wall += wall_ms(&w0, &w1); state_cpu += cpu_ms(&r0, &r1);
    }
    checkgl("state-change loop");
    printf("\nFull per-call sequence incl. viewport-resize+3-texture-rebind+finish (%d reps):\n", NREP);
    printf("  total: wall=%.2fms cpu=%.2fms (%.3fms/%.3fms per call, cpu/wall=%.1f%%)\n",
           state_wall, state_cpu, state_wall/NREP, state_cpu/NREP, state_wall>0?100.0*state_cpu/state_wall:0.0);

    /* Real dispatch_singlepass_group draw+finish averages ~2-3ms/call in
     * the actual pipeline (1295.7ms / ~450 calls) - between this probe's
     * two extremes (sub-ms "tiny", 84ms "heavy"). Scan intermediate wait
     * lengths directly (vary the heavy shader's loop iteration count) to
     * find where the driver's glFinish() CPU% actually transitions -
     * confirms or refutes a spin-then-sleep threshold theory with real
     * data instead of guessing from two endpoints. */
    printf("\nWait-length scan (varying shader loop count -> real GPU wait length):\n");
    GLhandleARB scanProg[6];
    const int iters[6] = {5, 20, 50, 100, 200, 400};
    char srcbuf[512];
    for (int s = 0; s < 6; s++) {
        snprintf(srcbuf, sizeof srcbuf,
            "void main(){float x=gl_FragCoord.x*0.001+gl_FragCoord.y*0.002;float acc=0.0;"
            "for(int i=0;i<%d;i++){x=sin(x)*cos(x*1.3)+sqrt(abs(x)+0.001);acc+=x;}"
            "gl_FragColor=vec4(acc,acc,acc,1.0);}", iters[s]);
        scanProg[s] = linkp(vs_plain, srcbuf);
    }
    glViewport(0, 0, 1024, 1024);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 1024, 0, 1024, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    for (int s = 0; s < 6; s++) {
        glUseProgramObjectARB(scanProg[s]);
        double tot_w = 0, tot_c = 0;
        const int REPS = 10;
        for (int rep = 0; rep < REPS; rep++) {
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_QUADS);
            glVertex2f(0, 0); glVertex2f(1024, 0); glVertex2f(1024, 1024); glVertex2f(0, 1024);
            glEnd();
            struct timeval w0, w1; struct rusage r0, r1;
            getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
            glFinish();
            gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
            tot_w += wall_ms(&w0, &w1); tot_c += cpu_ms(&r0, &r1);
        }
        checkgl("scan");
        printf("  iters=%3d: glFinish() avg wall=%.2fms cpu=%.2fms (cpu/wall=%.1f%%)\n",
               iters[s], tot_w/REPS, tot_c/REPS, tot_w>0?100.0*tot_c/tot_w:0.0);
    }

    /* Decisive remaining variable: NEITHER synthetic test above actually
     * samples a texture during the shader's real work - real
     * fs_mc_batch_var does real sampler2DRect texture2DRect() fetches from
     * a GL_RGBA_FLOAT32_ATI texture (float format, not GL_RGBA/UNSIGNED_BYTE
     * like the rebind-loop test used). Test real float-texture sampling
     * combined with glFinish() directly. */
    printf("\nFloat-texture-sampling shader + glFinish (isolates real fs_mc_batch_var-like cost):\n");
    GLuint bigTex; glGenTextures(1, &bigTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, bigTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    float *bigbuf = malloc(sizeof(float) * 512 * 512 * 4);
    for (int i = 0; i < 512*512*4; i++) bigbuf[i] = 0.5f;
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, 512, 512, 0, GL_RGBA, GL_FLOAT, bigbuf);
    checkgl("bigTex upload");

    GLhandleARB texProg = linkp(vs_plain,
        "uniform sampler2DRect refTex;\n"
        "void main(){\n"
        "  float acc = 0.0;\n"
        "  for (int i = 0; i < 40; i++) {\n"
        "    float fi = float(i);\n"
        "    acc += texture2DRect(refTex, gl_FragCoord.xy + vec2(fi, 0.0)).r;\n"
        "  }\n"
        "  gl_FragColor = vec4(acc/40.0, 0.0, 0.0, 1.0);\n"
        "}\n");
    glUseProgramObjectARB(texProg);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, bigTex);
    glUniform1iARB(glGetUniformLocationARB(texProg, "refTex"), 0);
    glViewport(0, 0, 400, 16);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 400, 0, 16, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    {
        double tot_w = 0, tot_c = 0;
        const int REPS = 30;
        struct timeval w0, w1; struct rusage r0, r1;
        for (int rep = 0; rep < REPS; rep++) {
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_QUADS);
            glVertex2f(0, 0); glVertex2f(400, 0); glVertex2f(400, 16); glVertex2f(0, 16);
            glEnd();
            getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
            glFinish();
            gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
            tot_w += wall_ms(&w0, &w1); tot_c += cpu_ms(&r0, &r1);
        }
        checkgl("tex sampling loop");
        printf("  400x16, 40 texture2DRect fetches/fragment: glFinish() avg wall=%.3fms cpu=%.3fms (cpu/wall=%.1f%%)\n",
               tot_w/REPS, tot_c/REPS, tot_w>0?100.0*tot_c/tot_w:0.0);
        /* Also time WITH the readback, matching the real call's own
         * combined "draw+finish" scope (which in the real code does NOT
         * include the readback - that's timed separately - but let's also
         * check readback cost for this exact texture-sampling case). */
        unsigned char *smallpix = malloc(400 * 16 * 4);
        getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
        glReadPixels(0, 0, 400, 16, GL_RGBA, GL_UNSIGNED_BYTE, smallpix);
        gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
        printf("  readback (400x16 after already-finished draw): wall=%.3fms cpu=%.3fms (cpu/wall=%.1f%%)\n",
               wall_ms(&w0,&w1), cpu_ms(&r0,&r1), wall_ms(&w0,&w1)>0?100.0*cpu_ms(&r0,&r1)/wall_ms(&w0,&w1):0.0);
    }

    unsigned char *pixels = malloc(1024 * 1024 * 4);
    struct timeval w0, w1; struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0); gettimeofday(&w0, NULL);
    glReadPixels(0, 0, 1024, 1024, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    gettimeofday(&w1, NULL); getrusage(RUSAGE_SELF, &r1);
    double wms = wall_ms(&w0, &w1), cms = cpu_ms(&r0, &r1);
    printf("glReadPixels alone (after already-finished draw): wall=%.1fms cpu=%.1fms (cpu/wall=%.1f%%)\n",
           wms, cms, wms > 0 ? 100.0 * cms / wms : 0.0);

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return 0;
}
