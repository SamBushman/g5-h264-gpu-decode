/* Frame-level GPU decode prototype v0 (issue #1): ONE 1080p inter frame, luma only, plain GLSL on the stock driver.
 *
 * Question being answered: can MC + residual for a whole 1088-row luma plane run in a handful of draws with O(1) round trips, fast enough
 * (budget ~25-33 ms/frame for the whole GPU stage at 1080p30) once uploads are included?  Deblock is NOT in v0 (see the note at the end).
 *
 *   pass MC  : 9 phase-class shaders, each drawn only over the 8x8 blocks of its class (client-array quads), writing predTex (FBO, RECT RGBA8).
 *              Branch-free fetches (no texture2D inside a runtime branch - quirk 2); fetch offsets are chosen arithmetically.
 *   pass RES : one full-plane draw: 4 coefficient fetches/pixel (coefficients packed as columns in RGBA32F), separable H.264 4x4 inverse
 *              transform exactly as FFmpeg/h264 (stage 1 down each column with the floor(x/2) terms, stage 2 across the row, (x+32)>>6),
 *              add to predTex, clip, write the default framebuffer (the pbuffer) - read back for verification only.
 *
 * Synthetic but structurally realistic data (all 16 quarter-pel phases, 25% of MBs with four distinct 8x8 MVs, ~20% of 4x4 blocks coded).
 * A CPU reference (straight from the spec) is compared bit-for-bit. Quirks respected: RECT textures only, floor(gl_FragCoord) addressing,
 * pbuffer >= largest viewport (15), FBO only ever verified by sampling (4/6), no unrounded multi-stage chains except the proven j filter (14).
 *
 * Usage: frame-proto [W H] [ITERS]        (defaults 1920 1088 20)
 */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/time.h>

static double now_ms(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec * 1000.0 + t.tv_usec / 1000.0; }
static uint32_t rng_s = 12345;
static uint32_t rnd(void) { rng_s = rng_s * 1664525u + 1013904223u; return rng_s >> 8; }
static int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static int W = 1920, H = 1088;
static uint8_t *ref, *cpu_out;
static int *mvx8, *mvy8;           /* per 8x8 block, quarter-pel */
static float *coefp;               /* packed coefficients: texel (bx, by*4+j) = column j: (c[0][j],c[1][j],c[2][j],c[3][j]) */
static int *coefi;                 /* [(by*nbx+bx)*16 + r*4+c] */

/* ---------------- CPU reference ---------------- */
static int R(int x, int y) { if (x < 0) x = 0; if (x >= W) x = W - 1; if (y < 0) y = 0; if (y >= H) y = H - 1; return ref[y * W + x]; }
static int tapH_raw(int x, int y) { return (R(x, y) + R(x + 1, y)) * 20 - (R(x - 1, y) + R(x + 2, y)) * 5 + R(x - 2, y) + R(x + 3, y); }
static int tapV_raw(int x, int y) { return (R(x, y) + R(x, y + 1)) * 20 - (R(x, y - 1) + R(x, y + 2)) * 5 + R(x, y - 2) + R(x, y + 3); }
static int halfH(int x, int y) { return clip255((tapH_raw(x, y) + 16) >> 5); }
static int halfV(int x, int y) { return clip255((tapV_raw(x, y) + 16) >> 5); }
static int centerJ(int x, int y) {
    int v = (tapH_raw(x, y) + tapH_raw(x, y + 1)) * 20 - (tapH_raw(x, y - 1) + tapH_raw(x, y + 2)) * 5 + tapH_raw(x, y - 2) + tapH_raw(x, y + 3);
    return clip255((v + 512) >> 10);
}
static int mc_pixel(int x, int y, int mvx, int mvy) {
    int ix = x + (mvx >> 2), iy = y + (mvy >> 2), fx = mvx & 3, fy = mvy & 3;
    int G = R(ix, iy);
    switch (fy * 4 + fx) {
    case 0: return G;
    case 1: return (G + halfH(ix, iy) + 1) >> 1;
    case 2: return halfH(ix, iy);
    case 3: return (R(ix + 1, iy) + halfH(ix, iy) + 1) >> 1;
    case 4: return (G + halfV(ix, iy) + 1) >> 1;
    case 5: return (halfH(ix, iy) + halfV(ix, iy) + 1) >> 1;
    case 6: return (halfH(ix, iy) + centerJ(ix, iy) + 1) >> 1;
    case 7: return (halfH(ix, iy) + halfV(ix + 1, iy) + 1) >> 1;
    case 8: return halfV(ix, iy);
    case 9: return (halfV(ix, iy) + centerJ(ix, iy) + 1) >> 1;
    case 10: return centerJ(ix, iy);
    case 11: return (centerJ(ix, iy) + halfV(ix + 1, iy) + 1) >> 1;
    case 12: return (R(ix, iy + 1) + halfV(ix, iy) + 1) >> 1;
    case 13: return (halfH(ix, iy + 1) + halfV(ix, iy) + 1) >> 1;
    case 14: return (centerJ(ix, iy) + halfH(ix, iy + 1) + 1) >> 1;
    default: return (halfH(ix, iy + 1) + halfV(ix + 1, iy) + 1) >> 1;
    }
}
static void idct4(const int *c, int *out) {            /* c[r*4+col] -> out[r*4+col] residual (already >>6 with +32) */
    int m[16], z0, z1, z2, z3, j, k;
    for (j = 0; j < 4; j++) {
        z0 = c[0 * 4 + j] + c[2 * 4 + j]; z1 = c[0 * 4 + j] - c[2 * 4 + j];
        z2 = (c[1 * 4 + j] >> 1) - c[3 * 4 + j]; z3 = c[1 * 4 + j] + (c[3 * 4 + j] >> 1);
        m[0 * 4 + j] = z0 + z3; m[1 * 4 + j] = z1 + z2; m[2 * 4 + j] = z1 - z2; m[3 * 4 + j] = z0 - z3;
    }
    for (k = 0; k < 4; k++) {
        z0 = m[k * 4 + 0] + m[k * 4 + 2]; z1 = m[k * 4 + 0] - m[k * 4 + 2];
        z2 = (m[k * 4 + 1] >> 1) - m[k * 4 + 3]; z3 = m[k * 4 + 1] + (m[k * 4 + 3] >> 1);
        out[k * 4 + 0] = (z0 + z3 + 32) >> 6; out[k * 4 + 1] = (z1 + z2 + 32) >> 6;
        out[k * 4 + 2] = (z1 - z2 + 32) >> 6; out[k * 4 + 3] = (z0 - z3 + 32) >> 6;
    }
}

/* ---------------- GL helpers ---------------- */
static GLhandleARB compile(GLenum t, const char *s) {
    GLhandleARB h = glCreateShaderObjectARB(t); glShaderSourceARB(h, 1, &s, NULL); glCompileShaderARB(h);
    GLint ok = 0; glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(h, sizeof log, &n, log); fprintf(stderr, "compile fail:\n%s\n--- source:\n%s\n", log, s); exit(1); }
    return h;
}
static GLhandleARB linkp(const char *fs) {
    GLhandleARB p = glCreateProgramObjectARB();
    glAttachObjectARB(p, compile(GL_VERTEX_SHADER_ARB, "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}"));
    glAttachObjectARB(p, compile(GL_FRAGMENT_SHADER_ARB, fs)); glLinkProgramARB(p);
    GLint ok = 0; glGetObjectParameterivARB(p, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(p, sizeof log, &n, log); fprintf(stderr, "link fail:\n%s\n", log); exit(1); }
    return p;
}

/* MC shader, parametrised by phase class (see classify()). Common prologue fetches the 8x8 block's MV and positions p at the integer ref pixel. */
static const char *mc_common =
"uniform sampler2DRect refTex; uniform sampler2DRect mvTex;\n"
"vec2 p; float fx; float fy;\n"
"#define RF(dx,dy) (texture2DRect(refTex, vec2(p.x + (dx), p.y + (dy))).r * 255.0)\n"
"#define RFI(dx,dy) floor(RF(dx,dy) + 0.5)\n"
"#define TAPH(ox,oy) floor(((RF((ox),(oy))+RF((ox)+1.0,(oy)))*20.0 - (RF((ox)-1.0,(oy))+RF((ox)+2.0,(oy)))*5.0 + RF((ox)-2.0,(oy)) + RF((ox)+3.0,(oy))) + 0.5)\n"
"#define TAPV(ox,oy) floor(((RF((ox),(oy))+RF((ox),(oy)+1.0))*20.0 - (RF((ox),(oy)-1.0)+RF((ox),(oy)+2.0))*5.0 + RF((ox),(oy)-2.0) + RF((ox),(oy)+3.0)) + 0.5)\n"
"#define CLIP(v) max(0.0, min(255.0, (v)))\n"
"#define HALFH(ox,oy) CLIP(floor((TAPH(ox,oy)+16.0)*0.03125))\n"
"#define HALFV(ox,oy) CLIP(floor((TAPV(ox,oy)+16.0)*0.03125))\n"
"#define CENTERJ CLIP(floor(((TAPH(0.0,0.0)+TAPH(0.0,1.0))*20.0 - (TAPH(0.0,-1.0)+TAPH(0.0,2.0))*5.0 + TAPH(0.0,-2.0) + TAPH(0.0,3.0) + 512.0)*0.0009765625))\n"
"#define AVG(a,b) floor(((a) + (b) + 1.0) * 0.5)\n"
"void setup() {\n"
"  vec2 fc = floor(gl_FragCoord.xy);\n"
"  vec2 mv = texture2DRect(mvTex, vec2(floor(fc.x*0.125)+0.5, floor(fc.y*0.125)+0.5)).rg;\n"
"  vec2 ip = floor(mv * 0.25); fx = mv.x - 4.0*ip.x; fy = mv.y - 4.0*ip.y;\n"
"  p = fc + ip + vec2(0.5);\n"
"}\n";
static const char *mc_body[9] = {
/* 0 full            */ "void main(){ setup(); float r = RFI(0.0,0.0); gl_FragColor = vec4(r/255.0,0.0,0.0,1.0); }",
/* 1 halfH           */ "void main(){ setup(); float r = HALFH(0.0,0.0); gl_FragColor = vec4(r/255.0,0.0,0.0,1.0); }",
/* 2 halfV           */ "void main(){ setup(); float r = HALFV(0.0,0.0); gl_FragColor = vec4(r/255.0,0.0,0.0,1.0); }",
/* 3 h-quarter (fy=0)*/ "void main(){ setup(); float a = HALFH(0.0,0.0); float b = RFI(step(2.5,fx),0.0); gl_FragColor = vec4(AVG(a,b)/255.0,0.0,0.0,1.0); }",
/* 4 v-quarter (fx=0)*/ "void main(){ setup(); float a = HALFV(0.0,0.0); float b = RFI(0.0,step(2.5,fy)); gl_FragColor = vec4(AVG(a,b)/255.0,0.0,0.0,1.0); }",
/* 5 diag quarter    */ "void main(){ setup(); float ro = step(2.5,fy); float co = step(2.5,fx); float a = HALFH(0.0,ro); float b = HALFV(co,0.0); gl_FragColor = vec4(AVG(a,b)/255.0,0.0,0.0,1.0); }",
/* 6 center j        */ "void main(){ setup(); float r = CENTERJ; gl_FragColor = vec4(r/255.0,0.0,0.0,1.0); }",
/* 7 j + halfH (fx=2,fy odd) */ "void main(){ setup(); float ro = step(2.5,fy); float a = CENTERJ; float b = HALFH(0.0,ro); gl_FragColor = vec4(AVG(a,b)/255.0,0.0,0.0,1.0); }",
/* 8 j + halfV (fy=2,fx odd) */ "void main(){ setup(); float co = step(2.5,fx); float a = CENTERJ; float b = HALFV(co,0.0); gl_FragColor = vec4(AVG(a,b)/255.0,0.0,0.0,1.0); }",
};
static int classify(int mvx, int mvy) {
    int fx = mvx & 3, fy = mvy & 3;
    if (!fx && !fy) return 0;
    if (fx == 2 && !fy) return 1;
    if (!fx && fy == 2) return 2;
    if (!fy) return 3;
    if (!fx) return 4;
    if ((fx & 1) && (fy & 1)) return 5;
    if (fx == 2 && fy == 2) return 6;
    if (fx == 2) return 7;
    return 8;
}

static const char *fs_res =
"uniform sampler2DRect coefTex; uniform sampler2DRect idxTex; uniform sampler2DRect predTex;\n"
"vec4 stage1(vec4 v) {   /* one column's 4 coefficients (rows 0..3) -> its 4 transformed values */\n"
"  vec4 h = floor(v * 0.5);\n"
"  float z0 = v.x + v.z, z1 = v.x - v.z, z2 = h.y - v.w, z3 = v.y + h.w;\n"
"  return vec4(z0 + z3, z1 + z2, z1 - z2, z0 - z3);\n"
"}\n"
"vec4 onehot(float k) { return max(vec4(0.0), vec4(1.0) - abs(vec4(0.0,1.0,2.0,3.0) - k)); }\n"
"void main() {\n"
"  vec2 fc = floor(gl_FragCoord.xy);\n"
"  vec2 blk = floor(fc * 0.25); vec2 loc = fc - blk * 4.0;\n"
"  float id = floor(texture2DRect(idxTex, blk + vec2(0.5)).r + 0.5);\n"
"  float lin = id * 4.0; float ry = floor(lin * 0.00048828125); float rx = lin - ry * 2048.0;\n"
"  vec2 b = vec2(rx + 0.5, ry + 0.5);\n"
"  vec4 oh = onehot(loc.y);\n"
"  vec4 m0 = stage1(texture2DRect(coefTex, b));\n"
"  vec4 m1 = stage1(texture2DRect(coefTex, b + vec2(1.0, 0.0)));\n"
"  vec4 m2 = stage1(texture2DRect(coefTex, b + vec2(2.0, 0.0)));\n"
"  vec4 m3 = stage1(texture2DRect(coefTex, b + vec2(3.0, 0.0)));\n"
"  float a0 = dot(m0, oh), a1 = dot(m1, oh), a2 = dot(m2, oh), a3 = dot(m3, oh);\n"
"  float z0 = a0 + a2, z1 = a0 - a2, z2 = floor(a1 * 0.5) - a3, z3 = a1 + floor(a3 * 0.5);\n"
"  vec4 o = vec4(z0 + z3, z1 + z2, z1 - z2, z0 - z3);\n"
"  float res = floor((dot(o, onehot(loc.x)) + 32.0) * 0.015625);\n"
"  float pr = floor(texture2DRect(predTex, fc + vec2(0.5)).r * 255.0 + 0.5);\n"
"  gl_FragColor = vec4(clamp(pr + res, 0.0, 255.0) * 0.00392156862745098, 0.0, 0.0, 1.0);\n"
"}\n";
/* Luma deblock, bS<4 filter (H.264 8.7.2.3), one pass per direction. COST STAND-IN: parameters are synthetic (alpha/beta/tc0 fixed per edge from a texture)
 * and the left-to-right dependency chain between neighbouring edges is NOT reproduced, so the output is not bit-exact vs the spec ordering. Each fragment
 * handles the single edge that can modify it (4x4 edges: pixel at offset r from edge in {-2,-1,0,1}). */
static const char *fs_deblock =
"uniform sampler2DRect srcTex; uniform sampler2DRect prmTex; uniform float dir;\n"
"#define S(k) floor(texture2DRect(srcTex, base + s * (k)).r * 255.0 + 0.5)\n"
"void main() {\n"
"  vec2 fc = floor(gl_FragCoord.xy); vec2 s = vec2(1.0 - dir, dir);\n"
"  float a = dot(fc, s); float x0 = floor((a + 2.0) * 0.25) * 4.0; float r = a - x0;\n"
"  vec2 base = fc - s * r;\n"
"  vec4 prm = floor(texture2DRect(prmTex, floor(base * 0.25) + vec2(0.5)) * 255.0 + 0.5);\n"
"  float alpha = prm.x, beta = prm.y, tc0 = prm.z;\n"
"  float p2 = S(-3.0), p1 = S(-2.0), p0 = S(-1.0), q0 = S(0.0), q1 = S(1.0), q2 = S(2.0);\n"
"  float fil = step(abs(p0 - q0), alpha - 0.5) * step(abs(p1 - p0), beta - 0.5) * step(abs(q1 - q0), beta - 0.5);\n"
"  float ap = step(abs(p2 - p0), beta - 0.5), aq = step(abs(q2 - q0), beta - 0.5);\n"
"  float tc = tc0 + ap + aq;\n"
"  float delta = clamp(floor((((q0 - p0) * 4.0) + (p1 - q1) + 4.0) * 0.125), -tc, tc);\n"
"  float np0 = clamp(p0 + delta, 0.0, 255.0), nq0 = clamp(q0 - delta, 0.0, 255.0);\n"
"  float np1 = p1 + ap * clamp(floor((p2 + floor((p0 + q0 + 1.0) * 0.5) - p1 * 2.0) * 0.5), -tc0, tc0);\n"
"  float nq1 = q1 + aq * clamp(floor((q2 + floor((p0 + q0 + 1.0) * 0.5) - q1 * 2.0) * 0.5), -tc0, tc0);\n"
"  float cur = (r < -1.5) ? p1 : ((r < -0.5) ? p0 : ((r < 0.5) ? q0 : q1));\n"
"  float flt = (r < -1.5) ? np1 : ((r < -0.5) ? np0 : ((r < 0.5) ? nq0 : nq1));\n"
"  float outv = mix(cur, flt, fil);\n"
"  gl_FragColor = vec4(outv * 0.00392156862745098, 0.0, 0.0, 1.0);\n"
"}\n";
static const char *fs_deblock_fetchonly =
"uniform sampler2DRect srcTex; uniform sampler2DRect prmTex; uniform float dir;\n"
"#define S(k) floor(texture2DRect(srcTex, base + s * (k)).r * 255.0 + 0.5)\n"
"void main() {\n"
"  vec2 fc = floor(gl_FragCoord.xy); vec2 s = vec2(1.0 - dir, dir);\n"
"  float a = dot(fc, s); float x0 = floor((a + 2.0) * 0.25) * 4.0; float r = a - x0;\n"
"  vec2 base = fc - s * r;\n"
"  vec4 prm = floor(texture2DRect(prmTex, floor(base * 0.25) + vec2(0.5)) * 255.0 + 0.5);\n"
"  float v = S(-3.0) + S(-2.0) + S(-1.0) + S(0.0) + S(1.0) + S(2.0) + prm.x;\n"
"  gl_FragColor = vec4(v * 0.0005, 0.0, 0.0, 1.0);\n"
"}\n";
static const char *fs_deblock_oneFetch =
"uniform sampler2DRect srcTex; uniform sampler2DRect prmTex; uniform float dir;\n"
"void main() { vec2 fc = floor(gl_FragCoord.xy); gl_FragColor = vec4(texture2DRect(srcTex, fc + vec2(0.5)).r, 0.0, 0.0, 1.0); }\n";
static const char *fs_present =
"uniform sampler2DRect finalTex;\n"
"void main() { gl_FragColor = texture2DRect(finalTex, floor(gl_FragCoord.xy) + vec2(0.5)); }\n";

int main(int argc, char **argv) {
    if (argc >= 3) { W = atoi(argv[1]); H = atoi(argv[2]); }
    int iters = argc >= 4 ? atoi(argv[3]) : 20;
    int nbx8 = W / 8, nby8 = H / 8, nbx4 = W / 4, nby4 = H / 4, i, x, y;
    ref = malloc(W * H); cpu_out = malloc(W * H);
    mvx8 = malloc(nbx8 * nby8 * sizeof(int)); mvy8 = malloc(nbx8 * nby8 * sizeof(int));
    coefi = calloc(nbx4 * nby4 * 16, sizeof(int)); coefp = calloc(nbx4 * nby4 * 16, sizeof(float));
    /* synthetic reference: smooth gradient + texture + noise */
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) ref[y * W + x] = clip255((int)(128 + 60 * sin(x * 0.03) * cos(y * 0.021) + (rnd() % 31) - 15 + ((x / 16 + y / 16) & 1) * 12));
    /* MVs: MB-level base; 25% of MBs split into four 8x8 with their own MV */
    for (y = 0; y < H / 16; y++) for (x = 0; x < W / 16; x++) {
        int bx = (int)(rnd() % 81) - 40, by = (int)(rnd() % 81) - 40, split = (rnd() % 4) == 0, k;
        for (k = 0; k < 4; k++) {
            int ox = x * 2 + (k & 1), oy = y * 2 + (k >> 1);
            mvx8[oy * nbx8 + ox] = split ? (int)(rnd() % 81) - 40 : bx;
            mvy8[oy * nbx8 + ox] = split ? (int)(rnd() % 81) - 40 : by;
        }
    }
    /* coefficients: ~20% of 4x4 blocks coded, 1..6 low-frequency coefficients */
    int ncoded = 0;
    for (i = 0; i < nbx4 * nby4; i++) if (rnd() % 5 == 0) {
        int n = 1 + rnd() % 6, k; ncoded++;
        for (k = 0; k < n; k++) { int r = rnd() % 3, c = rnd() % 3; coefi[i * 16 + r * 4 + c] = (int)(rnd() % 401) - 200; }
    }
    for (y = 0; y < nby4; y++) for (x = 0; x < nbx4; x++) {              /* pack: texel (x, y*4+j) = column j of block (x,y) */
        int j, r;
        for (j = 0; j < 4; j++) for (r = 0; r < 4; r++) coefp[((y * 4 + j) * nbx4 + x) * 4 + r] = (float)coefi[(y * nbx4 + x) * 16 + r * 4 + j];
    }
    printf("frame %dx%d: %d 8x8 MV blocks, %d/%d 4x4 blocks coded\n", W, H, nbx8 * nby8, ncoded, nbx4 * nby4);

    /* CPU reference */
    double t0 = now_ms();
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) cpu_out[y * W + x] = (uint8_t)mc_pixel(x, y, mvx8[(y / 8) * nbx8 + x / 8], mvy8[(y / 8) * nbx8 + x / 8]);
    for (y = 0; y < nby4; y++) for (x = 0; x < nbx4; x++) {
        int res[16], k; idct4(&coefi[(y * nbx4 + x) * 16], res);
        for (k = 0; k < 16; k++) { uint8_t *d = &cpu_out[(y * 4 + k / 4) * W + x * 4 + (k & 3)]; *d = (uint8_t)clip255(*d + res[k]); }
    }
    printf("CPU reference (naive C, not a performance figure): %.0f ms\n", now_ms() - t0);

    /* sparse coefficient atlas: only coded 4x4 blocks, 4 texels (columns) each, 2048 texels per row; idxTex maps block -> atlas id */
    int ncod = 0; float *idxf = malloc(nbx4 * nby4 * sizeof(float));
    for (i = 0; i < nbx4 * nby4; i++) { int any = 0, k; for (k = 0; k < 16; k++) if (coefi[i * 16 + k]) any = 1; idxf[i] = any ? (float)ncod++ : -1.0f; }
    int arows = (ncod * 4 + 2047) / 2048; float *atlas = calloc(2048 * arows * 4, sizeof(float));
    float *cverts = malloc(ncod * 8 * sizeof(float)); int cq = 0;
    for (y = 0; y < nby4; y++) for (x = 0; x < nbx4; x++) if (idxf[y * nbx4 + x] >= 0) {
        int id = (int)idxf[y * nbx4 + x], j, r;
        for (j = 0; j < 4; j++) for (r = 0; r < 4; r++) atlas[((id * 4 + j)) * 4 + r] = (float)coefi[(y * nbx4 + x) * 16 + r * 4 + j];
        float *v = cverts + cq++ * 8, x0 = x * 4, y0 = y * 4; v[0] = x0; v[1] = y0; v[2] = x0 + 4; v[3] = y0; v[4] = x0 + 4; v[5] = y0 + 4; v[6] = x0; v[7] = y0 + 4;
    }
    printf("sparse atlas: %d coded blocks -> %dx%d RGBA32F = %.2f MB (dense was %.1f MB)\n", ncod, 2048, arows, 2048 * arows * 16.0 / 1048576.0, nbx4 * nby4 * 4 * 16.0 / 1048576.0);

    /* GL setup */
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8, AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(2048, 2048, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);   /* quirk 15: >= largest viewport */
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_DITHER);

    GLuint tex[10]; glGenTextures(10, tex);       /* 0 ref, 1 mv, 2 atlas, 3 final (FBO), 4 idx */
    GLenum T = GL_TEXTURE_RECTANGLE_ARB;
    #define NEAREST() do { glTexParameteri(T, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(T, GL_TEXTURE_MAG_FILTER, GL_NEAREST); \
        glTexParameteri(T, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(T, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); } while (0)
    glBindTexture(T, tex[0]); NEAREST(); glTexImage2D(T, 0, GL_LUMINANCE8, W, H, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, ref);
    float *mvf = malloc(nbx8 * nby8 * 4 * sizeof(float));
    for (i = 0; i < nbx8 * nby8; i++) { mvf[i * 4] = (float)mvx8[i]; mvf[i * 4 + 1] = (float)mvy8[i]; mvf[i * 4 + 2] = 0; mvf[i * 4 + 3] = 0; }
    glBindTexture(T, tex[1]); NEAREST(); glTexImage2D(T, 0, GL_RGBA_FLOAT32_ATI, nbx8, nby8, 0, GL_RGBA, GL_FLOAT, mvf);
    glBindTexture(T, tex[2]); NEAREST(); glTexImage2D(T, 0, GL_RGBA_FLOAT32_ATI, 2048, arows, 0, GL_RGBA, GL_FLOAT, atlas);
    glBindTexture(T, tex[3]); NEAREST(); glTexImage2D(T, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(T, tex[4]); NEAREST(); glTexImage2D(T, 0, GL_LUMINANCE_FLOAT32_ATI, nbx4, nby4, 0, GL_LUMINANCE, GL_FLOAT, idxf);
    glBindTexture(T, tex[5]); NEAREST(); glTexImage2D(T, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GLuint fbo; glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, T, tex[3], 0);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) { fprintf(stderr, "FBO incomplete\n"); return 1; }
    /* synthetic edge strengths: bS=2 if either neighbouring 4x4 block is coded, else 1 if the 8x8 MVs differ, else 0 */
    uint8_t *prmV = calloc(nbx4 * nby4 * 4, 1), *prmH = calloc(nbx4 * nby4 * 4, 1); float *ev = malloc(nbx4 * nby4 * 8 * sizeof(float)), *eh = malloc(nbx4 * nby4 * 8 * sizeof(float)); int nev = 0, neh = 0;
    for (y = 0; y < nby4; y++) for (x = 0; x < nbx4; x++) {
        int cur = idxf[y * nbx4 + x] >= 0, k;
        for (k = 0; k < 2; k++) {                       /* 0: vertical edge (left of block), 1: horizontal edge (top of block) */
            int nx = x - (k == 0), ny = y - (k == 1); if (nx < 0 || ny < 0) continue;
            int nb = idxf[ny * nbx4 + nx] >= 0, bs = 0;
            if (cur || nb) bs = 2; else if (mvx8[(y / 2) * nbx8 + x / 2] != mvx8[(ny / 2) * nbx8 + nx / 2] || mvy8[(y / 2) * nbx8 + x / 2] != mvy8[(ny / 2) * nbx8 + nx / 2]) bs = 1;
            if (!bs) continue;
            uint8_t *pp = (k == 0 ? prmV : prmH) + (y * nbx4 + x) * 4; pp[0] = 15; pp[1] = 6; pp[2] = (uint8_t)(bs == 1 ? 0 : 1); pp[3] = (uint8_t)bs;
            if (k == 0) { float *v = ev + nev++ * 8, x0 = x * 4 - 2, y0 = y * 4; v[0] = x0; v[1] = y0; v[2] = x0 + 4; v[3] = y0; v[4] = x0 + 4; v[5] = y0 + 4; v[6] = x0; v[7] = y0 + 4; }
            else { float *v = eh + neh++ * 8, x0 = x * 4, y0 = y * 4 - 2; v[0] = x0; v[1] = y0; v[2] = x0 + 4; v[3] = y0; v[4] = x0 + 4; v[5] = y0 + 4; v[6] = x0; v[7] = y0 + 4; }
        }
    }
    printf("deblock edges with bS>0: %d vertical + %d horizontal of %d possible each\n", nev, neh, nbx4 * nby4);
    glBindTexture(T, tex[6]); NEAREST(); glTexImage2D(T, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(T, tex[7]); NEAREST(); glTexImage2D(T, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(T, tex[8]); NEAREST(); glTexImage2D(T, 0, GL_RGBA8, nbx4, nby4, 0, GL_RGBA, GL_UNSIGNED_BYTE, prmV);
    glBindTexture(T, tex[9]); NEAREST(); glTexImage2D(T, 0, GL_RGBA8, nbx4, nby4, 0, GL_RGBA, GL_UNSIGNED_BYTE, prmH);
    GLuint fbo3, fbo4; glGenFramebuffersEXT(1, &fbo3); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo3); glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, T, tex[6], 0);
    glGenFramebuffersEXT(1, &fbo4); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo4); glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, T, tex[7], 0);
    GLuint fbo2; glGenFramebuffersEXT(1, &fbo2); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo2);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, T, tex[5], 0);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) { fprintf(stderr, "FBO2 incomplete\n"); return 1; }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

    GLhandleARB mcp[9], resp, prp, dbp; char *src = malloc(16384);
    for (i = 0; i < 9; i++) { snprintf(src, 16384, "%s%s", mc_common, mc_body[i]); mcp[i] = linkp(src); }
    resp = linkp(fs_res); prp = linkp(fs_present); dbp = linkp(fs_deblock);

    float *verts[9]; int nq[9] = {0}; for (i = 0; i < 9; i++) verts[i] = malloc(nbx8 * nby8 * 8 * sizeof(float));
    for (y = 0; y < nby8; y++) for (x = 0; x < nbx8; x++) {
        int c = classify(mvx8[y * nbx8 + x], mvy8[y * nbx8 + x]); float *v = verts[c] + nq[c]++ * 8;
        float x0 = x * 8, y0 = y * 8; v[0] = x0; v[1] = y0; v[2] = x0 + 8; v[3] = y0; v[4] = x0 + 8; v[5] = y0 + 8; v[6] = x0; v[7] = y0 + 8;
    }
    printf("MC classes (8x8 blocks):"); for (i = 0; i < 9; i++) printf(" c%d=%d", i, nq[i]); printf("\n");

    #define DRAW_MC() do { \
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo); glViewport(0, 0, W, H); \
        glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[0]); glActiveTexture(GL_TEXTURE1); glBindTexture(T, tex[1]); \
        glEnableClientState(GL_VERTEX_ARRAY); \
        for (i = 0; i < 9; i++) if (nq[i]) { \
            glUseProgramObjectARB(mcp[i]); glUniform1iARB(glGetUniformLocationARB(mcp[i], "refTex"), 0); glUniform1iARB(glGetUniformLocationARB(mcp[i], "mvTex"), 1); \
            glVertexPointer(2, GL_FLOAT, 0, verts[i]); glDrawArrays(GL_QUADS, 0, nq[i] * 4); } \
        glDisableClientState(GL_VERTEX_ARRAY); } while (0)
    #define DRAW_COPY() do { \
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo2); glViewport(0, 0, W, H); \
        glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[3]); glUseProgramObjectARB(prp); glUniform1iARB(glGetUniformLocationARB(prp, "finalTex"), 0); \
        glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(W, 0); glVertex2f(W, H); glVertex2f(0, H); glEnd(); } while (0)
    #define DRAW_RES() do { \
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo2); glViewport(0, 0, W, H); \
        glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[2]); glActiveTexture(GL_TEXTURE1); glBindTexture(T, tex[4]); glActiveTexture(GL_TEXTURE2); glBindTexture(T, tex[3]); \
        glUseProgramObjectARB(resp); glUniform1iARB(glGetUniformLocationARB(resp, "coefTex"), 0); glUniform1iARB(glGetUniformLocationARB(resp, "idxTex"), 1); glUniform1iARB(glGetUniformLocationARB(resp, "predTex"), 2); \
        glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2, GL_FLOAT, 0, cverts); glDrawArrays(GL_QUADS, 0, cq * 4); glDisableClientState(GL_VERTEX_ARRAY); } while (0)
    #define COPYQ() do { glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(W, 0); glVertex2f(W, H); glVertex2f(0, H); glEnd(); } while (0)
    #define DRAW_DEBLOCK() do { \
        /* vertical edges: B(tex5) -> C(tex6) ; horizontal edges: C(tex6) -> D(tex7) */ \
        glViewport(0, 0, W, H); glUseProgramObjectARB(prp); glUniform1iARB(glGetUniformLocationARB(prp, "finalTex"), 0); \
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo3); glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[5]); COPYQ(); \
        glUseProgramObjectARB(dbp); glUniform1iARB(glGetUniformLocationARB(dbp, "srcTex"), 0); glUniform1iARB(glGetUniformLocationARB(dbp, "prmTex"), 1); glUniform1fARB(glGetUniformLocationARB(dbp, "dir"), 0.0f); \
        glActiveTexture(GL_TEXTURE1); glBindTexture(T, tex[8]); glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2, GL_FLOAT, 0, ev); glDrawArrays(GL_QUADS, 0, nev * 4); \
        glUseProgramObjectARB(prp); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo4); glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[6]); glDisableClientState(GL_VERTEX_ARRAY); COPYQ(); \
        glUseProgramObjectARB(dbp); glUniform1fARB(glGetUniformLocationARB(dbp, "dir"), 1.0f); \
        glActiveTexture(GL_TEXTURE1); glBindTexture(T, tex[9]); glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2, GL_FLOAT, 0, eh); glDrawArrays(GL_QUADS, 0, neh * 4); glDisableClientState(GL_VERTEX_ARRAY); } while (0)
    #define DRAW_PRESENT() do { \
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0); glViewport(0, 0, W, H); \
        glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[5]); glUseProgramObjectARB(prp); glUniform1iARB(glGetUniformLocationARB(prp, "finalTex"), 0); \
        glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(W, 0); glVertex2f(W, H); glVertex2f(0, H); glEnd(); } while (0)
    #define UPLOAD() do { \
        glActiveTexture(GL_TEXTURE0); \
        glBindTexture(T, tex[1]); glTexSubImage2D(T, 0, 0, 0, nbx8, nby8, GL_RGBA, GL_FLOAT, mvf); \
        glBindTexture(T, tex[2]); glTexSubImage2D(T, 0, 0, 0, 2048, arows, GL_RGBA, GL_FLOAT, atlas); \
        glBindTexture(T, tex[4]); glTexSubImage2D(T, 0, 0, 0, nbx4, nby4, GL_LUMINANCE, GL_FLOAT, idxf); } while (0)

    /* ---- correctness ---- */
    DRAW_MC(); DRAW_COPY(); DRAW_RES(); DRAW_PRESENT(); glFinish();
    uint8_t *rb = malloc(W * H * 4); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rb);
    long bad = 0, bad2 = 0, maxd = 0; long perclass_bad[9] = {0};
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
        int d = abs((int)rb[(y * W + x) * 4] - (int)cpu_out[y * W + x]);
        if (d) { bad++; if (d > 1) bad2++; if (d > maxd) maxd = d; perclass_bad[classify(mvx8[(y / 8) * nbx8 + x / 8], mvy8[(y / 8) * nbx8 + x / 8])]++; }
    }
    printf("VERIFY vs CPU reference: %ld/%d pixels differ (%.4f%%), %ld differ by >1, max diff %ld\n", bad, W * H, 100.0 * bad / (W * H), bad2, maxd);
    printf("  mismatches by MC class:"); for (i = 0; i < 9; i++) printf(" c%d=%ld", i, perclass_bad[i]); printf("\n");

    /* ---- timing (per-phase with a finish after each, then end-to-end with ONE finish) ---- */
    double tUp = 0, tMC = 0, tRES = 0, tPR = 0, tAll = 0; int it;
    for (it = 0; it < iters; it++) {
        double a = now_ms(); UPLOAD(); glFinish();
        double b = now_ms(); DRAW_MC(); glFinish();
        double c = now_ms(); DRAW_COPY(); DRAW_RES(); glFinish();
        double d = now_ms(); DRAW_PRESENT(); glFinish();
        double e = now_ms(); tUp += b - a; tMC += c - b; tRES += d - c; tPR += e - d;
    }
    for (it = 0; it < iters; it++) { double a = now_ms(); UPLOAD(); DRAW_MC(); DRAW_COPY(); DRAW_RES(); DRAW_PRESENT(); glFinish(); tAll += now_ms() - a; }
    { double tDB = 0, tAll2 = 0;
      for (it = 0; it < iters; it++) { double a = now_ms(); DRAW_DEBLOCK(); glFinish(); tDB += now_ms() - a; }
      for (it = 0; it < iters; it++) { double a = now_ms(); UPLOAD(); DRAW_MC(); DRAW_COPY(); DRAW_RES(); DRAW_DEBLOCK(); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0); glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[7]); glUseProgramObjectARB(prp); glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(W, 0); glVertex2f(W, H); glVertex2f(0, H); glEnd(); glFinish(); tAll2 += now_ms() - a; }
      printf("DEBLOCK (luma, bS<4 stand-in; 2 edge passes + 2 copies): %.2f ms | FULL luma frame incl. deblock + present, single finish: %.2f ms\n", tDB / iters, tAll2 / iters); }
    if (getenv("DBX")) {
        const char *names[3] = {"full filter", "6 samples+prm fetch only (no filter math)", "single fetch passthrough"};
        const char *srcs[3] = {fs_deblock, fs_deblock_fetchonly, fs_deblock_oneFetch}; int v;
        for (v = 0; v < 3; v++) {
            GLhandleARB pg = linkp(srcs[v]); int rr; double a;
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo3); glViewport(0, 0, W, H); glUseProgramObjectARB(pg);
            glUniform1iARB(glGetUniformLocationARB(pg, "srcTex"), 0); glUniform1iARB(glGetUniformLocationARB(pg, "prmTex"), 1); glUniform1fARB(glGetUniformLocationARB(pg, "dir"), 0.0f);
            glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[5]); glActiveTexture(GL_TEXTURE1); glBindTexture(T, tex[8]);
            glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2, GL_FLOAT, 0, ev);
            glDrawArrays(GL_QUADS, 0, nev * 4); glFinish();
            if (v == 2) {   /* where does the time go? CPU submit vs GPU wait, client array vs VBO */
                double t0, t1, t2; int k;
                t0 = now_ms(); for (k = 0; k < 10; k++) glDrawArrays(GL_QUADS, 0, nev * 4); t1 = now_ms(); glFinish(); t2 = now_ms();
                printf("  [client arrays] 10 draws: CPU submit %.2f ms/draw, then GPU wait %.2f ms/draw\n", (t1 - t0) / 10, (t2 - t1) / 10);
                GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb); glBufferData(GL_ARRAY_BUFFER, nev * 8 * sizeof(float), ev, GL_STATIC_DRAW);
                glVertexPointer(2, GL_FLOAT, 0, (void *)0);
                t0 = now_ms(); for (k = 0; k < 10; k++) glDrawArrays(GL_QUADS, 0, nev * 4); t1 = now_ms(); glFinish(); t2 = now_ms();
                printf("  [VBO static]    10 draws: CPU submit %.2f ms/draw, then GPU wait %.2f ms/draw  (total %.2f)\n", (t1 - t0) / 10, (t2 - t1) / 10, (t2 - t0) / 10);
                glBindBuffer(GL_ARRAY_BUFFER, 0); glVertexPointer(2, GL_FLOAT, 0, ev);
            }
            a = now_ms(); for (rr = 0; rr < 10; rr++) { glDrawArrays(GL_QUADS, 0, nev * 4); glFinish(); }
            printf("  deblock V pass, %s: %.2f ms for %d quads (%.2f Mpx) -> %.2f ns/px\n", names[v], (now_ms() - a) / 10, nev, nev * 16 / 1e6, (now_ms() - a) / 10 * 1e6 / (nev * 16));
            glDisableClientState(GL_VERTEX_ARRAY);
        }
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    }
    if (getenv("GEOM")) {
        /* geometry submission experiments on the deblock V-edge set (72k 4x4 regions), constant-colour shader, default framebuffer for exact coverage compare */
        GLhandleARB pg = linkp("void main(){ gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }");
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0); glViewport(0, 0, W, H); glUseProgramObjectARB(pg);
        uint8_t *a1 = malloc(W * H * 4), *a2 = malloc(W * H * 4); int k; long diff = 0; double t0, t1, t2;
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2, GL_FLOAT, 0, ev); glDrawArrays(GL_QUADS, 0, nev * 4); glFinish();
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, a1);
        float *pv = malloc(nev * 2 * sizeof(float)); for (i = 0; i < nev; i++) { pv[i * 2] = ev[i * 8] + 2.0f; pv[i * 2 + 1] = ev[i * 8 + 1] + 2.0f; }
        glClear(GL_COLOR_BUFFER_BIT); glPointSize(4.0f); glDisable(GL_POINT_SMOOTH); glVertexPointer(2, GL_FLOAT, 0, pv); glDrawArrays(GL_POINTS, 0, nev); glFinish();
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, a2);
        for (i = 0; i < W * H; i++) if (a1[i * 4] != a2[i * 4]) diff++;
        printf("GEOM coverage: points(size 4) vs quads: %ld differing pixels of %d\n", diff, W * H);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo3); glViewport(0, 0, W, H);
        #define TIME_DRAW(label, CALL) do { t0 = now_ms(); for (k = 0; k < 10; k++) { CALL; } t1 = now_ms(); glFinish(); t2 = now_ms(); \
            printf("GEOM %-34s CPU submit %.2f ms | GPU wait %.2f ms | total %.2f ms/draw\n", label, (t1 - t0) / 10, (t2 - t1) / 10, (t2 - t0) / 10); } while (0)
        glVertexPointer(2, GL_FLOAT, 0, ev); TIME_DRAW("quads, client arrays", glDrawArrays(GL_QUADS, 0, nev * 4));
        glVertexPointer(2, GL_FLOAT, 0, pv); TIME_DRAW("points(4), client arrays", glDrawArrays(GL_POINTS, 0, nev));
        GLuint vb; glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb); glBufferData(GL_ARRAY_BUFFER, nev * 2 * sizeof(float), pv, GL_STATIC_DRAW); glVertexPointer(2, GL_FLOAT, 0, (void *)0);
        TIME_DRAW("points(4), static VBO", glDrawArrays(GL_POINTS, 0, nev));
        TIME_DRAW("points(4), VBO re-uploaded each draw", (glBufferData(GL_ARRAY_BUFFER, nev * 2 * sizeof(float), pv, GL_DYNAMIC_DRAW), glDrawArrays(GL_POINTS, 0, nev)));
        /* indexed quads from a static full-grid vertex VBO (all 4x4 blocks), dynamic index list */
        { int nall = nbx4 * nby4; float *gv = malloc(nall * 8 * sizeof(float)); unsigned int *ix = malloc(nev * 4 * sizeof(unsigned int)); int n = 0, bxx, byy;
          for (byy = 0; byy < nby4; byy++) for (bxx = 0; bxx < nbx4; bxx++) { float *v = gv + (byy * nbx4 + bxx) * 8, x0 = bxx * 4 - 2, y0 = byy * 4; v[0] = x0; v[1] = y0; v[2] = x0 + 4; v[3] = y0; v[4] = x0 + 4; v[5] = y0 + 4; v[6] = x0; v[7] = y0 + 4; }
          /* which blocks are active: re-derive from prmV alpha channel */
          for (i = 0; i < nall; i++) if (prmV[i * 4 + 3]) { ix[n * 4] = i * 4; ix[n * 4 + 1] = i * 4 + 1; ix[n * 4 + 2] = i * 4 + 2; ix[n * 4 + 3] = i * 4 + 3; n++; }
          glBindBuffer(GL_ARRAY_BUFFER, vb); glBufferData(GL_ARRAY_BUFFER, nall * 8 * sizeof(float), gv, GL_STATIC_DRAW); glVertexPointer(2, GL_FLOAT, 0, (void *)0);
          GLuint ib; glGenBuffers(1, &ib); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
          TIME_DRAW("indexed quads, static grid VBO + dyn IB", (glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * 4 * sizeof(unsigned int), ix, GL_DYNAMIC_DRAW), glDrawElements(GL_QUADS, n * 4, GL_UNSIGNED_INT, (void *)0)));
          printf("GEOM (indexed: %d active of %d)\n", n, nall); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); }
        glBindBuffer(GL_ARRAY_BUFFER, 0); glDisableClientState(GL_VERTEX_ARRAY); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    }
    printf("TIMING (ms/frame, %d iters): upload %.2f | MC %.2f | copy+RES(single pass, coded only) %.2f | present %.2f | sum %.2f | end-to-end single-finish %.2f\n",
           iters, tUp / iters, tMC / iters, tRES / iters, tPR / iters, (tUp + tMC + tRES + tPR) / iters, tAll / iters);
    if (getenv("PROFILE")) {
        /* per MC class cost: draw only that class, finish, average */
        int reps = 10, c;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo); glViewport(0, 0, W, H);
        glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[0]); glActiveTexture(GL_TEXTURE1); glBindTexture(T, tex[1]);
        glEnableClientState(GL_VERTEX_ARRAY);
        printf("MC per-class (ms, px, ns/px):\n");
        for (c = 0; c < 9; c++) if (nq[c]) {
            double a = now_ms(); int r;
            for (r = 0; r < reps; r++) { glUseProgramObjectARB(mcp[c]); glUniform1iARB(glGetUniformLocationARB(mcp[c], "refTex"), 0); glUniform1iARB(glGetUniformLocationARB(mcp[c], "mvTex"), 1);
                glVertexPointer(2, GL_FLOAT, 0, verts[c]); glDrawArrays(GL_QUADS, 0, nq[c] * 4); glFinish(); }
            double ms = (now_ms() - a) / reps; printf("  c%d: %.2f ms, %d px, %.1f ns/px\n", c, ms, nq[c] * 64, ms * 1e6 / (nq[c] * 64));
        }
        glDisableClientState(GL_VERTEX_ARRAY);
        /* floor: empty-ish full-frame copy for fill-rate reference */
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0); glActiveTexture(GL_TEXTURE0); glBindTexture(T, tex[3]); glUseProgramObjectARB(prp); glUniform1iARB(glGetUniformLocationARB(prp, "finalTex"), 0);
        double a = now_ms(); int r; for (r = 0; r < reps; r++) { glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(W, 0); glVertex2f(W, H); glVertex2f(0, H); glEnd(); glFinish(); }
        printf("full-frame copy (fill-rate reference, 2.1 Mpx): %.2f ms\n", (now_ms() - a) / reps);
    }
    aglSetCurrentContext(NULL); aglDestroyContext(ctx);
    return 0;
}
