/* PROMO4 Stage 3n: microbenchmark - sync cost of glFinish() vs glFlush()+poll of the GPU retire stamp (memory type 0, word 0, little-endian;
 * found by stage3m: advances once per retired submission). Read-only use of the stamp page; the GL work is a trivial clear+quad.
 * For each rep: A) draw; glFinish().   B) draw; glFlush(); spin on the stamp until it changes from its pre-flush value.
 * Reports per-rep wall and CPU time. Usage: stage3n-poll-fence LOGFILE [REPS]
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>

static FILE *g_log;
static void LOG(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); fcntl(fileno(g_log), F_FULLFSYNC, 0); }
}
static uint32_t bswap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }
static double now_ms(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec * 1000.0 + t.tv_usec / 1000.0; }
static double cpu_ms(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec * 1000.0 + r.ru_utime.tv_usec / 1000.0 + r.ru_stime.tv_sec * 1000.0 + r.ru_stime.tv_usec / 1000.0; }
static int g_heavy = 0;
static void work(void) {
    if (g_heavy) { glClear(GL_COLOR_BUFFER_BIT); glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4); glEnd(); return; }
    glClear(GL_COLOR_BUFFER_BIT); glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4); glEnd(); }

int main(int argc, char **argv) {
    g_log = argc > 1 ? fopen(argv[1], "a") : NULL;
    int reps = argc > 2 ? atoi(argv[2]) : 200, r;
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(512, 512, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glViewport(0, 0, 16, 16); glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    if (getenv("HEAVY")) {
        g_heavy = 1; glViewport(0, 0, 512, 512);
        const char *fs = "void main(){float a=gl_FragCoord.x*0.001; for(int i=0;i<400;i++){a=sin(a)*1.5+cos(a*0.7);} gl_FragColor=vec4(a,0.0,0.0,1.0);}";
        GLhandleARB v = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB), f = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB), pr = glCreateProgramObjectARB();
        const char *vs = "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}";
        glShaderSourceARB(v, 1, &vs, NULL); glCompileShaderARB(v); glShaderSourceARB(f, 1, &fs, NULL); glCompileShaderARB(f);
        glAttachObjectARB(pr, v); glAttachObjectARB(pr, f); glLinkProgramARB(pr); glUseProgramObjectARB(pr);
    }
    work(); glFinish();
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    io_connect_t conn = MACH_PORT_NULL; kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 1, &conn); IOObjectRelease(svc);
    vm_address_t a0 = 0; vm_size_t s0 = 0;
    if (kr || IOConnectMapMemory(conn, 0, mach_task_self(), &a0, &s0, 0x101)) { LOG("cannot map type 0\n"); return 1; }
    volatile uint32_t *stamp = (volatile uint32_t *)a0;
    LOG("[3n] stamp page %p, initial stamp 0x%x\n", (void *)a0, bswap(*stamp));
    double w0, c0, tA, cA, tB, cB; long spins = 0, timeouts = 0;
    w0 = now_ms(); c0 = cpu_ms();
    for (r = 0; r < reps; r++) { work(); glFinish(); }
    tA = now_ms() - w0; cA = cpu_ms() - c0;
    w0 = now_ms(); c0 = cpu_ms();
    for (r = 0; r < reps; r++) {
        uint32_t before = *stamp; work(); glFlush();
        long n = 0; while ((bswap(*stamp) - bswap(before)) < 2 && n < 20000000) n++;   /* each flush retires 2 stamps (preamble + job): observed in TIMELINE */
        if (n >= 20000000) timeouts++;
        spins += n;
    }
    tB = now_ms() - w0; cB = cpu_ms() - c0;
    LOG("[3n] %d reps. A glFinish:            wall %.3f ms/rep, cpu %.3f ms/rep\n", reps, tA / reps, cA / reps);
    LOG("[3n]        B glFlush+poll stamp:  wall %.3f ms/rep, cpu %.3f ms/rep (avg %.0f spins, %ld timeouts), final stamp 0x%x\n", tB / reps, cB / reps, (double)spins / reps, timeouts, bswap(*stamp));
    if (g_heavy && getenv("TIMELINE")) {   /* time-resolved: when does the stamp change relative to flush return and to actual job completion? */
        int k;
        for (k = 0; k < 3; k++) {
            double t0, tf, t; uint32_t last, v; int nch = 0; double ct[16]; uint32_t cv[16];
            work(); glFinish();
            last = *stamp; t0 = now_ms(); work(); glFlush(); tf = now_ms() - t0;
            while ((t = now_ms() - t0) < 40.0) { v = *stamp; if (v != last && nch < 16) { ct[nch] = t; cv[nch++] = v; last = v; } }
            double tfin0 = now_ms(); glFinish(); double tfin = now_ms() - tfin0;
            LOG("[3n] TIMELINE run %d: glFlush returned at %.3f ms; stamp changes at:", k, tf);
            { int j; for (j = 0; j < nch; j++) LOG(" %.3fms(0x%x)", ct[j], bswap(cv[j])); }
            LOG("; glFinish issued at 40ms took %.3f ms\n", tfin);
            /* reference: same job, glFinish immediately */
            t0 = now_ms(); work(); glFinish(); LOG("[3n]    reference draw+glFinish = %.3f ms\n", now_ms() - t0);
        }
    }
    if (g_heavy) {   /* did the stamp move at SUBMIT or at COMPLETION? time one heavy job three ways */
        double t0, t1, t2; uint32_t before;
        work(); glFinish();
        before = *stamp; t0 = now_ms(); work(); glFlush(); t1 = now_ms(); while (*stamp == before) ; t2 = now_ms(); 
        LOG("[3n] HEAVY: flush returned after %.3f ms, stamp changed after %.3f ms; ", t1 - t0, t2 - t0);
        t0 = now_ms(); glFinish(); LOG("remaining glFinish %.3f ms\n", now_ms() - t0);
        t0 = now_ms(); work(); glFinish(); LOG("[3n] HEAVY: full draw+glFinish = %.3f ms\n", now_ms() - t0);
    }
    glFinish();
    aglSetCurrentContext(NULL); aglDestroyContext(ctx);
    return 0;
}
