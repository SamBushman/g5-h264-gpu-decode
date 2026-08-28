/* Item 9 follow-up (2026-08-28): standalone verification for the "merge
 * MC + IDCT into one dispatch" redesign (option 2, full merge including
 * diag), per this project's own established practice - prove the fused
 * shader's MATH is correct in isolation, against the already-proven
 * separate shaders, before touching the live pipeline at all.
 *
 * Method: build several synthetic 4x4 blocks (varied reference pixel
 * patterns + varied DCT coefficients + every real quarter-pel phase, both
 * singlepass and diagonal families), batched multiple-blocks-wide (not
 * just one isolated block - addressing bugs in the blockIdx*4 batching
 * scheme are exactly the kind of thing a single-block test would miss).
 * Run the OLD two-pass path (today's fs_mc_batch_var / fs_diag_stage1+2
 * for prediction, fs_idct_batch for residual, CPU combine: clip255(pred+
 * residual)) and the NEW single-pass merged shader side by side, and
 * diff every output pixel. Byte-exact match confirms the fusion doesn't
 * change the math - only where it's evaluated. */
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void checkgl(const char *w) { GLenum e = glGetError(); if (e) fprintf(stderr, "GL err %s: 0x%lx\n", w, (unsigned long)e); }
static GLhandleARB compile(GLenum t, const char *s) {
    GLhandleARB h = glCreateShaderObjectARB(t);
    glShaderSourceARB(h, 1, &s, NULL); glCompileShaderARB(h);
    GLint ok = 0; glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(h, sizeof log, &n, log); fprintf(stderr, "compile fail:\n%s\n%s\n", log, s); exit(1); }
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
static float clip255f(float v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ---- existing, already-proven shaders (verbatim from gpu_live_decode_test.c) ---- */
static const char *fs_mc_batch_var =
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"
"uniform sampler2DRect colInfoTex;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float halfH(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfV(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"void main(){\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float row = floor(gl_FragCoord.y);\n"
"  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));\n"
"  float blockIdx = colInfo.r;\n"
"  float localX = colInfo.g;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 b = vec2(info.r + localX, info.g + row);\n"
"  float hPhase = info.b, vPhase = info.a;\n"
"  float full00 = texture2DRect(refTex, b).r*255.0;\n"
"  float full10 = texture2DRect(refTex, b+vec2(1.0,0.0)).r*255.0;\n"
"  float full01 = texture2DRect(refTex, b+vec2(0.0,1.0)).r*255.0;\n"
"  float halfH0 = halfH(b);\n"
"  float halfH1 = halfH(b+vec2(0.0,1.0));\n"
"  float halfV0 = halfV(b);\n"
"  float halfV1 = halfV(b+vec2(1.0,0.0));\n"
"  float result = full00;\n"
"  if (hPhase == 0.0 && vPhase == 0.0) {\n"
"    result = full00;\n"
"  } else if (hPhase == 2.0 && vPhase == 0.0) {\n"
"    result = halfH0;\n"
"  } else if (hPhase == 0.0 && vPhase == 2.0) {\n"
"    result = halfV0;\n"
"  } else if (vPhase == 0.0) {\n"
"    float fullOp = full00; if (hPhase == 3.0) fullOp = full10;\n"
"    result = floor((fullOp + halfH0 + 1.0) / 2.0);\n"
"  } else if (hPhase == 0.0) {\n"
"    float fullOp = full00; if (vPhase == 3.0) fullOp = full01;\n"
"    result = floor((fullOp + halfV0 + 1.0) / 2.0);\n"
"  } else {\n"
"    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;\n"
"    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;\n"
"    result = floor((hOp + vOp + 1.0) / 2.0);\n"
"  }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

static const char *fs_idct_batch =
"uniform sampler2DRect coeffTex;\n"
"void main() {\n"
"  vec2 p = floor(gl_FragCoord.xy);\n"
"  float base = floor(p.x / 4.0) * 4.0;\n"
"  float lc = p.x - base;\n"
"  float lr = p.y;\n"
"  float c0  = texture2DRect(coeffTex, vec2(base+0.5, 0.5)).r;\n"
"  float c1  = texture2DRect(coeffTex, vec2(base+1.5, 0.5)).r;\n"
"  float c2  = texture2DRect(coeffTex, vec2(base+2.5, 0.5)).r;\n"
"  float c3  = texture2DRect(coeffTex, vec2(base+3.5, 0.5)).r;\n"
"  float c4  = texture2DRect(coeffTex, vec2(base+0.5, 1.5)).r;\n"
"  float c5  = texture2DRect(coeffTex, vec2(base+1.5, 1.5)).r;\n"
"  float c6  = texture2DRect(coeffTex, vec2(base+2.5, 1.5)).r;\n"
"  float c7  = texture2DRect(coeffTex, vec2(base+3.5, 1.5)).r;\n"
"  float c8  = texture2DRect(coeffTex, vec2(base+0.5, 2.5)).r;\n"
"  float c9  = texture2DRect(coeffTex, vec2(base+1.5, 2.5)).r;\n"
"  float c10 = texture2DRect(coeffTex, vec2(base+2.5, 2.5)).r;\n"
"  float c11 = texture2DRect(coeffTex, vec2(base+3.5, 2.5)).r;\n"
"  float c12 = texture2DRect(coeffTex, vec2(base+0.5, 3.5)).r;\n"
"  float c13 = texture2DRect(coeffTex, vec2(base+1.5, 3.5)).r;\n"
"  float c14 = texture2DRect(coeffTex, vec2(base+2.5, 3.5)).r;\n"
"  float c15 = texture2DRect(coeffTex, vec2(base+3.5, 3.5)).r;\n"
"  float z0, z1, z2, z3;\n"
"  float m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15;\n"
"  z0 = c0 + c8;  z1 = c0 - c8;  z2 = floor(c4/2.0) - c12; z3 = c4 + floor(c12/2.0);\n"
"  m0 = z0+z3; m4 = z1+z2; m8 = z1-z2; m12 = z0-z3;\n"
"  z0 = c1 + c9;  z1 = c1 - c9;  z2 = floor(c5/2.0) - c13; z3 = c5 + floor(c13/2.0);\n"
"  m1 = z0+z3; m5 = z1+z2; m9 = z1-z2; m13 = z0-z3;\n"
"  z0 = c2 + c10; z1 = c2 - c10; z2 = floor(c6/2.0) - c14; z3 = c6 + floor(c14/2.0);\n"
"  m2 = z0+z3; m6 = z1+z2; m10 = z1-z2; m14 = z0-z3;\n"
"  z0 = c3 + c11; z1 = c3 - c11; z2 = floor(c7/2.0) - c15; z3 = c7 + floor(c15/2.0);\n"
"  m3 = z0+z3; m7 = z1+z2; m11 = z1-z2; m15 = z0-z3;\n"
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m2;  z1=m0-m2;  z2=floor(m1/2.0)-m3;   z3=m1+floor(m3/2.0);\n"
"  o0=floor((z0+z3)/64.0); o4=floor((z1+z2)/64.0); o8=floor((z1-z2)/64.0); o12=floor((z0-z3)/64.0);\n"
"  z0=m4+m6;  z1=m4-m6;  z2=floor(m5/2.0)-m7;   z3=m5+floor(m7/2.0);\n"
"  o1=floor((z0+z3)/64.0); o5=floor((z1+z2)/64.0); o9=floor((z1-z2)/64.0); o13=floor((z0-z3)/64.0);\n"
"  z0=m8+m10; z1=m8-m10; z2=floor(m9/2.0)-m11;  z3=m9+floor(m11/2.0);\n"
"  o2=floor((z0+z3)/64.0); o6=floor((z1+z2)/64.0); o10=floor((z1-z2)/64.0); o14=floor((z0-z3)/64.0);\n"
"  z0=m12+m14;z1=m12-m14;z2=floor(m13/2.0)-m15; z3=m13+floor(m15/2.0);\n"
"  o3=floor((z0+z3)/64.0); o7=floor((z1+z2)/64.0); o11=floor((z1-z2)/64.0); o15=floor((z0-z3)/64.0);\n"
"  int idx = int(lc) + int(lr) * 4;\n"
"  float result = o0;\n"
"  if (idx == 1) result = o1; else if (idx == 2) result = o2; else if (idx == 3) result = o3;\n"
"  else if (idx == 4) result = o4; else if (idx == 5) result = o5; else if (idx == 6) result = o6; else if (idx == 7) result = o7;\n"
"  else if (idx == 8) result = o8; else if (idx == 9) result = o9; else if (idx == 10) result = o10; else if (idx == 11) result = o11;\n"
"  else if (idx == 12) result = o12; else if (idx == 13) result = o13; else if (idx == 14) result = o14; else if (idx == 15) result = o15;\n"
"  float biased = result + 32768.0;\n"
"  float hi = floor(biased / 256.0);\n"
"  float lo = biased - hi * 256.0;\n"
"  gl_FragColor = vec4(hi/255.0, lo/255.0, 0.0, 1.0);\n"
"}\n";

/* ---- NEW: merged single-pass shader (singlepass-MC family + IDCT) ----
 * Same per-block 4-wide x 4-tall layout as fs_idct_batch (reuses it
 * unchanged - coeffTex addressing, the whole butterfly, block/local-pixel
 * indexing). blockInfoTex here is PER-BLOCK (one entry per 4x4 luma
 * block), not per-partition like fs_mc_batch_var's - the live-integration
 * step still needs to build this per-block info (today's MC requests are
 * per-partition), but that's an addressing/bookkeeping question, not a
 * math question - this shader proves the MATH side is correct assuming
 * per-block info is available. Output is the FINAL clipped pixel
 * directly (single channel, 0-255) - no intermediate two-byte residual
 * encoding needed, since we're not preserving a wide range for a later
 * CPU decode step anymore. */
static const char *fs_mc_idct_merged =
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"   /* per-BLOCK: (ref_x, ref_y, hPhase, vPhase) */
"uniform sampler2DRect coeffTex;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float halfH(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfV(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"void main() {\n"
"  float p_x = floor(gl_FragCoord.x);\n"
"  float p_y = floor(gl_FragCoord.y);\n"
"  float blockIdx = floor(p_x / 4.0);\n"
"  float lc = p_x - blockIdx * 4.0;\n"
"  float lr = p_y;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx + 0.5, 0.5));\n"
"  vec2 b = vec2(info.r + lc, info.g + lr);\n"
"  float hPhase = info.b, vPhase = info.a;\n"
"  float full00 = texture2DRect(refTex, b).r*255.0;\n"
"  float full10 = texture2DRect(refTex, b+vec2(1.0,0.0)).r*255.0;\n"
"  float full01 = texture2DRect(refTex, b+vec2(0.0,1.0)).r*255.0;\n"
"  float halfH0 = halfH(b);\n"
"  float halfH1 = halfH(b+vec2(0.0,1.0));\n"
"  float halfV0 = halfV(b);\n"
"  float halfV1 = halfV(b+vec2(1.0,0.0));\n"
"  float pred = full00;\n"
"  if (hPhase == 0.0 && vPhase == 0.0) {\n"
"    pred = full00;\n"
"  } else if (hPhase == 2.0 && vPhase == 0.0) {\n"
"    pred = halfH0;\n"
"  } else if (hPhase == 0.0 && vPhase == 2.0) {\n"
"    pred = halfV0;\n"
"  } else if (vPhase == 0.0) {\n"
"    float fullOp = full00; if (hPhase == 3.0) fullOp = full10;\n"
"    pred = floor((fullOp + halfH0 + 1.0) / 2.0);\n"
"  } else if (hPhase == 0.0) {\n"
"    float fullOp = full00; if (vPhase == 3.0) fullOp = full01;\n"
"    pred = floor((fullOp + halfV0 + 1.0) / 2.0);\n"
"  } else {\n"
"    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;\n"
"    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;\n"
"    pred = floor((hOp + vOp + 1.0) / 2.0);\n"
"  }\n"
"  float base = blockIdx * 4.0;\n"
"  float c0  = texture2DRect(coeffTex, vec2(base+0.5, 0.5)).r;\n"
"  float c1  = texture2DRect(coeffTex, vec2(base+1.5, 0.5)).r;\n"
"  float c2  = texture2DRect(coeffTex, vec2(base+2.5, 0.5)).r;\n"
"  float c3  = texture2DRect(coeffTex, vec2(base+3.5, 0.5)).r;\n"
"  float c4  = texture2DRect(coeffTex, vec2(base+0.5, 1.5)).r;\n"
"  float c5  = texture2DRect(coeffTex, vec2(base+1.5, 1.5)).r;\n"
"  float c6  = texture2DRect(coeffTex, vec2(base+2.5, 1.5)).r;\n"
"  float c7  = texture2DRect(coeffTex, vec2(base+3.5, 1.5)).r;\n"
"  float c8  = texture2DRect(coeffTex, vec2(base+0.5, 2.5)).r;\n"
"  float c9  = texture2DRect(coeffTex, vec2(base+1.5, 2.5)).r;\n"
"  float c10 = texture2DRect(coeffTex, vec2(base+2.5, 2.5)).r;\n"
"  float c11 = texture2DRect(coeffTex, vec2(base+3.5, 2.5)).r;\n"
"  float c12 = texture2DRect(coeffTex, vec2(base+0.5, 3.5)).r;\n"
"  float c13 = texture2DRect(coeffTex, vec2(base+1.5, 3.5)).r;\n"
"  float c14 = texture2DRect(coeffTex, vec2(base+2.5, 3.5)).r;\n"
"  float c15 = texture2DRect(coeffTex, vec2(base+3.5, 3.5)).r;\n"
"  float z0, z1, z2, z3;\n"
"  float m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15;\n"
"  z0 = c0 + c8;  z1 = c0 - c8;  z2 = floor(c4/2.0) - c12; z3 = c4 + floor(c12/2.0);\n"
"  m0 = z0+z3; m4 = z1+z2; m8 = z1-z2; m12 = z0-z3;\n"
"  z0 = c1 + c9;  z1 = c1 - c9;  z2 = floor(c5/2.0) - c13; z3 = c5 + floor(c13/2.0);\n"
"  m1 = z0+z3; m5 = z1+z2; m9 = z1-z2; m13 = z0-z3;\n"
"  z0 = c2 + c10; z1 = c2 - c10; z2 = floor(c6/2.0) - c14; z3 = c6 + floor(c14/2.0);\n"
"  m2 = z0+z3; m6 = z1+z2; m10 = z1-z2; m14 = z0-z3;\n"
"  z0 = c3 + c11; z1 = c3 - c11; z2 = floor(c7/2.0) - c15; z3 = c7 + floor(c15/2.0);\n"
"  m3 = z0+z3; m7 = z1+z2; m11 = z1-z2; m15 = z0-z3;\n"
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m2;  z1=m0-m2;  z2=floor(m1/2.0)-m3;   z3=m1+floor(m3/2.0);\n"
"  o0=floor((z0+z3)/64.0); o4=floor((z1+z2)/64.0); o8=floor((z1-z2)/64.0); o12=floor((z0-z3)/64.0);\n"
"  z0=m4+m6;  z1=m4-m6;  z2=floor(m5/2.0)-m7;   z3=m5+floor(m7/2.0);\n"
"  o1=floor((z0+z3)/64.0); o5=floor((z1+z2)/64.0); o9=floor((z1-z2)/64.0); o13=floor((z0-z3)/64.0);\n"
"  z0=m8+m10; z1=m8-m10; z2=floor(m9/2.0)-m11;  z3=m9+floor(m11/2.0);\n"
"  o2=floor((z0+z3)/64.0); o6=floor((z1+z2)/64.0); o10=floor((z1-z2)/64.0); o14=floor((z0-z3)/64.0);\n"
"  z0=m12+m14;z1=m12-m14;z2=floor(m13/2.0)-m15; z3=m13+floor(m15/2.0);\n"
"  o3=floor((z0+z3)/64.0); o7=floor((z1+z2)/64.0); o11=floor((z1-z2)/64.0); o15=floor((z0-z3)/64.0);\n"
"  int idx = int(lc) + int(lr) * 4;\n"
"  float residual = o0;\n"
"  if (idx == 1) residual = o1; else if (idx == 2) residual = o2; else if (idx == 3) residual = o3;\n"
"  else if (idx == 4) residual = o4; else if (idx == 5) residual = o5; else if (idx == 6) residual = o6; else if (idx == 7) residual = o7;\n"
"  else if (idx == 8) residual = o8; else if (idx == 9) residual = o9; else if (idx == 10) residual = o10; else if (idx == 11) residual = o11;\n"
"  else if (idx == 12) residual = o12; else if (idx == 13) residual = o13; else if (idx == 14) residual = o14; else if (idx == 15) residual = o15;\n"
"  float finalPix = clip255(pred + residual);\n"
"  gl_FragColor = vec4(finalPix/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

#define NBLOCKS 12
#define REFW 64
#define REFH 64

/* Test cases: every real SINGLEPASS-family (h,v) phase combination
 * (excludes (2,2),(2,1),(2,3),(1,2),(3,2) - the diagonal family, tested
 * separately). Distinct ref offsets per block so addressing bugs (using
 * the wrong block's info) show up as a mismatch, not a coincidental
 * match. */
static const int test_hphase[NBLOCKS] = {0,2,0,1,3,0,0,1,1,3,3,0};
static const int test_vphase[NBLOCKS] = {0,0,2,0,0,1,3,1,3,1,3,0};
static const int test_refx[NBLOCKS]   = {10,15,20,25,30,12,18,22,28,14,26,8};
static const int test_refy[NBLOCKS]   = {10,12,14,16,18,20,22,24,26,28,30,32};

int main(void) {
    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8,
                        AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    AGLContext ctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    AGLPbuffer pbuf; aglCreatePBuffer(1024, 16, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(ctx, pbuf, 0, 0, 0); aglSetCurrentContext(ctx);

    /* Synthetic reference image - a pattern varied enough that different
     * sample offsets/phases produce genuinely different filtered values
     * (not a flat/constant image, which would hide addressing bugs). */
    static unsigned char refimg[REFH][REFW];
    for (int y = 0; y < REFH; y++)
        for (int x = 0; x < REFW; x++)
            refimg[y][x] = (unsigned char)((x * 7 + y * 13 + (x*y) % 23) % 256);

    GLuint refTex;
    glGenTextures(1, &refTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_LUMINANCE8, REFW, REFH, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, refimg);

    /* Synthetic DCT coefficients, one 4x4 set per block - varied
     * magnitudes including negative values and some large enough to
     * produce a clipped-at-0-or-255 final pixel (exercises clip255). */
    static int coeffs[NBLOCKS][16];
    for (int b = 0; b < NBLOCKS; b++)
        for (int i = 0; i < 16; i++)
            coeffs[b][i] = ((b * 31 + i * 17) % 61) - 30 + (b == 3 ? 400 : 0) - (b == 7 ? 400 : 0);

    /* GL_RGBA_FLOAT32_ATI, 4 floats/texel (r=coeff, g/b unused, a=1) -
     * matches gpu_idct_batch's real upload exactly. NOT GL_LUMINANCE_
     * FLOAT32_ATI - this project already found (and documented) that this
     * driver's GL_LUMINANCE_FLOAT32_ATI implementation doesn't preserve
     * negative values correctly, which DCT coefficients routinely are. */
    GLuint coeffTex;
    { static float coefftexdata[4 * NBLOCKS*4 * 4];
      for (int b = 0; b < NBLOCKS; b++)
          for (int row = 0; row < 4; row++)
              for (int col = 0; col < 4; col++) {
                  int texel = row * (NBLOCKS*4) + (b*4+col);
                  coefftexdata[texel*4+0] = (float)coeffs[b][row*4+col];
                  coefftexdata[texel*4+1] = coefftexdata[texel*4+2] = 0.0f;
                  coefftexdata[texel*4+3] = 1.0f;
              }
      glGenTextures(1, &coeffTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, coeffTex);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, NBLOCKS*4, 4, 0, GL_RGBA, GL_FLOAT, coefftexdata);
    }
    checkgl("coeffTex upload");

    /* Old-path blockInfoTex (per-partition == per-block here since each
     * test case IS one 4x4 block) and colInfoTex (identity: column c ->
     * block c, localX 0..3). */
    GLuint blockInfoTexOld, colInfoTex;
    { float bi[NBLOCKS][4];
      for (int b = 0; b < NBLOCKS; b++) {
          bi[b][0] = (float)test_refx[b]; bi[b][1] = (float)test_refy[b];
          bi[b][2] = (float)test_hphase[b]; bi[b][3] = (float)test_vphase[b];
      }
      glGenTextures(1, &blockInfoTexOld); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexOld);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, NBLOCKS, 1, 0, GL_RGBA, GL_FLOAT, bi);
    }
    { float ci[NBLOCKS*4][4];
      for (int b = 0; b < NBLOCKS; b++)
          for (int lx = 0; lx < 4; lx++) {
              ci[b*4+lx][0] = (float)b; ci[b*4+lx][1] = (float)lx; ci[b*4+lx][2] = 0; ci[b*4+lx][3] = 0;
          }
      glGenTextures(1, &colInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, NBLOCKS*4, 1, 0, GL_RGBA, GL_FLOAT, ci);
    }
    checkgl("old-path lookup textures");

    /* ---- Old path: MC pass ---- */
    GLhandleARB progMC = linkp(vs_plain, fs_mc_batch_var);
    static unsigned char predpix[NBLOCKS*4][4];
    {
        glUseProgramObjectARB(progMC);
        glViewport(0, 0, NBLOCKS*4, 4);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, NBLOCKS*4, 0, 4, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
        glUniform1iARB(glGetUniformLocationARB(progMC, "refTex"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexOld);
        glUniform1iARB(glGetUniformLocationARB(progMC, "blockInfoTex"), 1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
        glUniform1iARB(glGetUniformLocationARB(progMC, "colInfoTex"), 2);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(NBLOCKS*4,0); glVertex2f(NBLOCKS*4,4); glVertex2f(0,4); glEnd();
        checkgl("old MC draw");
        static unsigned char rgba[NBLOCKS*4*4*4];
        glReadPixels(0,0,NBLOCKS*4,4,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        for (int y = 0; y < 4; y++) for (int x = 0; x < NBLOCKS*4; x++) predpix[x][y] = rgba[(y*NBLOCKS*4+x)*4];
    }

    /* ---- Old path: IDCT pass ---- */
    GLhandleARB progIDCT = linkp(vs_plain, fs_idct_batch);
    static int residual[NBLOCKS*4][4];
    {
        glUseProgramObjectARB(progIDCT);
        glViewport(0, 0, NBLOCKS*4, 4);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, NBLOCKS*4, 0, 4, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, coeffTex);
        glUniform1iARB(glGetUniformLocationARB(progIDCT, "coeffTex"), 0);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(NBLOCKS*4,0); glVertex2f(NBLOCKS*4,4); glVertex2f(0,4); glEnd();
        checkgl("old IDCT draw");
        static unsigned char rgba[NBLOCKS*4*4*4];
        glReadPixels(0,0,NBLOCKS*4,4,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        for (int y = 0; y < 4; y++) for (int x = 0; x < NBLOCKS*4; x++) {
            int hi = rgba[(y*NBLOCKS*4+x)*4+0], lo = rgba[(y*NBLOCKS*4+x)*4+1];
            residual[x][y] = (hi*256+lo) - 32768;
        }
    }

    /* ---- Old path: CPU combine ---- */
    static unsigned char oldFinal[NBLOCKS*4][4];
    for (int x = 0; x < NBLOCKS*4; x++)
        for (int y = 0; y < 4; y++)
            oldFinal[x][y] = (unsigned char)clip255f((float)predpix[x][y] + (float)residual[x][y]);

    /* ---- New path: merged single-pass shader ---- */
    GLhandleARB progMerged = linkp(vs_plain, fs_mc_idct_merged);
    static unsigned char newFinal[NBLOCKS*4][4];
    {
        glUseProgramObjectARB(progMerged);
        glViewport(0, 0, NBLOCKS*4, 4);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, NBLOCKS*4, 0, 4, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
        glUniform1iARB(glGetUniformLocationARB(progMerged, "refTex"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTexOld); /* per-block, same layout needed */
        glUniform1iARB(glGetUniformLocationARB(progMerged, "blockInfoTex"), 1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, coeffTex);
        glUniform1iARB(glGetUniformLocationARB(progMerged, "coeffTex"), 2);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(NBLOCKS*4,0); glVertex2f(NBLOCKS*4,4); glVertex2f(0,4); glEnd();
        checkgl("merged draw");
        static unsigned char rgba[NBLOCKS*4*4*4];
        glReadPixels(0,0,NBLOCKS*4,4,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        for (int y = 0; y < 4; y++) for (int x = 0; x < NBLOCKS*4; x++) newFinal[x][y] = rgba[(y*NBLOCKS*4+x)*4];
    }

    /* ---- Compare ---- */
    int mism = 0, worst = 0;
    for (int b = 0; b < NBLOCKS; b++) {
        int blockmism = 0, blockworst = 0;
        for (int lx = 0; lx < 4; lx++) for (int ly = 0; ly < 4; ly++) {
            int x = b*4+lx;
            int d = abs((int)oldFinal[x][ly] - (int)newFinal[x][ly]);
            if (d > 0) { blockmism++; mism++; }
            if (d > blockworst) blockworst = d;
            if (d > worst) worst = d;
        }
        printf("block %2d (h=%d v=%d refx=%d refy=%d): %d/16 mismatch, worst diff %d\n",
               b, test_hphase[b], test_vphase[b], test_refx[b], test_refy[b], blockmism, blockworst);
    }
    printf("\nTOTAL: %d/%d pixels differ, worst diff %d\n", mism, NBLOCKS*16, worst);
    printf(mism == 0 ? "RESULT: MERGED SHADER MATCHES OLD TWO-PASS EXACTLY\n" : "RESULT: MISMATCH FOUND\n");

    aglSetCurrentContext(NULL);
    aglDestroyContext(ctx);
    return mism != 0;
}
