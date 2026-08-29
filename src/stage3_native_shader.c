/* PROMO4 Stage 3: reproduce an already-proven GLSL shader in native R5xx US (fragment) ISA.
 *
 * Target: "void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}" - the exact GLSL shader already
 * compiled and run successfully on this hardware in finish_probe.c's `lightProg` earlier this
 * project. Real, already-proven, trivial - a good Stage 3 target per the proposal's own wording.
 *
 * Design (three-point test, not a bare "did it look red" check):
 *   1. Real AGL context, real GLSL compile of the target shader, real draw + readback -> baseline.
 *   2. Open a second, independent, hand-built IOKit connection (same proven pattern as Stage 1/2),
 *      real context allocation (Stage 2a), inject a hand-encoded BLUE program (vec4(0,0,1,1)) at
 *      an unused instruction slot (100, far from where a freshly-compiled trivial shader would
 *      land), redirect US_CODE_ADDR there, redraw the SAME quad with the SAME bound GL program
 *      object, readback -> control. This proves the redirect mechanism actually takes effect -
 *      if it didn't, this draw would still show the real baseline's red, not blue.
 *   3. Overwrite the SAME slot with a hand-encoded RED program (vec4(1,0,0,1) - the real target),
 *      redraw, readback -> test. Compare against the real baseline from step 1.
 *
 * All real instruction words derived directly from AMD's own R5xx Acceleration doc (US_CMN_INST,
 * US_ALU_RGB_ADDR, US_ALU_ALPHA_ADDR, US_ALU_RGB_INST, US_ALU_ALPHA_INST, US_ALU_RGBA_INST bit
 * fields; GA_US_VECTOR_INDEX/DATA load mechanism; US_CODE_ADDR). Submission uses exactly the real,
 * proven mechanism from Stage 2 (fill remaining payload with real PM4 Type-2 filler, trigger the
 * real re-map).
 *
 * Usage: stage3-native-shader
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *krstr(kern_return_t kr) {
    static char buf[32];
    snprintf(buf, sizeof buf, "0x%x", kr);
    return buf;
}

/* ---- GLSL helpers (same pattern as finish_probe.c) ---- */
static void checkgl(const char *w) { GLenum e = glGetError(); if (e) fprintf(stderr, "GL err %s: 0x%x\n", w, (unsigned)e); }
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
static const char *fs_red   = "void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}";

static void draw_quad_and_read(unsigned char out[4]) {
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(4, 0); glVertex2f(4, 4); glVertex2f(0, 4);
    glEnd();
    glFinish();
    checkgl("draw");
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    checkgl("readpixels");
}

/* ---- raw IOKit / real PM4 native-shader injection ---- */

/* real register dword (MMIO/4) addresses, from AMD's R5xx doc register reference */
#define REG_GA_US_VECTOR_INDEX 0x1094u  /* MMReg 0x4250 */
#define REG_GA_US_VECTOR_DATA  0x1095u  /* MMReg 0x4254 */
#define REG_US_CODE_ADDR       0x118Cu  /* MMReg 0x4630 */

static uint32_t type0_header(uint32_t baseIndex, uint32_t countMinus1, int oneRegWr) {
    return (baseIndex & 0x1FFFu) | ((uint32_t)(oneRegWr ? 1 : 0) << 15) |
           ((countMinus1 & 0x3FFFu) << 16) | (0u << 30);
}

/* writes: [Type-0 hdr, INDEX val] [Type-0 hdr(ONE_REG_WR), 6 instruction dwords] [Type-0 hdr, US_CODE_ADDR val] */
static uint32_t *emit_load_program(uint32_t *p, uint32_t slot,
                                    uint32_t cmn, uint32_t rgbAddr, uint32_t alphaAddr,
                                    uint32_t rgbInst, uint32_t alphaInst, uint32_t rgbaInst) {
    /* GA_US_VECTOR_INDEX = TYPE(0=inst)<<16 | INDEX */
    *p++ = type0_header(REG_GA_US_VECTOR_INDEX, 0, 0);
    *p++ = (slot & 0x1FFu);

    /* burst 6 dwords into GA_US_VECTOR_DATA (ONE_REG_WR=1, same register 6 times) */
    *p++ = type0_header(REG_GA_US_VECTOR_DATA, 5, 1);
    *p++ = cmn;
    *p++ = rgbAddr;
    *p++ = alphaAddr;
    *p++ = rgbInst;
    *p++ = alphaInst;
    *p++ = rgbaInst;

    /* US_CODE_ADDR: START_ADDR = END_ADDR = slot (single-instruction program) */
    *p++ = type0_header(REG_US_CODE_ADDR, 0, 0);
    *p++ = ((slot & 0x1FFu) << 16) | (slot & 0x1FFu);

    return p;
}

/* shared instruction fields for both test programs (only RGB_INST's swizzle differs) */
#define US_CMN_INST_VAL    0x001F8105u
#define US_ALU_RGB_ADDR_VAL 0x00000000u
#define US_ALU_ALPHA_ADDR_VAL 0x00000000u
#define US_ALU_ALPHA_INST_VAL 0x00C18000u
#define US_ALU_RGBA_INST_VAL  0x20490000u
#define US_ALU_RGB_INST_RED   0x00DB0498u  /* R=One,G=Zero,B=Zero * B=One,One,One + C=0 -> (1,0,0) */
#define US_ALU_RGB_INST_BLUE  0x00DB0690u  /* R=Zero,G=Zero,B=One * B=One,One,One + C=0 -> (0,0,1) */
#define TEST_SLOT 100u

static int submit_program(io_connect_t conn, vm_address_t cmdBuf, vm_size_t cmdBufSize,
                           uint32_t rgbInstVal, const char *label) {
    const uint32_t HEADER_BYTES = 0x20;
    if (cmdBufSize <= HEADER_BYTES) { fprintf(stderr, "[stage3:%s] buffer too small\n", label); return 0; }
    uint32_t *payload = (uint32_t *)(cmdBuf + HEADER_BYTES);
    vm_size_t payloadDwords = (cmdBufSize - HEADER_BYTES) / 4;

    uint32_t *p = payload;
    p = emit_load_program(p, TEST_SLOT, US_CMN_INST_VAL, US_ALU_RGB_ADDR_VAL, US_ALU_ALPHA_ADDR_VAL,
                           rgbInstVal, US_ALU_ALPHA_INST_VAL, US_ALU_RGBA_INST_VAL);
    vm_size_t usedDwords = (vm_size_t)(p - payload);
    fprintf(stderr, "[stage3:%s] wrote %lu real register-write dwords, padding remaining %lu with PM4 Type-2 filler\n",
            label, (unsigned long)usedDwords, (unsigned long)(payloadDwords - usedDwords));
    for (vm_size_t i = usedDwords; i < payloadDwords; i++) payload[i] = 0x80000000u;

    vm_address_t newAddr = 0; vm_size_t newSize = 0;
    kern_return_t kr = IOConnectMapMemory(conn, 1, mach_task_self(), &newAddr, &newSize, 1);
    fprintf(stderr, "[stage3:%s] re-map (submit) result: %s\n", label, krstr(kr));
    if (kr != KERN_SUCCESS) return 0;

    int tries = 0; const int MAX_TRIES = 200; kern_return_t fkr;
    do { fkr = IOConnectMethodScalarIStructureI(conn, 8, 0, 0, NULL); tries++; }
    while (fkr == (kern_return_t)0xE00002D6 && tries < MAX_TRIES);
    fprintf(stderr, "[stage3:%s] selector 8 (finish) after %d tries: %s\n", label, tries, krstr(fkr));
    return fkr == KERN_SUCCESS;
}

int main(void) {
    /* 1. real, isolated AGL context + pbuffer */
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(4, 4, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    GLhandleARB prog = linkp(vs_plain, fs_red);
    glUseProgramObjectARB(prog);

    unsigned char baseline[4] = {0,0,0,0};
    draw_quad_and_read(baseline);
    fprintf(stderr, "[stage3] real GLSL baseline (compiler-generated native code): RGBA=(%d,%d,%d,%d)\n",
            baseline[0], baseline[1], baseline[2], baseline[3]);

    /* 2. second, independent raw IOKit connection + real context allocation */
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    if (service == 0) { fprintf(stderr, "[stage3] no matching service\n"); return 1; }
    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "[stage3] IOServiceOpen failed: %s\n", krstr(kr)); return 1; }

    int o0=0,o1=0,o2=0;
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0, &o1, &o2);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "[stage3] selector 3 failed: %s\n", krstr(kr)); IOServiceClose(conn); return 1; }

    struct { uint32_t type; uint32_t options; } maps[4] = { {0,0x101}, {1,1}, {2,1}, {4,1} };
    vm_address_t addrs[4]={0,0,0,0}; vm_size_t sizes[4]={0,0,0,0}; int mapped=0;
    for (int i=0;i<4;i++) {
        kr = IOConnectMapMemory(conn, maps[i].type, mach_task_self(), &addrs[i], &sizes[i], maps[i].options);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "[stage3] map type=%u failed: %s\n", maps[i].type, krstr(kr)); break; }
        mapped = i+1;
    }
    if (mapped != 4) {
        fprintf(stderr, "[stage3] could not allocate real context memory - aborting shader injection\n");
        for (int i=mapped-1;i>=0;i--) IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]);
        IOServiceClose(conn);
        return 1;
    }
    fprintf(stderr, "[stage3] real second connection + context memory allocated, cmdBuf=0x%lx size=0x%lx\n",
            (unsigned long)addrs[1], (unsigned long)sizes[1]);

    /* 3. inject hand-encoded BLUE control program, redraw the SAME bound GL program object */
    int blueOk = submit_program(conn, addrs[1], sizes[1], US_ALU_RGB_INST_BLUE, "control-blue");
    unsigned char control[4] = {0,0,0,0};
    draw_quad_and_read(control);
    fprintf(stderr, "[stage3] after BLUE injection, redraw result: RGBA=(%d,%d,%d,%d)\n",
            control[0], control[1], control[2], control[3]);

    /* 4. overwrite with hand-encoded RED (the real target), redraw again */
    int redOk = submit_program(conn, addrs[1], sizes[1], US_ALU_RGB_INST_RED, "test-red");
    unsigned char test[4] = {0,0,0,0};
    draw_quad_and_read(test);
    fprintf(stderr, "[stage3] after RED injection, redraw result: RGBA=(%d,%d,%d,%d)\n",
            test[0], test[1], test[2], test[3]);

    /* cleanup */
    for (int i=mapped-1;i>=0;i--) {
        kr = IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]);
        fprintf(stderr, "[stage3] unmap type=%u: %s\n", maps[i].type, krstr(kr));
    }
    IOServiceClose(conn);
    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);

    /* verdict */
    int redirectProven = (blueOk && (control[0]!=baseline[0] || control[1]!=baseline[1] ||
                                      control[2]!=baseline[2] || control[3]!=baseline[3]));
    int reproductionMatches = (redOk && test[0]==baseline[0] && test[1]==baseline[1] &&
                                test[2]==baseline[2] && test[3]==baseline[3]);

    fprintf(stderr, "\n[stage3] === VERDICT ===\n");
    fprintf(stderr, "[stage3] baseline (real GLSL):    RGBA=(%d,%d,%d,%d)\n", baseline[0],baseline[1],baseline[2],baseline[3]);
    fprintf(stderr, "[stage3] control  (hand blue):    RGBA=(%d,%d,%d,%d) - redirect mechanism %s\n",
            control[0],control[1],control[2],control[3], redirectProven ? "PROVEN (differs from baseline)" : "NOT PROVEN (matches baseline or submit failed)");
    fprintf(stderr, "[stage3] test     (hand red):     RGBA=(%d,%d,%d,%d) - native reproduction %s\n",
            test[0],test[1],test[2],test[3], reproductionMatches ? "MATCHES real GLSL baseline" : "DOES NOT MATCH baseline");

    return (redirectProven && reproductionMatches) ? 0 : 1;
}
